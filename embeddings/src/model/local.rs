use super::TextModel;
use crate::LibError;
use candle_transformers::models::bert::{BertModel, Config, HiddenAct, DTYPE};
use anyhow::Result;
use candle_core::{Device, Tensor};
use candle_nn::VarBuilder;
use hf_hub::{api::sync::ApiBuilder, Repo, RepoType};
use tokenizers::{DecoderWrapper, ModelWrapper, NormalizerWrapper, PostProcessorWrapper, PreTokenizerWrapper, Tokenizer, TokenizerImpl};
use std::path::PathBuf;

use crate::utils::{get_max_input_length, get_hidden_size, normalize, chunk_input_tokens, get_mean_vector};

struct ModelInfo {
	config_path: PathBuf,
	tokenizer_path: PathBuf,
	weights_path: PathBuf,
	use_pth: bool,
}

fn build_model_info(
	cache_path: PathBuf,
	model_id: &str,
	revision: &str,
	use_pth: bool,
) -> Result<ModelInfo> {
	let repo = Repo::with_revision(model_id.to_string(), RepoType::Model, revision.to_string());
	let api = ApiBuilder::new().with_cache_dir(cache_path).build().map_err(|_| LibError::HuggingFaceApiBuildFailed)?;
	let api = api.repo(repo);

	let config_path = api.get("config.json").map_err(|_| LibError::ModelConfigFetchFailed)?;
	let tokenizer_path = api.get("tokenizer.json").map_err(|_| LibError::ModelTokenizerFetchFailed)?;
	let weights_path = if use_pth {
		api.get("pytorch_model.bin").map_err(|_| LibError::ModelWeightsFetchFailed)?
	} else {
		api.get("model.safetensors").map_err(|_| LibError::ModelWeightsFetchFailed)?
	};
	Ok(ModelInfo{
		config_path,
		tokenizer_path,
		weights_path,
		use_pth,
	})
}

fn build_model_and_tokenizer(
	model: ModelInfo,
	device: Device,
) -> Result<(BertModel, Tokenizer, usize, usize)> {
	let config = std::fs::read_to_string(model.config_path).map_err(|_| LibError::ModelConfigReadFailed)?;
	let max_input_len = get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
	let hidden_size = get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;

	let mut config: Config = serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
	let tokenizer: Tokenizer = Tokenizer::from_file(model.tokenizer_path).map_err(|_| LibError::ModelTokenizerLoadFailed)?;

	let vb = if model.use_pth {
		VarBuilder::from_pth(&model.weights_path, DTYPE, &device).map_err(|_| LibError::ModelWeightsLoadFailed)?
	} else {
		unsafe { VarBuilder::from_mmaped_safetensors(&[model.weights_path], DTYPE, &device).map_err(|_| LibError::ModelWeightsLoadFailed)? }
	};
	config.hidden_act = HiddenAct::GeluApproximate;

	let model = BertModel::load(vb, &config).map_err(|_| LibError::ModelLoadFailed)?;
	Ok((model, tokenizer, max_input_len, hidden_size))
}

pub struct LocalModel {
	model: BertModel,
	tokenizer: Tokenizer,
	max_input_len: usize,
	hidden_size: usize,
}

impl LocalModel {
	pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn std::error::Error>> {
		let revision = "main";
		let use_pth = false;
		let device = if use_gpu {
			Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
		} else {
			Device::Cpu
		};

		let model_info = build_model_info(cache_path, model_id, revision, use_pth)?;
		let (model, mut tokenizer, max_input_len, hidden_size) =
		build_model_and_tokenizer(model_info, device)?;
		let tokenizer = tokenizer
			.with_padding(None)
			.with_truncation(None)
			.map_err(|_| LibError::ModelTokenizerConfigurationFailed)?;

		Ok(Self {
			model,
			tokenizer: tokenizer.clone().into(),
			max_input_len,
			hidden_size,
		})
	}
}

impl serde::Serialize for LocalModel {
	fn serialize<S>(&self, serializer: S) -> std::prelude::v1::Result<S::Ok, S::Error> where
		S: serde::Serializer, {
		<TokenizerImpl<ModelWrapper, NormalizerWrapper, PreTokenizerWrapper, PostProcessorWrapper, DecoderWrapper> as serde::Serialize>::serialize(&self.tokenizer, serializer)
	}
}

impl TextModel for LocalModel {
	fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn std::error::Error>> {
		let device = &self.model.device;
		let mut all_results: Vec<Vec<f32>> = Vec::new();
		for text in texts.iter() {
			let tokens = self.tokenizer
				.encode(*text, true)
				.map_err(|_| LibError::ModelTokenizerEncodeFailed)?
				.get_ids()
				.to_vec();
			let chunks = chunk_input_tokens(&tokens, self.max_input_len, (self.max_input_len / 10) as usize);
			let mut results: Vec<Vec<f32>> = Vec::new();
			for chunk in chunks.iter() {
				let token_ids = Tensor::new(&chunk[..], device)?.unsqueeze(0)?;
				let token_type_ids = token_ids.zeros_like()?;
				let embeddings = self.model.forward(&token_ids, &token_type_ids)?;

				let (n_sentences, n_tokens, _hidden_size) = embeddings.dims3()?;
				let embeddings = (embeddings.sum(1)? / (n_tokens as f64))?;

				for j in 0..n_sentences {
					let e_j = embeddings.get(j)?;
					let mut emb: Vec<f32> = e_j.to_vec1()?;
					normalize(&mut emb);
					results.push(emb);
					break;
				}
			}
			all_results.push(get_mean_vector(&results));
		}

		Ok(all_results)
	}

	fn get_hidden_size(&self) -> usize {
		self.hidden_size
	}

	fn get_max_input_len(&self) -> usize {
		self.max_input_len
	}
}


#[cfg(test)]
mod tests {
	use super::*;
	use approx::assert_abs_diff_eq;

	fn check_embedding_properties(embedding: &[f32], expected_len: usize) {
		assert_eq!(embedding.len(), expected_len);

		// Check if the embedding is normalized (L2 norm should be close to 1)
		let norm: f32 = embedding.iter().map(|&x| x * x).sum::<f32>().sqrt();
		assert_abs_diff_eq!(norm, 1.0, epsilon = 1e-6);
	}

	#[test]
	fn test_all_minilm_l6_v2() {
		let model_id = "sentence-transformers/all-MiniLM-L6-v2";
		let cache_path = PathBuf::from(".cache/manticore");
		let local_model = LocalModel::new(model_id, cache_path, false);

		let test_sentences = [
			"This is a test sentence.",
			"Another sentence to encode.",
			"Sentence transformers are awesome!",
		];

		for sentence in &test_sentences {
			let embedding = local_model.predict(sentence);
			check_embedding_properties(&embedding, local_model.get_hidden_size());
		}
	}

	#[test]
	fn test_embedding_consistency() {
		let model_id = "sentence-transformers/all-MiniLM-L6-v2";
		let cache_path = PathBuf::from(".cache/manticore");
		let local_model = LocalModel::new(model_id, cache_path, false);

		let sentence = "This is a test sentence.";
		let embedding1 = local_model.predict(sentence);
		let embedding2 = local_model.predict(sentence);

		for (e1, e2) in embedding1.iter().zip(embedding2.iter()) {
			assert_abs_diff_eq!(e1, e2, epsilon = 1e-6);
		}
	}

	#[test]
	fn test_hidden_size() {
		let model_id = "sentence-transformers/all-MiniLM-L6-v2";
		let cache_path = PathBuf::from(".cache/manticore");
		let local_model = LocalModel::new(model_id, cache_path, false);
		assert_eq!(local_model.get_hidden_size(), 384);
	}

	#[test]
	fn test_max_input_len() {
		let model_id = "sentence-transformers/all-MiniLM-L6-v2";
		let cache_path = PathBuf::from(".cache/manticore");
		let local_model = LocalModel::new(model_id, cache_path, false);
		assert_eq!(local_model.get_max_input_len(), 512);
	}
}
