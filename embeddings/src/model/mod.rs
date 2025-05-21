mod openai;
mod local;
pub mod text_model_wrapper;

use std::path::PathBuf;
use std::error::Error;

pub trait TextModel {
	fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>>;
	fn get_hidden_size(&self) -> usize;
	fn get_max_input_len(&self) -> usize;
}

#[repr(C)]
pub struct ModelOptions {
	model_id: String,
	cache_path: Option<String>,
	api_key: Option<String>,
	use_gpu: Option<bool>,
}

#[repr(C)]
pub enum Model {
	OpenAI(openai::OpenAIModel),
	Local(local::LocalModel),
}

impl TextModel for Model {
	fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
		match self {
			Model::OpenAI(m) => m.predict(texts),
			Model::Local(m) => m.predict(texts),
		}
	}

	fn get_hidden_size(&self) -> usize {
		match self {
			Model::OpenAI(m) => m.get_hidden_size(),
			Model::Local(m) => m.get_hidden_size(),
		}
	}

	fn get_max_input_len(&self) -> usize {
		match self {
			Model::OpenAI(m) => m.get_max_input_len(),
			Model::Local(m) => m.get_max_input_len(),
		}
	}
}

pub fn create_model(options: ModelOptions) -> Result<Model, Box<dyn Error>> {
	let model_id = options.model_id.as_str();
	if model_id.starts_with("openai/") {
		let model = openai::OpenAIModel::new(
			model_id,
			options.api_key
				.unwrap_or(String::new())
				.as_str()
		)?;

		Ok(Model::OpenAI(model))
	} else {
		let model = local::LocalModel::new(
			model_id,
			PathBuf::from(
				options.cache_path
					.unwrap_or(String::from(".cache/manticore"))
			),
			options.use_gpu.unwrap_or(false)
		)?;

		Ok(Model::Local(model))
	}
}
