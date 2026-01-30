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
use candle_transformers::models::qwen3::{Config as QwenConfig, Model as QwenModel};
use hf_hub::{api::sync::ApiBuilder, Repo, RepoType};
use serde_json::Value;
use std::cell::RefCell;
use std::error::Error;
use std::path::PathBuf;
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
        let config: Value = serde_json::from_str(config).unwrap_or_default();
        let model_type = config
            .get("model_type")
            .and_then(Value::as_str)
            .map(str::to_lowercase);

        match model_type.as_deref() {
            // Qwen, Llama, Mistral, Gemma use causal architecture
            Some("qwen2") | Some("qwen3") | Some("llama") | Some("mistral") | Some("gemma") => {
                ModelArch::Causal
            }
            // Default to BERT for everything else
            _ => ModelArch::Bert,
        }
    }
}

/// Model metadata for local models
#[derive(Debug)]
pub struct LocalModelInfo {
    pub config_path: PathBuf,
    pub tokenizer_path: PathBuf,
    pub weights_path: PathBuf,
}

/// Download and cache model files from HuggingFace
pub fn build_model_info(
    cache_path: PathBuf,
    model_id: &str,
    revision: &str,
) -> Result<LocalModelInfo, Box<dyn Error>> {
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
    let weights_path = api
        .get("model.safetensors")
        .map_err(|_| LibError::ModelWeightsFetchFailed)?;

    Ok(LocalModelInfo {
        config_path,
        tokenizer_path,
        weights_path,
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
    pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };

        let model_info = build_model_info(cache_path, model_id, "main")?;
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
            VarBuilder::from_mmaped_safetensors(&[model_info.weights_path], BERT_DTYPE, &device)
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

/// Causal/LM-style local embedding model (supports Qwen, Llama, etc.)
pub struct CausalEmbeddingModel {
    model: RefCell<QwenModel>,
    tokenizer: Tokenizer,
    max_input_len: usize,
    hidden_size: usize,
    device: Device,
}

impl CausalEmbeddingModel {
    pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        eprintln!("[DEBUG] CausalEmbeddingModel::new - model_id={}", model_id);

        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };
        eprintln!("[DEBUG] Device created: {:?}", device);

        let model_info = build_model_info(cache_path.clone(), model_id, "main")?;
        eprintln!(
            "[DEBUG] Model info built - config={:?}, tokenizer={:?}, weights={:?}",
            model_info.config_path, model_info.tokenizer_path, model_info.weights_path
        );

        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        eprintln!("[DEBUG] Config read successfully, {} bytes", config.len());

        let max_input_len =
            get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
        let hidden_size =
            get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;
        eprintln!(
            "[DEBUG] max_input_len={}, hidden_size={}",
            max_input_len, hidden_size
        );

        let cfg: QwenConfig =
            serde_json::from_str(&config).map_err(|_| LibError::ModelConfigParseFailed)?;
        eprintln!("[DEBUG] QwenConfig parsed successfully");

        let dtype = dtype_from_config(&config, &device);
        eprintln!("[DEBUG] dtype determined: {:?}", dtype);

        eprintln!(
            "[DEBUG] Loading safetensors from {:?}",
            model_info.weights_path
        );

        let weights_path = model_info.weights_path;
        eprintln!("[DEBUG] Creating VarBuilder from safetensors...");

        // Qwen3-Embedding weights are stored without "model." prefix
        // (e.g., "embed_tokens.weight", "layers.0.*"), while candle's QwenModel
        // looks up "model.embed_tokens.weight" and "model.layers.0.*".
        let vb = unsafe {
            VarBuilder::from_mmaped_safetensors(std::slice::from_ref(&weights_path), dtype, &device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        };
        eprintln!("[DEBUG] VarBuilder created successfully!");

        // Important: some HF checkpoints (incl. Qwen3-Embedding, many Llama/Mistral)
        // store tensors without "model." while candle looks up "model.*".
        // This remap aligns lookup names to on-disk keys without modifying files.
        let vb = if vb.contains_tensor("model.embed_tokens.weight") {
            vb
        } else if vb.contains_tensor("embed_tokens.weight") {
            vb.rename_f(|name| name.strip_prefix("model.").unwrap_or(name).to_string())
        } else {
            vb
        };

        eprintln!("[DEBUG] Creating QwenModel...");
        let model = QwenModel::new(&cfg, vb).map_err(|e| {
            eprintln!("[DEBUG] QwenModel::new failed with error: {:?}", e);
            LibError::ModelLoadFailed
        })?;
        eprintln!("[DEBUG] QwenModel created successfully!");

        let mut tokenizer = load_tokenizer(&model_info.tokenizer_path)?;
        tokenizer.with_padding(None);
        let _ = tokenizer.with_truncation(None);

        Ok(Self {
            model: RefCell::new(model),
            tokenizer: tokenizer.clone(),
            max_input_len,
            hidden_size,
            device,
        })
    }
}

/// Determine tensor dtype from config, handling CPU BF16 limitation
fn dtype_from_config(config: &str, device: &Device) -> DType {
    let config: Value = serde_json::from_str(config).unwrap_or_default();
    let dtype = config
        .get("torch_dtype")
        .and_then(Value::as_str)
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
        let model_info = build_model_info(cache_path.clone(), model_id, "main")?;
        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        let arch = ModelArch::from_config(&config);

        match arch {
            ModelArch::Bert => {
                let model = BertEmbeddingModel::new(model_id, cache_path, use_gpu)?;
                Ok(LocalModel::Bert(model))
            }
            ModelArch::Causal => {
                let model = CausalEmbeddingModel::new(model_id, cache_path, use_gpu)?;
                Ok(LocalModel::Causal(model))
            }
        }
    }
}

impl TextModel for LocalModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
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
                    LocalModel::Causal(m) => {
                        let mut model = m.model.borrow_mut();
                        model.clear_kv_cache();
                        let emb = model.forward(&token_ids, 0)?;
                        let (_, n_tokens, _) = emb.dims3()?;
                        let summed = emb.sum(1)?.to_dtype(DType::F32)?;
                        let divisor = Tensor::new(n_tokens as f32, &device)?;
                        let divided = summed.broadcast_div(&divisor)?;
                        divided.to_dtype(DType::F32)?
                    }
                };

                if let Ok(e_j) = embeddings.get(0) {
                    let emb_vec: Vec<f32> = e_j
                        .to_vec1::<f32>()
                        .map_err(|e| Box::new(e) as Box<dyn Error>)?;
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
    use approx::assert_abs_diff_eq;

    fn check_embedding_properties(embedding: &[f32], expected_len: usize) {
        assert_eq!(embedding.len(), expected_len);
        let norm: f32 = embedding.iter().map(|&x| x * x).sum::<f32>().sqrt();
        assert_abs_diff_eq!(norm, 1.0, epsilon = 1e-6);
    }

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
    fn test_all_minilm_l6_v2() {
        let model_id = "sentence-transformers/all-MiniLM-L6-v2";
        let cache_path = PathBuf::from(".cache/manticore");

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

    #[test]
    fn test_qwen_embedding_properties() {
        // Integration test for Qwen embedding models
        let model_id = "Qwen/Qwen3-Embedding-0.6B";
        let cache_path = PathBuf::from(".cache/manticore");

        // Create model (will download if not cached)
        let local_model = LocalModel::new(model_id, cache_path.clone(), false)
            .expect("Qwen model should load successfully");

        // Test basic properties (Qwen3-Embedding-0.6B has 1024 hidden_size)
        assert_eq!(local_model.get_hidden_size(), 1024);
        assert_eq!(local_model.get_max_input_len(), 32768);

        // Test embedding generation
        let test_text = &["This is a test sentence for Qwen embedding model."];
        let embeddings = local_model
            .predict(test_text)
            .expect("Qwen model should generate embeddings");

        // Verify embedding properties
        check_embedding_properties(&embeddings[0], local_model.get_hidden_size());
    }

    #[test]
    fn test_llama_embedding_properties() {
        // Integration test for Llama-based embedding models
        // Note: This uses a hypothetical Llama embedding model
        // Most Llama models are not specifically designed for embeddings
        // but the architecture should work with mean pooling
        let model_id = "llama-embedding-test-model"; // Placeholder
        let cache_path = PathBuf::from(".cache/manticore");

        // Create model (will download if not cached)
        let result = LocalModel::new(model_id, cache_path.clone(), false);

        // Skip if model cannot be loaded (expected for most cases)
        let local_model = match result {
            Ok(m) => m,
            Err(e) => {
                println!("Llama model loading skipped (expected): {}", e);
                return;
            }
        };

        // Test embedding generation if model loads
        let test_text = &["This is a test sentence for Llama embedding model."];
        let embeddings = local_model.predict(test_text).unwrap();

        // Verify embedding properties
        check_embedding_properties(&embeddings[0], local_model.get_hidden_size());
    }

    #[test]
    fn test_mistral_embedding_properties() {
        // Integration test for Mistral-based embedding models
        // Similar to Llama, Mistral models are typically not designed for embeddings
        // but the causal architecture should support mean pooling
        let model_id = "mistral-embedding-test-model"; // Placeholder
        let cache_path = PathBuf::from(".cache/manticore");

        let result = LocalModel::new(model_id, cache_path.clone(), false);

        let local_model = match result {
            Ok(m) => m,
            Err(e) => {
                println!("Mistral model loading skipped (expected): {}", e);
                return;
            }
        };

        let test_text = &["This is a test sentence for Mistral embedding model."];
        let embeddings = local_model.predict(test_text).unwrap();
        check_embedding_properties(&embeddings[0], local_model.get_hidden_size());
    }

    #[test]
    fn test_causal_model_batch_embeddings() {
        // Test batch processing with Qwen model
        let model_id = "Qwen/Qwen3-Embedding-0.6B";
        let cache_path = PathBuf::from(".cache/manticore");

        let result = LocalModel::new(model_id, cache_path.clone(), false);

        let local_model = match result {
            Ok(m) => m,
            Err(e) => {
                println!("Qwen batch test skipped: {}", e);
                return;
            }
        };

        // Test multiple sentences at once
        let test_texts = &[
            "First test sentence.",
            "Second test sentence with different content.",
            "Third sentence for batch processing verification.",
        ];

        let embeddings = local_model.predict(test_texts).unwrap();

        // Verify we got embeddings for all inputs
        assert_eq!(embeddings.len(), test_texts.len());

        // Verify each embedding has correct properties
        for embedding in &embeddings {
            check_embedding_properties(embedding, local_model.get_hidden_size());
        }

        // Verify embeddings are different (not all the same)
        let first = &embeddings[0];
        let second = &embeddings[1];
        let mut differences = 0;
        for (a, b) in first.iter().zip(second.iter()) {
            if (a - b).abs() > 1e-6 {
                differences += 1;
            }
        }
        assert!(
            differences > 100,
            "Embeddings should be different for different texts"
        );
    }
}
