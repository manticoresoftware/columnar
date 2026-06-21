mod jina;
mod local;
mod openai;
pub mod text_model_wrapper;
mod voyage;

#[cfg(test)]
mod openai_test;

#[cfg(test)]
mod voyage_test;

#[cfg(test)]
mod jina_test;

#[cfg(test)]
mod local_test;

#[cfg(test)]
mod ffi_test;

#[cfg(test)]
mod create_model_test;

use std::error::Error;
use std::path::PathBuf;

pub trait TextModel {
    /// Generate embeddings for the given texts.
    ///
    /// `threads` caps the number of CPU threads used during generation.
    /// 0 means "use all available CPUs" (default).
    fn predict(&self, texts: &[&str], threads: usize) -> Result<Vec<Vec<f32>>, Box<dyn Error>>;
    fn get_hidden_size(&self) -> usize;
    fn get_max_input_len(&self) -> usize;
    /// Validates the API key by making a minimal test request to the API.
    /// For remote models, this makes an actual HTTP request. For local models, this is a no-op.
    fn validate_api_key(&self) -> Result<(), Box<dyn Error>>;

    /// Split one document into chunk byte spans `(start, end)` per `settings`.
    /// Default impl is the char/byte heuristic for remote API models (which have
    /// no local tokenizer); `LocalModel` overrides it with token-accurate
    /// chunking via its loaded tokenizer.
    fn chunk(&self, text: &str, settings: &crate::chunk::ChunkSettings) -> Vec<(usize, usize)> {
        let max = crate::chunk::effective_max(settings, self.get_max_input_len());
        crate::chunk::chunk_chars(text, max, settings.overlap_tokens as usize)
    }
}

#[repr(C)]
pub struct ModelOptions {
    pub model_id: String,
    pub cache_path: Option<String>,
    pub api_key: Option<String>,
    pub api_url: Option<String>,
    pub api_timeout: Option<u64>, // Timeout in seconds (None means use default: 10 seconds)
    pub use_gpu: Option<bool>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModelValidationMode {
    StrictBuiltInList,
    Passthrough,
}

/// Unified model enum
///
/// Architecture:
/// - Remote providers: OpenAI, Voyage, Jina (HTTP API-based, need API keys)
/// - Local models: Local (auto-detects BERT vs Causal architecture from config)
///
/// Qwen/Llama/etc. models are handled via LocalModel - no separate provider needed.
#[repr(C)]
pub enum Model {
    OpenAI(Box<openai::OpenAIModel>),
    Voyage(Box<voyage::VoyageModel>),
    Jina(Box<jina::JinaModel>),
    Local(Box<local::LocalModel>),
}

impl TextModel for Model {
    fn predict(&self, texts: &[&str], threads: usize) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
        match self {
            Model::OpenAI(m) => m.predict(texts, threads),
            Model::Voyage(m) => m.predict(texts, threads),
            Model::Jina(m) => m.predict(texts, threads),
            Model::Local(m) => m.predict(texts, threads),
        }
    }

    fn get_hidden_size(&self) -> usize {
        match self {
            Model::OpenAI(m) => m.get_hidden_size(),
            Model::Voyage(m) => m.get_hidden_size(),
            Model::Jina(m) => m.get_hidden_size(),
            Model::Local(m) => m.get_hidden_size(),
        }
    }

    fn get_max_input_len(&self) -> usize {
        match self {
            Model::OpenAI(m) => m.get_max_input_len(),
            Model::Voyage(m) => m.get_max_input_len(),
            Model::Jina(m) => m.get_max_input_len(),
            Model::Local(m) => m.get_max_input_len(),
        }
    }

    fn validate_api_key(&self) -> Result<(), Box<dyn Error>> {
        match self {
            Model::OpenAI(m) => m.validate_api_key(),
            Model::Voyage(m) => m.validate_api_key(),
            Model::Jina(m) => m.validate_api_key(),
            Model::Local(m) => m.validate_api_key(),
        }
    }

    fn chunk(&self, text: &str, settings: &crate::chunk::ChunkSettings) -> Vec<(usize, usize)> {
        match self {
            Model::OpenAI(m) => m.chunk(text, settings),
            Model::Voyage(m) => m.chunk(text, settings),
            Model::Jina(m) => m.chunk(text, settings),
            Model::Local(m) => m.chunk(text, settings),
        }
    }
}

/// Refuse local (candle) inference on an emulated x86 CPU. Rosetta 2 and QEMU
/// advertise FMA without AVX — a combination that exists on no real x86 chip
/// (FMA3 shipped with Haswell alongside AVX2; AVX predates both). candle/gemm
/// crash on that broken SIMD profile, taking the daemon with them. Fail fast
/// with a clear message; API models don't run local inference so they skip this.
fn ensure_local_inference_supported() -> Result<(), Box<dyn Error>> {
    #[cfg(target_arch = "x86_64")]
    if std::arch::is_x86_feature_detected!("fma") && !std::arch::is_x86_feature_detected!("avx") {
        return Err(
            "local embedding models are not supported under x86 emulation \
            (Rosetta/QEMU on Apple Silicon): the emulated CPU lacks AVX and inference \
            would crash. Use a native arm64 build, or use an API model (openai/voyage/jina)"
                .into(),
        );
    }
    Ok(())
}

pub fn create_model(options: ModelOptions) -> Result<Model, Box<dyn Error>> {
    let model_id = options.model_id.as_str();
    let api_key = options.api_key.unwrap_or_default();
    let api_url = options.api_url;
    let api_timeout = options.api_timeout;

    // Remote providers (HTTP APIs)
    if model_id.starts_with("openai:") {
        let model = openai::OpenAIModel::new_with_validation_mode(
            model_id,
            api_key.as_str(),
            api_url.as_deref(),
            api_timeout,
            ModelValidationMode::Passthrough,
        )?;

        Ok(Model::OpenAI(Box::new(model)))
    } else if model_id.starts_with("openai/") {
        let model =
            openai::OpenAIModel::new(model_id, api_key.as_str(), api_url.as_deref(), api_timeout)?;

        Ok(Model::OpenAI(Box::new(model)))
    } else if model_id.starts_with("voyage:") {
        let model = voyage::VoyageModel::new_with_validation_mode(
            model_id,
            api_key.as_str(),
            api_url.as_deref(),
            api_timeout,
            ModelValidationMode::Passthrough,
        )?;

        Ok(Model::Voyage(Box::new(model)))
    } else if model_id.starts_with("voyage/") {
        let model =
            voyage::VoyageModel::new(model_id, api_key.as_str(), api_url.as_deref(), api_timeout)?;

        Ok(Model::Voyage(Box::new(model)))
    } else if model_id.starts_with("jina:") {
        let model = jina::JinaModel::new_with_validation_mode(
            model_id,
            api_key.as_str(),
            api_url.as_deref(),
            api_timeout,
            ModelValidationMode::Passthrough,
        )?;

        Ok(Model::Jina(Box::new(model)))
    } else if model_id.starts_with("jina/") {
        let model =
            jina::JinaModel::new(model_id, api_key.as_str(), api_url.as_deref(), api_timeout)?;

        Ok(Model::Jina(Box::new(model)))
    } else {
        // Local models - auto-detect architecture from config
        // Supports: BERT, SentenceTransformers, Qwen, Llama, Mistral, Gemma, etc.
        // For gated models, api_key is used as HuggingFace token
        ensure_local_inference_supported()?;
        let cache_path = PathBuf::from(
            options
                .cache_path
                .unwrap_or(String::from(".cache/manticore")),
        );

        let hf_token = if api_key.is_empty() {
            None
        } else {
            Some(api_key.as_str())
        };
        let model = local::LocalModel::new(
            model_id,
            cache_path,
            options.use_gpu.unwrap_or(false),
            hf_token,
        )?;

        Ok(Model::Local(Box::new(model)))
    }
}
