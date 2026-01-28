use super::TextModel;
use crate::LibError;
use anyhow::Result;
use candle_core::{Device, Tensor};
use candle_nn::VarBuilder;
use candle_transformers::models::bert::{BertModel, Config, HiddenAct, DTYPE};
use hf_hub::{api::sync::ApiBuilder, Repo, RepoType};
use std::path::PathBuf;
use tokenizers::{
    models::bpe::BPE, normalizers, pre_tokenizers::byte_level::ByteLevel,
    processors::roberta::RobertaProcessing, DecoderWrapper, ModelWrapper, NormalizerWrapper,
    PostProcessorWrapper, PreTokenizerWrapper, Tokenizer, TokenizerImpl,
};

use crate::utils::{
    chunk_input_tokens, get_hidden_size, get_max_input_length, get_mean_vector, normalize,
};

#[derive(Debug)]
pub struct ModelInfo {
    config_path: PathBuf,
    tokenizer_path: Option<PathBuf>,
    vocab_path: Option<PathBuf>,
    merges_path: Option<PathBuf>,
    weights_path: PathBuf,
    use_pth: bool,
}

pub fn build_model_info(
    cache_path: PathBuf,
    model_id: &str,
    revision: &str,
    use_pth: bool,
) -> Result<ModelInfo> {
    let repo = Repo::with_revision(model_id.to_string(), RepoType::Model, revision.to_string());
    let api = ApiBuilder::new()
        .with_cache_dir(cache_path)
        .build()
        .map_err(|_| LibError::HuggingFaceApiBuildFailed)?;
    let api = api.repo(repo);

    let config_path = api
        .get("config.json")
        .map_err(|_| LibError::ModelConfigFetchFailed)?;
    let tokenizer_path = api.get("tokenizer.json").ok();
    let vocab_path = api.get("vocab.json").ok();
    let merges_path = api.get("merges.txt").ok();
    if tokenizer_path.is_none() && (vocab_path.is_none() || merges_path.is_none()) {
        return Err(LibError::ModelTokenizerFetchFailed.into());
    }
    let weights_path = if use_pth {
        api.get("pytorch_model.bin")
            .map_err(|_| LibError::ModelWeightsFetchFailed)?
    } else {
        api.get("model.safetensors")
            .map_err(|_| LibError::ModelWeightsFetchFailed)?
    };
    Ok(ModelInfo {
        config_path,
        tokenizer_path,
        vocab_path,
        merges_path,
        weights_path,
        use_pth,
    })
}

fn load_tokenizer(model: &ModelInfo) -> Result<Tokenizer> {
    if let Some(path) = &model.tokenizer_path {
        return Tokenizer::from_file(path).map_err(|_| LibError::ModelTokenizerLoadFailed.into());
    }

    let vocab_path = model
        .vocab_path
        .as_ref()
        .ok_or(LibError::ModelTokenizerLoadFailed)?;
    let merges_path = model
        .merges_path
        .as_ref()
        .ok_or(LibError::ModelTokenizerLoadFailed)?;

    let bpe = BPE::from_file(
        vocab_path
            .to_str()
            .ok_or(LibError::ModelTokenizerLoadFailed)?,
        merges_path
            .to_str()
            .ok_or(LibError::ModelTokenizerLoadFailed)?,
    )
    .unk_token("<unk>".to_string())
    .build()
    .map_err(|_| LibError::ModelTokenizerLoadFailed)?;

    let mut tokenizer = Tokenizer::new(bpe);
    tokenizer.with_pre_tokenizer(ByteLevel::default());

    let post_processor = RobertaProcessing::new(("</s>".to_string(), 2), ("<s>".to_string(), 0))
        .trim_offsets(false)
        .add_prefix_space(true);
    tokenizer.with_post_processor(post_processor);

    let normalizer = normalizers::Sequence::new(vec![normalizers::Strip::new(true, true).into()]);
    tokenizer.with_normalizer(normalizer);

    Ok(tokenizer)
}

fn build_model_and_tokenizer(
    model: ModelInfo,
    device: Device,
) -> Result<(BertModel, Tokenizer, usize, usize)> {
    let config =
        std::fs::read_to_string(&model.config_path).map_err(|_| LibError::ModelConfigReadFailed)?;
    let max_input_len =
        get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
    let hidden_size = get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;

    let mut config: Config =
        serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
    let tokenizer = load_tokenizer(&model)?;

    let vb = if model.use_pth {
        VarBuilder::from_pth(&model.weights_path, DTYPE, &device)
            .map_err(|_| LibError::ModelWeightsLoadFailed)?
    } else {
        unsafe {
            VarBuilder::from_mmaped_safetensors(&[model.weights_path], DTYPE, &device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        }
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
    pub fn new(
        model_id: &str,
        cache_path: PathBuf,
        use_gpu: bool,
    ) -> Result<Self, Box<dyn std::error::Error>> {
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
    fn serialize<S>(&self, serializer: S) -> std::prelude::v1::Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        <TokenizerImpl<
            ModelWrapper,
            NormalizerWrapper,
            PreTokenizerWrapper,
            PostProcessorWrapper,
            DecoderWrapper,
        > as serde::Serialize>::serialize(&self.tokenizer, serializer)
    }
}

impl TextModel for LocalModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn std::error::Error>> {
        let device = &self.model.device;
        let mut all_results: Vec<Vec<f32>> = Vec::new();
        for text in texts.iter() {
            let tokens = self
                .tokenizer
                .encode(*text, true)
                .map_err(|_| LibError::ModelTokenizerEncodeFailed)?
                .get_ids()
                .to_vec();
            let chunks = chunk_input_tokens(&tokens, self.max_input_len, self.max_input_len / 10);
            let mut results: Vec<Vec<f32>> = Vec::new();
            for chunk in chunks.iter() {
                let token_ids = Tensor::new(&chunk[..], device)?.unsqueeze(0)?;
                let token_type_ids = token_ids.zeros_like()?;
                let embeddings = self.model.forward(&token_ids, &token_type_ids, None)?;

                let (n_sentences, n_tokens, _hidden_size) = embeddings.dims3()?;
                let embeddings = (embeddings.sum(1)? / (n_tokens as f64))?;

                if let Some(j) = (0..n_sentences).next() {
                    let e_j = embeddings.get(j)?;
                    let mut emb: Vec<f32> = e_j.to_vec1()?;
                    normalize(&mut emb);
                    results.push(emb);
                }
            }

            // Validate that we have results before computing mean - this should never happen for local models
            if results.is_empty() {
                return Err(Box::new(LibError::ModelLoadFailed));
            }

            let mean_vector = get_mean_vector(&results);
            all_results.push(mean_vector);
        }

        // Final validation - ensure we have embeddings for all input texts
        if all_results.is_empty() || all_results.len() != texts.len() {
            return Err(Box::new(LibError::ModelLoadFailed));
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
        let _local_model = LocalModel::new(model_id, cache_path.clone(), false);

        let test_sentences = [
            "This is a test sentence.",
            "Another sentence to encode.",
            "Sentence transformers are awesome!",
        ];

        for sentence in &test_sentences {
            let local_model = LocalModel::new(model_id, cache_path.clone(), false).unwrap();
            let embedding = local_model.predict(&[sentence]).unwrap();
            check_embedding_properties(&embedding[0], local_model.get_hidden_size());
        }
    }

    #[test]
    fn test_embedding_consistency() {
        let model_id = "sentence-transformers/all-MiniLM-L6-v2";
        let cache_path = PathBuf::from(".cache/manticore");
        let local_model = LocalModel::new(model_id, cache_path, false).unwrap();

        let sentence = &["This is a test sentence."];
        let embedding1 = local_model.predict(sentence).unwrap();
        let embedding2 = local_model.predict(sentence).unwrap();

        for (e1, e2) in embedding1[0].iter().zip(embedding2[0].iter()) {
            assert_abs_diff_eq!(e1, e2, epsilon = 1e-6);
        }
    }

    #[test]
    fn test_hidden_size() {
        let model_id = "sentence-transformers/all-MiniLM-L6-v2";
        let cache_path = PathBuf::from(".cache/manticore");
        let local_model = LocalModel::new(model_id, cache_path, false).unwrap();
        assert_eq!(local_model.get_hidden_size(), 384);
    }

    #[test]
    fn test_max_input_len() {
        let model_id = "sentence-transformers/all-MiniLM-L6-v2";
        let cache_path = PathBuf::from(".cache/manticore");
        let local_model = LocalModel::new(model_id, cache_path, false).unwrap();
        assert_eq!(local_model.get_max_input_len(), 512);
    }
}
