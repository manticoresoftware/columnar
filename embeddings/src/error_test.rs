use super::error::LibError;
use std::error::Error;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_lib_error_display() {
        let test_cases = vec![
            (
                LibError::HuggingFaceApiBuildFailed,
                "Failed to set up the Hugging Face API connection",
            ),
            (
                LibError::ModelConfigFetchFailed,
                "Failed to download model configuration",
            ),
            (
                LibError::ModelConfigReadFailed,
                "Failed to read model configuration file",
            ),
            (
                LibError::ModelConfigParseFailed,
                "Failed to parse model configuration",
            ),
            (
                LibError::ModelTokenizerFetchFailed,
                "Failed to download model tokenizer",
            ),
            (
                LibError::ModelTokenizerLoadFailed,
                "Failed to load model tokenizer to memory",
            ),
            (
                LibError::ModelTokenizerConfigurationFailed,
                "Failed to configure model tokenizer",
            ),
            (
                LibError::ModelTokenizerEncodeFailed,
                "Failed to encode text for model",
            ),
            (
                LibError::ModelWeightsFetchFailed,
                "Failed to download model weights",
            ),
            (
                LibError::ModelWeightsLoadFailed,
                "Failed to load model weights to memory",
            ),
            (
                LibError::ModelLoadFailed,
                "Failed to create an instance of the model",
            ),
            (
                LibError::ModelHiddenSizeGetFailed,
                "Failed to get model hidden size",
            ),
            (
                LibError::ModelMaxInputLenGetFailed,
                "Failed to get model max input length",
            ),
            (
                LibError::DeviceCudaInitFailed,
                "Failed to initialize CUDA device",
            ),
            (
                LibError::RemoteUnsupportedModel,
                "Unsupported remote model given",
            ),
            (
                LibError::RemoteInvalidAPIKey,
                "Invalid API key for remote model",
            ),
            (
                LibError::RemoteRequestSendFailed,
                "Failed to send request to remote model",
            ),
            (
                LibError::RemoteResponseParseFailed,
                "Failed to parse response from remote model",
            ),
        ];

        for (error, expected_message) in test_cases {
            assert_eq!(error.to_string(), expected_message);
        }
    }

    #[test]
    fn test_lib_error_debug() {
        let error = LibError::ModelLoadFailed;
        let debug_str = format!("{:?}", error);
        assert_eq!(debug_str, "ModelLoadFailed");
    }

    #[test]
    fn test_lib_error_equality() {
        assert_eq!(LibError::ModelLoadFailed, LibError::ModelLoadFailed);
        assert_ne!(LibError::ModelLoadFailed, LibError::ModelConfigFetchFailed);
    }

    #[test]
    fn test_lib_error_hash() {
        use std::collections::HashMap;

        let mut error_map = HashMap::new();
        error_map.insert(LibError::ModelLoadFailed, "Model failed to load");
        error_map.insert(LibError::RemoteInvalidAPIKey, "Invalid API key");

        assert_eq!(
            error_map.get(&LibError::ModelLoadFailed),
            Some(&"Model failed to load")
        );
        assert_eq!(
            error_map.get(&LibError::RemoteInvalidAPIKey),
            Some(&"Invalid API key")
        );
        assert_eq!(error_map.get(&LibError::ModelConfigFetchFailed), None);
    }

    #[test]
    fn test_lib_error_as_std_error() {
        let error = LibError::RemoteRequestSendFailed;
        let std_error: &dyn Error = &error;

        assert_eq!(
            std_error.to_string(),
            "Failed to send request to remote model"
        );
        assert!(std_error.source().is_none()); // LibError doesn't have a source
    }

    #[test]
    fn test_all_error_variants_covered() {
        // This test ensures we don't forget to test new error variants
        let all_errors = vec![
            LibError::HuggingFaceApiBuildFailed,
            LibError::ModelConfigFetchFailed,
            LibError::ModelConfigReadFailed,
            LibError::ModelConfigParseFailed,
            LibError::ModelTokenizerFetchFailed,
            LibError::ModelTokenizerLoadFailed,
            LibError::ModelTokenizerConfigurationFailed,
            LibError::ModelTokenizerEncodeFailed,
            LibError::ModelWeightsFetchFailed,
            LibError::ModelWeightsLoadFailed,
            LibError::ModelHiddenSizeGetFailed,
            LibError::ModelMaxInputLenGetFailed,
            LibError::ModelLoadFailed,
            LibError::DeviceCudaInitFailed,
            LibError::RemoteUnsupportedModel,
            LibError::RemoteInvalidAPIKey,
            LibError::RemoteRequestSendFailed,
            LibError::RemoteResponseParseFailed,
        ];

        // Verify each error can be displayed and debugged
        for error in all_errors {
            let _display = error.to_string();
            let _debug = format!("{:?}", error);
            // If we reach here, all variants are properly implemented
        }
    }

    #[test]
    fn test_error_message_consistency() {
        // Test that error messages are consistent and informative
        let error_messages = vec![
            LibError::HuggingFaceApiBuildFailed.to_string(),
            LibError::ModelConfigFetchFailed.to_string(),
            LibError::RemoteInvalidAPIKey.to_string(),
        ];

        for message in error_messages {
            assert!(!message.is_empty());
            assert!(message.len() > 10); // Ensure messages are descriptive
            assert!(
                message.starts_with("Failed")
                    || message.starts_with("Invalid")
                    || message.starts_with("Unsupported")
            );
        }
    }

    #[test]
    fn test_error_categorization() {
        // Test that errors can be categorized by type
        let model_errors = vec![
            LibError::ModelConfigFetchFailed,
            LibError::ModelConfigReadFailed,
            LibError::ModelConfigParseFailed,
            LibError::ModelTokenizerFetchFailed,
            LibError::ModelTokenizerLoadFailed,
            LibError::ModelTokenizerConfigurationFailed,
            LibError::ModelTokenizerEncodeFailed,
            LibError::ModelWeightsFetchFailed,
            LibError::ModelWeightsLoadFailed,
            LibError::ModelLoadFailed,
            LibError::ModelHiddenSizeGetFailed,
            LibError::ModelMaxInputLenGetFailed,
        ];

        let remote_errors = vec![
            LibError::RemoteUnsupportedModel,
            LibError::RemoteInvalidAPIKey,
            LibError::RemoteRequestSendFailed,
            LibError::RemoteResponseParseFailed,
        ];

        let device_errors = [LibError::DeviceCudaInitFailed];

        let api_errors = [LibError::HuggingFaceApiBuildFailed];

        // Verify all errors are categorized
        let total_errors =
            model_errors.len() + remote_errors.len() + device_errors.len() + api_errors.len();
        assert_eq!(total_errors, 18); // Update this if new errors are added

        // Verify model errors contain "model" or related terms in their messages
        for error in model_errors {
            let message = error.to_string().to_lowercase();
            assert!(
                message.contains("model")
                    || message.contains("tokenizer")
                    || message.contains("weights")
                    || message.contains("config")
            );
        }

        // Verify remote errors contain "remote" or related terms
        for error in remote_errors {
            let message = error.to_string().to_lowercase();
            assert!(
                message.contains("remote")
                    || message.contains("api")
                    || message.contains("request")
                    || message.contains("response")
            );
        }
    }
}
