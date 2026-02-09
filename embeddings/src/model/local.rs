use super::TextModel;
use crate::error::LibError;
use crate::utils::{
    chunk_input_tokens, get_hidden_size, get_max_input_length, get_mean_vector, normalize,
};
use candle_core::{DType, Device, Tensor};
use candle_nn::VarBuilder;
use candle_transformers::models::bert::{
    BertModel, Config as BertConfig, HiddenAct, DTYPE as BERT_DTYPE,
};
use candle_transformers::models::gemma::{Config as GemmaConfig, Model as GemmaModel};
use candle_transformers::models::llama::{
    Cache as LlamaCache, Config as LlamaConfig, Llama as LlamaModel,
    LlamaConfig as LlamaConfigSerde,
};
use candle_transformers::models::mistral::{Config as MistralConfig, Model as MistralModel};
use candle_transformers::models::qwen3::{Config as QwenConfig, Model as QwenModel};
use hf_hub::{api::sync::ApiBuilder, Repo, RepoType};
use serde_json::Value;
use std::cell::RefCell;
use std::collections::HashMap;
use std::error::Error;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};
use tokenizers::Tokenizer;

/// Model architecture type - determines pooling strategy
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ModelArch {
    /// BERT-style bidirectional encoder (mean pooling)
    Bert,
    /// Causal/LM-style decoder (mean pooling)
    Causal,
}

impl ModelArch {
    /// Detect architecture from model config
    pub fn from_config(config: &str) -> Self {
        let model_type = model_type_from_config(config);

        match model_type.as_deref() {
            // Qwen, Llama, Mistral, Gemma use causal architecture
            Some("qwen2") | Some("qwen3") | Some("llama") | Some("mistral") | Some("gemma")
            | Some("gemma2") | Some("gemma3") | Some("gemma3_text") => ModelArch::Causal,
            // Default to BERT for everything else
            _ => ModelArch::Bert,
        }
    }
}

fn model_type_from_config(config: &str) -> Option<String> {
    let config: Value = serde_json::from_str(config).unwrap_or_default();
    config
        .get("model_type")
        .and_then(Value::as_str)
        .map(str::to_lowercase)
}

/// Model metadata for local models
#[derive(Debug)]
pub struct LocalModelInfo {
    pub config_path: PathBuf,
    pub tokenizer_path: PathBuf,
    pub weights_paths: Vec<PathBuf>,
}

fn model_download_lock(model_id: &str) -> Arc<Mutex<()>> {
    static LOCKS: OnceLock<Mutex<HashMap<String, Arc<Mutex<()>>>>> = OnceLock::new();
    let locks = LOCKS.get_or_init(|| Mutex::new(HashMap::new()));
    let mut map = locks.lock().unwrap_or_else(|e| e.into_inner());
    map.entry(model_id.to_string())
        .or_insert_with(|| Arc::new(Mutex::new(())))
        .clone()
}

#[cfg(test)]
struct DownloadTrackerGuard {
    model_id: String,
}

#[cfg(test)]
fn download_tracker_state() -> &'static Mutex<HashMap<String, (usize, usize)>> {
    static TRACKER: OnceLock<Mutex<HashMap<String, (usize, usize)>>> = OnceLock::new();
    TRACKER.get_or_init(|| Mutex::new(HashMap::new()))
}

#[cfg(test)]
fn download_tracker_enter(model_id: &str) -> DownloadTrackerGuard {
    let mut map = download_tracker_state()
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let entry = map.entry(model_id.to_string()).or_insert((0, 0));
    entry.0 += 1;
    if entry.0 > entry.1 {
        entry.1 = entry.0;
    }
    DownloadTrackerGuard {
        model_id: model_id.to_string(),
    }
}

#[cfg(test)]
impl Drop for DownloadTrackerGuard {
    fn drop(&mut self) {
        let mut map = download_tracker_state()
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        if let Some(entry) = map.get_mut(&self.model_id) {
            if entry.0 > 0 {
                entry.0 -= 1;
            }
        }
    }
}

#[cfg(test)]
pub(crate) fn reset_download_tracker(model_id: &str) {
    let mut map = download_tracker_state()
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    map.insert(model_id.to_string(), (0, 0));
}

#[cfg(test)]
pub(crate) fn download_max_for(model_id: &str) -> usize {
    let map = download_tracker_state()
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    map.get(model_id).map(|entry| entry.1).unwrap_or(0)
}

/// Download and cache model files from HuggingFace
pub fn build_model_info(
    cache_path: PathBuf,
    model_id: &str,
    revision: &str,
) -> Result<LocalModelInfo, Box<dyn Error>> {
    let download_lock = model_download_lock(model_id);
    let _download_guard = download_lock
        .lock()
        .map_err(|_| LibError::ModelLoadFailed)?;
    #[cfg(test)]
    let _download_tracker = download_tracker_enter(model_id);

    let repo = Repo::with_revision(model_id.to_string(), RepoType::Model, revision.to_string());
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
    let weights_paths = match api.get("model.safetensors") {
        Ok(path) => vec![path],
        Err(_) => {
            // Support sharded safetensors via model.safetensors.index.json
            let index_path = api
                .get("model.safetensors.index.json")
                .map_err(|_| LibError::ModelWeightsFetchFailed)?;
            let index_contents = std::fs::read_to_string(&index_path)
                .map_err(|_| LibError::ModelWeightsFetchFailed)?;
            let index_json: Value = serde_json::from_str(&index_contents)
                .map_err(|_| LibError::ModelWeightsFetchFailed)?;
            let weight_map = index_json
                .get("weight_map")
                .and_then(Value::as_object)
                .ok_or(LibError::ModelWeightsFetchFailed)?;

            let mut shards: Vec<String> = weight_map
                .values()
                .filter_map(|v| v.as_str().map(|s| s.to_string()))
                .collect();
            shards.sort();
            shards.dedup();

            if shards.is_empty() {
                return Err(Box::new(LibError::ModelWeightsFetchFailed));
            }

            let mut paths = Vec::with_capacity(shards.len());
            for shard in shards {
                let p = api
                    .get(&shard)
                    .map_err(|_| LibError::ModelWeightsFetchFailed)?;
                paths.push(p);
            }
            paths
        }
    };

    Ok(LocalModelInfo {
        config_path,
        tokenizer_path,
        weights_paths,
    })
}

/// Load tokenizer with fallback for BPE format
pub fn load_tokenizer(path: &PathBuf) -> Result<Tokenizer, Box<dyn Error>> {
    if let Ok(tok) = Tokenizer::from_file(path) {
        return Ok(tok);
    }

    let contents = std::fs::read_to_string(path).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
    let mut value: Value =
        serde_json::from_str(&contents).map_err(|_| LibError::ModelTokenizerLoadFailed)?;

    if let Some(model) = value.get_mut("model").and_then(Value::as_object_mut) {
        model.remove("ignore_merges");
        if let Some(merges) = model.get_mut("merges").and_then(Value::as_array_mut) {
            for item in merges.iter_mut() {
                if let Value::Array(parts) = item {
                    if parts.len() == 2 {
                        let a = parts[0].as_str().unwrap_or_default();
                        let b = parts[1].as_str().unwrap_or_default();
                        *item = Value::String(format!("{a} {b}"));
                    }
                }
            }
        }
    }

    let bytes = serde_json::to_vec(&value).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
    Tokenizer::from_bytes(&bytes).map_err(|_| LibError::ModelTokenizerLoadFailed.into())
}

/// BERT-style local embedding model
pub struct BertEmbeddingModel {
    model: BertModel,
    tokenizer: Tokenizer,
    max_input_len: usize,
    hidden_size: usize,
    device: Device,
}

impl BertEmbeddingModel {
    pub fn new(model_info: LocalModelInfo, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };
        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        let max_input_len =
            get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
        let hidden_size =
            get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;

        let mut config: BertConfig =
            serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
        config.hidden_act = HiddenAct::GeluApproximate;

        let mut tokenizer = load_tokenizer(&model_info.tokenizer_path)?;
        tokenizer.with_padding(None);
        let _ = tokenizer.with_truncation(None);

        let vb = unsafe {
            VarBuilder::from_mmaped_safetensors(&model_info.weights_paths, BERT_DTYPE, &device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        };

        let model = BertModel::load(vb, &config).map_err(|_| LibError::ModelLoadFailed)?;

        Ok(Self {
            model,
            tokenizer: tokenizer.clone(),
            max_input_len,
            hidden_size,
            device,
        })
    }
}

pub enum CausalEmbeddingKind {
    Qwen {
        model: RefCell<QwenModel>,
    },
    Llama {
        model: RefCell<LlamaModel>,
        config: LlamaConfig,
        dtype: DType,
    },
    Mistral {
        model: RefCell<MistralModel>,
    },
    Gemma {
        model: RefCell<GemmaModel>,
    },
}

/// Causal embedding model (Qwen/Llama/Mistral/Gemma).
pub struct CausalEmbeddingModel {
    kind: CausalEmbeddingKind,
    tokenizer: Tokenizer,
    max_input_len: usize,
    hidden_size: usize,
    device: Device,
    predict_lock: Mutex<()>,
}

impl CausalEmbeddingModel {
    pub fn new(
        model_info: LocalModelInfo,
        model_type: &str,
        use_gpu: bool,
    ) -> Result<Self, Box<dyn Error>> {
        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };

        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        let max_input_len =
            get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
        let hidden_size =
            get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;

        let dtype = dtype_from_config(&config, &device);
        let vb = unsafe {
            VarBuilder::from_mmaped_safetensors(&model_info.weights_paths, dtype, &device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        };

        let vb = if vb.contains_tensor("model.embed_tokens.weight") {
            vb
        } else if vb.contains_tensor("embed_tokens.weight") {
            vb.rename_f(|name| name.strip_prefix("model.").unwrap_or(name).to_string())
        } else {
            vb
        };

        let kind = match model_type {
            "qwen2" | "qwen3" => {
                let cfg: QwenConfig =
                    serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
                let model = QwenModel::new(&cfg, vb).map_err(|_| LibError::ModelLoadFailed)?;
                CausalEmbeddingKind::Qwen {
                    model: RefCell::new(model),
                }
            }
            "llama" => {
                let cfg_serde: LlamaConfigSerde =
                    serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
                let cfg = cfg_serde.into_config(false);
                let model = LlamaModel::load(vb, &cfg).map_err(|_| LibError::ModelLoadFailed)?;
                CausalEmbeddingKind::Llama {
                    model: RefCell::new(model),
                    config: cfg,
                    dtype,
                }
            }
            "mistral" => {
                let cfg: MistralConfig =
                    serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
                let model = MistralModel::new(&cfg, vb).map_err(|_| LibError::ModelLoadFailed)?;
                CausalEmbeddingKind::Mistral {
                    model: RefCell::new(model),
                }
            }
            "gemma" | "gemma2" | "gemma3" | "gemma3_text" => {
                let cfg: GemmaConfig =
                    serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
                let model =
                    GemmaModel::new(false, &cfg, vb).map_err(|_| LibError::ModelLoadFailed)?;
                CausalEmbeddingKind::Gemma {
                    model: RefCell::new(model),
                }
            }
            _ => return Err(Box::new(LibError::ModelLoadFailed)),
        };

        let mut tokenizer = load_tokenizer(&model_info.tokenizer_path)?;
        tokenizer.with_padding(None);
        let _ = tokenizer.with_truncation(None);

        Ok(Self {
            kind,
            tokenizer,
            max_input_len,
            hidden_size,
            device,
            predict_lock: Mutex::new(()),
        })
    }
}

/// Determine tensor dtype from config, handling CPU BF16 limitation
fn dtype_from_config(config: &str, device: &Device) -> DType {
    let config: Value = serde_json::from_str(config).unwrap_or_default();
    let dtype_str = config
        .get("torch_dtype")
        .or_else(|| config.get("dtype"))
        .and_then(Value::as_str);
    let dtype = dtype_str
        .map(|s| match s {
            "bfloat16" => DType::BF16,
            "float16" => DType::F16,
            "float32" => DType::F32,
            _ => DType::F16,
        })
        .unwrap_or(DType::F16);

    match (dtype, device) {
        (DType::BF16, Device::Cpu) => DType::F16,
        _ => dtype,
    }
}

/// Unified local embedding model
pub enum LocalModel {
    Bert(BertEmbeddingModel),
    Causal(CausalEmbeddingModel),
}

impl LocalModel {
    pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        let model_info = build_model_info(cache_path, model_id, "main")?;
        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        let arch = ModelArch::from_config(&config);
        let model_type = model_type_from_config(&config);

        match arch {
            ModelArch::Bert => {
                let model = BertEmbeddingModel::new(model_info, use_gpu)?;
                Ok(LocalModel::Bert(model))
            }
            ModelArch::Causal => match model_type.as_deref() {
                Some("qwen2") | Some("qwen3") | Some("llama") | Some("mistral") | Some("gemma")
                | Some("gemma2") | Some("gemma3") | Some("gemma3_text") => {
                    let model = CausalEmbeddingModel::new(
                        model_info,
                        model_type.as_deref().unwrap_or_default(),
                        use_gpu,
                    )?;
                    Ok(LocalModel::Causal(model))
                }
                _ => Err(Box::new(LibError::ModelLoadFailed)),
            },
        }
    }
}

impl TextModel for LocalModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
        let _predict_guard = match self {
            LocalModel::Causal(m) => Some(
                m.predict_lock
                    .lock()
                    .map_err(|_| LibError::ModelLoadFailed)?,
            ),
            LocalModel::Bert(_) => None,
        };

        let (device, max_input_len) = match self {
            LocalModel::Bert(m) => (m.device.clone(), m.max_input_len),
            LocalModel::Causal(m) => (m.device.clone(), m.max_input_len),
        };

        let mut all_results: Vec<Vec<f32>> = Vec::new();

        for text in texts.iter() {
            let tokens = match self {
                LocalModel::Bert(m) => m
                    .tokenizer
                    .encode(*text, true)
                    .map_err(|_| LibError::ModelTokenizerEncodeFailed)?
                    .get_ids()
                    .to_vec(),
                LocalModel::Causal(m) => m
                    .tokenizer
                    .encode(*text, true)
                    .map_err(|_| LibError::ModelTokenizerEncodeFailed)?
                    .get_ids()
                    .to_vec(),
            };

            let chunks = chunk_input_tokens(&tokens, max_input_len, max_input_len / 10);
            let mut results: Vec<Vec<f32>> = Vec::new();

            for chunk in chunks.iter() {
                let token_ids = Tensor::new(&chunk[..], &device)?.unsqueeze(0)?;
                let embeddings = match self {
                    LocalModel::Bert(m) => {
                        let token_type_ids = token_ids.zeros_like()?;
                        let emb = m.model.forward(&token_ids, &token_type_ids, None)?;
                        let seq_len = token_ids.dims()[1];
                        let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                        let divisor = Tensor::new(seq_len as f32, &device)?;
                        let divided = summed.broadcast_div(&divisor)?;
                        divided.to_dtype(DType::F32)?
                    }
                    LocalModel::Causal(m) => match &m.kind {
                        CausalEmbeddingKind::Qwen { model } => {
                            let mut model = model.borrow_mut();
                            model.clear_kv_cache();
                            let emb = model.forward(&token_ids, 0)?;
                            let (_, n_tokens, _) = emb.dims3()?;
                            let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                            let divisor = Tensor::new(n_tokens as f32, &device)?;
                            let divided = summed.broadcast_div(&divisor)?;
                            divided.to_dtype(DType::F32)?
                        }
                        CausalEmbeddingKind::Llama {
                            model,
                            config,
                            dtype,
                        } => {
                            let mut cache = LlamaCache::new(false, *dtype, config, &device)?;
                            let emb = model
                                .borrow()
                                .forward_hidden_states(&token_ids, 0, &mut cache)?;
                            let (_, n_tokens, _) = emb.dims3()?;
                            let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                            let divisor = Tensor::new(n_tokens as f32, &device)?;
                            let divided = summed.broadcast_div(&divisor)?;
                            divided.to_dtype(DType::F32)?
                        }
                        CausalEmbeddingKind::Mistral { model } => {
                            let mut model = model.borrow_mut();
                            model.clear_kv_cache();
                            let emb = model.forward_hidden_states(&token_ids, 0)?;
                            let (_, n_tokens, _) = emb.dims3()?;
                            let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                            let divisor = Tensor::new(n_tokens as f32, &device)?;
                            let divided = summed.broadcast_div(&divisor)?;
                            divided.to_dtype(DType::F32)?
                        }
                        CausalEmbeddingKind::Gemma { model } => {
                            let mut model = model.borrow_mut();
                            model.clear_kv_cache();
                            let emb = model.forward_hidden_states(&token_ids, 0)?;
                            let (_, n_tokens, _) = emb.dims3()?;
                            let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                            let divisor = Tensor::new(n_tokens as f32, &device)?;
                            let divided = summed.broadcast_div(&divisor)?;
                            divided.to_dtype(DType::F32)?
                        }
                    },
                };

                if let Ok(e_j) = embeddings.get(0) {
                    let emb_vec: Vec<f32> = e_j
                        .to_vec1::<f32>()
                        .map_err(|e| -> Box<dyn Error> { Box::new(e) })?;
                    let mut emb = emb_vec;
                    normalize(&mut emb);
                    results.push(emb);
                }
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
        match self {
            LocalModel::Bert(m) => m.hidden_size,
            LocalModel::Causal(m) => m.hidden_size,
        }
    }

    fn get_max_input_len(&self) -> usize {
        match self {
            LocalModel::Bert(m) => m.max_input_len,
            LocalModel::Causal(m) => m.max_input_len,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_model_arch_detection_qwen3() {
        let qwen_config = r#"{"model_type":"qwen3"}"#;
        assert_eq!(ModelArch::from_config(qwen_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_qwen2() {
        let qwen2_config = r#"{"model_type":"qwen2"}"#;
        assert_eq!(ModelArch::from_config(qwen2_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_llama() {
        let llama_config = r#"{"model_type":"llama"}"#;
        assert_eq!(ModelArch::from_config(llama_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_mistral() {
        let mistral_config = r#"{"model_type":"mistral"}"#;
        assert_eq!(ModelArch::from_config(mistral_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_gemma() {
        let gemma_config = r#"{"model_type":"gemma"}"#;
        assert_eq!(ModelArch::from_config(gemma_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_gemma3_text() {
        let gemma3_config = r#"{"model_type":"gemma3_text"}"#;
        assert_eq!(ModelArch::from_config(gemma3_config), ModelArch::Causal);
    }

    #[test]
    fn test_model_arch_detection_bert() {
        let bert_config = r#"{"model_type":"bert"}"#;
        assert_eq!(ModelArch::from_config(bert_config), ModelArch::Bert);
    }

    #[test]
    fn test_model_arch_detection_invalid() {
        let invalid_config = r#"{"foo":"bar"}"#;
        assert_eq!(ModelArch::from_config(invalid_config), ModelArch::Bert);
    }

    #[test]
    fn test_model_arch_detection_case_insensitive() {
        let config1 = r#"{"model_type":"Qwen3","_name_or_path":"qwen/embedding"}"#;
        assert_eq!(ModelArch::from_config(config1), ModelArch::Causal);
    }

    #[test]
    fn test_dtype_from_config_bf16_cpu() {
        let bf16_config = r#"{"torch_dtype":"bfloat16"}"#;
        assert_eq!(dtype_from_config(bf16_config, &Device::Cpu), DType::F16);
    }

    #[test]
    fn test_dtype_from_config_f16() {
        let f16_config = r#"{"torch_dtype":"float16"}"#;
        assert_eq!(dtype_from_config(f16_config, &Device::Cpu), DType::F16);
    }

    #[test]
    fn test_dtype_from_config_f32() {
        let f32_config = r#"{"torch_dtype":"float32"}"#;
        assert_eq!(dtype_from_config(f32_config, &Device::Cpu), DType::F32);
    }

    #[test]
    fn test_dtype_from_config_default() {
        let default_config = r#"{}"#;
        assert_eq!(dtype_from_config(default_config, &Device::Cpu), DType::F16);
    }

    #[test]
    fn test_dtype_from_config_dtype_field() {
        let f32_config = r#"{"dtype":"float32"}"#;
        assert_eq!(dtype_from_config(f32_config, &Device::Cpu), DType::F32);
    }

    // Qwen model integration tests (require actual model files)
    #[test]
    fn test_qwen3_embedding_model_detection() {
        // Test that Qwen3 models are detected as Causal architecture
        let qwen_config = r#"{
            "model_type": "qwen3",
            "hidden_size": 2048,
            "num_attention_heads": 16,
            "num_hidden_layers": 24,
            "vocab_size": 151646,
            "max_position_embeddings": 32768
        }"#;
        assert_eq!(ModelArch::from_config(qwen_config), ModelArch::Causal);
    }

    #[test]
    fn test_qwen3_embedding_hidden_size() {
        // Qwen3-Embedding models typically have 2048 dimensions
        let qwen_config = r#"{"model_type":"qwen3","hidden_size":2048}"#;
        assert_eq!(get_hidden_size(qwen_config).unwrap(), 2048);
    }

    #[test]
    fn test_qwen3_embedding_max_input_len() {
        // Qwen3 models support long context (32K tokens)
        let qwen_config = r#"{"model_type":"qwen3","max_position_embeddings":32768}"#;
        assert_eq!(get_max_input_length(qwen_config).unwrap(), 32768);
    }

    #[test]
    fn test_qwen2_model_detection() {
        // Qwen2 should also be detected as Causal
        let qwen2_config = r#"{"model_type":"qwen2"}"#;
        assert_eq!(ModelArch::from_config(qwen2_config), ModelArch::Causal);
    }

    #[test]
    fn test_llama_embedding_hidden_size() {
        // Llama models typically have various hidden sizes (4096, 5120, etc.)
        let llama_config = r#"{"model_type":"llama","hidden_size":4096}"#;
        assert_eq!(get_hidden_size(llama_config).unwrap(), 4096);
    }

    #[test]
    fn test_llama_embedding_max_input_len() {
        // Llama models support various context lengths
        let llama_config = r#"{"model_type":"llama","max_position_embeddings":4096}"#;
        assert_eq!(get_max_input_length(llama_config).unwrap(), 4096);
    }

    #[test]
    fn test_mistral_embedding_hidden_size() {
        // Mistral models typically have 4096 hidden dimensions
        let mistral_config = r#"{"model_type":"mistral","hidden_size":4096}"#;
        assert_eq!(get_hidden_size(mistral_config).unwrap(), 4096);
    }

    #[test]
    fn test_gemma_embedding_hidden_size() {
        // Gemma models have various hidden sizes
        let gemma_config = r#"{"model_type":"gemma","hidden_size":2048}"#;
        assert_eq!(get_hidden_size(gemma_config).unwrap(), 2048);
    }
}
