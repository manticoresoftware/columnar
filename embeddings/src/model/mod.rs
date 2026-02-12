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

use std::error::Error;
use std::path::PathBuf;

pub trait TextModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>>;
    fn get_hidden_size(&self) -> usize;
    fn get_max_input_len(&self) -> usize;
}

#[repr(C)]
pub struct ModelOptions {
    pub model_id: String,
    pub cache_path: Option<String>,
    pub api_key: Option<String>,
    pub use_gpu: Option<bool>,
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
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
        match self {
            Model::OpenAI(m) => m.predict(texts),
            Model::Voyage(m) => m.predict(texts),
            Model::Jina(m) => m.predict(texts),
            Model::Local(m) => m.predict(texts),
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
}

pub fn create_model(options: ModelOptions) -> Result<Model, Box<dyn Error>> {
    let model_id = options.model_id.as_str();

    // Remote providers (HTTP APIs)
    if model_id.starts_with("openai/") {
        let model =
            openai::OpenAIModel::new(model_id, options.api_key.unwrap_or_default().as_str())?;
        return Ok(Model::OpenAI(Box::new(model)));
    }

    if model_id.starts_with("voyage/") {
        let model =
            voyage::VoyageModel::new(model_id, options.api_key.unwrap_or_default().as_str())?;
        return Ok(Model::Voyage(Box::new(model)));
    }

    if model_id.starts_with("jina/") {
        let model = jina::JinaModel::new(model_id, options.api_key.unwrap_or_default().as_str())?;
        return Ok(Model::Jina(Box::new(model)));
    }

    // Local models - auto-detect architecture from config
    // Supports: BERT, SentenceTransformers, Qwen, Llama, Mistral, Gemma, etc.
    let cache_path = PathBuf::from(
        options
            .cache_path
            .unwrap_or(String::from(".cache/manticore")),
    );

    let model = local::LocalModel::new(model_id, cache_path, options.use_gpu.unwrap_or(false))?;
    Ok(Model::Local(Box::new(model)))
}
