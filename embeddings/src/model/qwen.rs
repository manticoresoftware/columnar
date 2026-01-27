use super::TextModel;
use crate::utils::{
    chunk_input_tokens, get_hidden_size, get_max_input_length, get_mean_vector, normalize,
};
use crate::LibError;
use candle_core::{DType, Device, Tensor};
use candle_nn::VarBuilder;
use candle_transformers::models::qwen2::{Config, Model};
use hf_hub::{api::sync::ApiBuilder, Repo, RepoType};
use serde_json::Value;
use std::collections::HashSet;
use std::error::Error;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use tokenizers::Tokenizer;

struct QwenModelFiles {
    config_path: PathBuf,
    tokenizer_path: PathBuf,
    weight_paths: Vec<PathBuf>,
}

pub fn is_qwen_model(model_id: &str, cache_path: &Path) -> Result<bool, Box<dyn Error>> {
    let repo = Repo::with_revision(model_id.to_string(), RepoType::Model, "main".to_string());
    let api = ApiBuilder::new()
        .with_cache_dir(cache_path.to_path_buf())
        .build()
        .map_err(|_| LibError::HuggingFaceApiBuildFailed)?;
    let api = api.repo(repo);
    let config_path = api
        .get("config.json")
        .map_err(|_| LibError::ModelConfigFetchFailed)?;
    let config =
        std::fs::read_to_string(config_path).map_err(|_| LibError::ModelConfigReadFailed)?;
    Ok(is_qwen_config(&config))
}

fn is_qwen_config(config: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(config) else {
        return false;
    };
    matches!(
        value.get("model_type").and_then(Value::as_str),
        Some("qwen2") | Some("qwen3")
    )
}

fn fetch_qwen_files(model_id: &str, cache_path: PathBuf) -> Result<QwenModelFiles, Box<dyn Error>> {
    let repo = Repo::with_revision(model_id.to_string(), RepoType::Model, "main".to_string());
    let api = ApiBuilder::new()
        .with_cache_dir(cache_path)
        .build()
        .map_err(|_| LibError::HuggingFaceApiBuildFailed)?;
    let api = api.repo(repo);

    let config_path = api
        .get("config.json")
        .map_err(|_| LibError::ModelConfigFetchFailed)?;
    let tokenizer_path = api
        .get("tokenizer.json")
        .map_err(|_| LibError::ModelTokenizerFetchFailed)?;

    let weight_paths = match api.get("model.safetensors.index.json") {
        Ok(index_path) => {
            let contents = std::fs::read_to_string(index_path)
                .map_err(|_| LibError::ModelWeightsFetchFailed)?;
            let json: Value =
                serde_json::from_str(&contents).map_err(|_| LibError::ModelWeightsFetchFailed)?;
            let weight_map = json
                .get("weight_map")
                .and_then(Value::as_object)
                .ok_or(LibError::ModelWeightsFetchFailed)?;
            let mut unique_files: HashSet<String> = HashSet::new();
            for value in weight_map.values() {
                if let Some(name) = value.as_str() {
                    unique_files.insert(name.to_string());
                }
            }
            let mut paths = Vec::with_capacity(unique_files.len());
            for file in unique_files {
                let path = api
                    .get(&file)
                    .map_err(|_| LibError::ModelWeightsFetchFailed)?;
                paths.push(path);
            }
            if paths.is_empty() {
                return Err(Box::new(LibError::ModelWeightsFetchFailed));
            }
            paths
        }
        Err(_) => {
            vec![api
                .get("model.safetensors")
                .map_err(|_| LibError::ModelWeightsFetchFailed)?]
        }
    };

    Ok(QwenModelFiles {
        config_path,
        tokenizer_path,
        weight_paths,
    })
}

fn dtype_for_device(device: &Device) -> DType {
    if device.is_cuda() {
        DType::BF16
    } else {
        DType::F32
    }
}

fn load_tokenizer(path: &Path) -> Result<Tokenizer, Box<dyn Error>> {
    match Tokenizer::from_file(path) {
        Ok(tok) => Ok(tok),
        Err(_) => {
            let contents =
                std::fs::read_to_string(path).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
            let mut value: Value =
                serde_json::from_str(&contents).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
            if let Some(model) = value.get_mut("model").and_then(Value::as_object_mut) {
                model.remove("ignore_merges");
                if let Some(merges) = model.get_mut("merges") {
                    if let Some(items) = merges.as_array() {
                        let mut updated = Vec::with_capacity(items.len());
                        let mut converted = false;
                        for item in items {
                            match item {
                                Value::String(s) => updated.push(Value::String(s.clone())),
                                Value::Array(parts) if parts.len() == 2 => {
                                    let a = parts[0].as_str().unwrap_or_default();
                                    let b = parts[1].as_str().unwrap_or_default();
                                    updated.push(Value::String(format!("{a} {b}")));
                                    converted = true;
                                }
                                _ => updated.push(item.clone()),
                            }
                        }
                        if converted {
                            *merges = Value::Array(updated);
                        }
                    }
                }
            }
            let sanitized =
                serde_json::to_vec(&value).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
            Ok(
                Tokenizer::from_bytes(&sanitized)
                    .map_err(|_| LibError::ModelTokenizerLoadFailed)?,
            )
        }
    }
}

pub struct QwenModel {
    model: Mutex<Model>,
    tokenizer: Tokenizer,
    max_input_len: usize,
    hidden_size: usize,
    device: Device,
}

impl QwenModel {
    pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };

        let model_files = fetch_qwen_files(model_id, cache_path)?;
        let config_raw = std::fs::read_to_string(&model_files.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        if !is_qwen_config(&config_raw) {
            return Err(Box::new(LibError::ModelConfigParseFailed));
        }

        let max_input_len =
            get_max_input_length(&config_raw).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
        let hidden_size =
            get_hidden_size(&config_raw).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;

        let mut tokenizer = load_tokenizer(&model_files.tokenizer_path)?;
        let tokenizer = tokenizer
            .with_padding(None)
            .with_truncation(None)
            .map_err(|_| LibError::ModelTokenizerConfigurationFailed)?;

        let dtype = dtype_for_device(&device);
        let config: Config =
            serde_json::from_str(&config_raw).map_err(|_| LibError::ModelConfigParseFailed)?;
        let vb = unsafe {
            VarBuilder::from_mmaped_safetensors(&model_files.weight_paths, dtype, &device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        };
        let model = Model::new(&config, vb).map_err(|_| LibError::ModelLoadFailed)?;

        Ok(Self {
            model: Mutex::new(model),
            tokenizer: tokenizer.clone().into(),
            max_input_len,
            hidden_size,
            device,
        })
    }
}

impl TextModel for QwenModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
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
                let token_ids = Tensor::new(vec![chunk.clone()], &self.device)?;
                let attn_mask = Tensor::new(vec![vec![1u32; chunk.len()]], &self.device)?;
                let mut model = self.model.lock().map_err(|_| LibError::ModelLoadFailed)?;
                let embeddings = model.forward(&token_ids, 0, Some(&attn_mask))?;
                let (_n_sentences, n_tokens, _hidden_size) = embeddings.dims3()?;
                let embeddings = (embeddings.sum(1)? / (n_tokens as f64))?.to_dtype(DType::F32)?;

                let e_j = embeddings.get(0)?;
                let mut emb: Vec<f32> = e_j.to_vec1()?;
                normalize(&mut emb);
                results.push(emb);
            }

            if results.is_empty() {
                return Err(Box::new(LibError::ModelLoadFailed));
            }

            let mean_vector = get_mean_vector(&results);
            all_results.push(mean_vector);
        }

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
