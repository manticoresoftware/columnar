
#[derive(Debug, PartialEq, Eq, Hash)]
pub enum LibError {
	HuggingFaceApiBuildFailed,
	ModelConfigFetchFailed,
	ModelConfigReadFailed,
	ModelConfigParseFailed,
	ModelTokenizerFetchFailed,
	ModelTokenizerLoadFailed,
	ModelTokenizerConfigurationFailed,
	ModelTokenizerEncodeFailed,
	ModelWeightsFetchFailed,
	ModelWeightsLoadFailed,
	ModelHiddenSizeGetFailed,
	ModelMaxInputLenGetFailed,
	ModelLoadFailed,
	DeviceCudaInitFailed,
	RemoteUnsupportedModel,
	RemoteInvalidAPIKey,
	RemoteRequestSendFailed,
	RemoteResponseParseFailed,
}

// Implement std::error::Error for LibError
impl std::error::Error for LibError {}

// Implement Display for LibError
impl std::fmt::Display for LibError {
	fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
		match self {
			LibError::HuggingFaceApiBuildFailed => write!(f, "Failed to set up the Hugging Face API connection"),
			LibError::ModelConfigFetchFailed => write!(f, "Failed to download model configuration"),
			LibError::ModelConfigReadFailed => write!(f, "Failed to read model configuration file"),
			LibError::ModelConfigParseFailed => write!(f, "Failed to parse model configuration"),
			LibError::ModelTokenizerFetchFailed => write!(f, "Failed to download model tokenizer"),
			LibError::ModelTokenizerLoadFailed => write!(f, "Failed to load model tokenizer to memory"),
			LibError::ModelTokenizerConfigurationFailed => write!(f, "Failed to configure model tokenizer"),
			LibError::ModelTokenizerEncodeFailed => write!(f, "Failed to encode text for model"),
			LibError::ModelWeightsFetchFailed => write!(f, "Failed to download model weights"),
			LibError::ModelWeightsLoadFailed => write!(f, "Failed to load model weights to memory"),
			LibError::ModelLoadFailed => write!(f, "Failed to create an instance of the model"),
			LibError::ModelHiddenSizeGetFailed => write!(f, "Failed to get model hidden size"),
			LibError::ModelMaxInputLenGetFailed => write!(f, "Failed to get model max input length"),
			LibError::DeviceCudaInitFailed => write!(f, "Failed to initialize CUDA device"),
			LibError::RemoteUnsupportedModel => write!(f, "Unsupported remote model given"),
			LibError::RemoteInvalidAPIKey => write!(f, "Invalid API key for remote model"),
			LibError::RemoteRequestSendFailed => write!(f, "Failed to send request to remote model"),
			LibError::RemoteResponseParseFailed => write!(f, "Failed to parse response from remote model"),
		}
	}
}
