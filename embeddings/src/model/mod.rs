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
    /// Validates the API key by making a minimal test request to the API.
    /// For remote models, this makes an actual HTTP request. For local models, this is a no-op.
    fn validate_api_key(&self) -> Result<(), Box<dyn Error>>;
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

    fn validate_api_key(&self) -> Result<(), Box<dyn Error>> {
        match self {
            Model::OpenAI(m) => m.validate_api_key(),
            Model::Voyage(m) => m.validate_api_key(),
            Model::Jina(m) => m.validate_api_key(),
            Model::Local(m) => m.validate_api_key(),
        }
    }
}

pub fn create_model(options: ModelOptions) -> Result<Model, Box<dyn Error>> {
    let model_id = options.model_id.as_str();
    if model_id.starts_with("openai/") {
        let model = openai::OpenAIModel::new(
            model_id,
            options.api_key.unwrap_or_default().as_str(),
            options.api_url.as_deref(),
            options.api_timeout,
        )?;

        Ok(Model::OpenAI(Box::new(model)))
    } else if model_id.starts_with("voyage/") {
        let model = voyage::VoyageModel::new(
            model_id,
            options.api_key.unwrap_or_default().as_str(),
            options.api_url.as_deref(),
            options.api_timeout,
        )?;

        Ok(Model::Voyage(Box::new(model)))
    } else if model_id.starts_with("jina/") {
        let model = jina::JinaModel::new(
            model_id,
            options.api_key.unwrap_or_default().as_str(),
            options.api_url.as_deref(),
            options.api_timeout,
        )?;

        Ok(Model::Jina(Box::new(model)))
    } else {
        let model = local::LocalModel::new(
            model_id,
            PathBuf::from(
                options
                    .cache_path
                    .unwrap_or(String::from(".cache/manticore")),
            ),
            options.use_gpu.unwrap_or(false),
        )?;

        Ok(Model::Local(Box::new(model)))
    }
}
