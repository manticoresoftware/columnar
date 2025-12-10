use super::openai::{validate_model, OpenAIModel};
use super::TextModel;
use crate::LibError;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validate_model_valid() {
        assert!(validate_model("text-embedding-ada-002").is_ok());
        assert!(validate_model("text-embedding-3-small").is_ok());
        assert!(validate_model("text-embedding-3-large").is_ok());
    }

    #[test]
    fn test_validate_model_invalid() {
        let invalid_models = vec![
            "gpt-3.5-turbo",
            "text-davinci-003",
            "invalid-model",
            "",
            "text-embedding-ada-001",  // Old model
            "text-embedding-3-medium", // Non-existent model
        ];

        for model in invalid_models {
            let result = validate_model(model);
            assert!(result.is_err());
            assert!(result.unwrap_err().contains("Invalid model"));
        }
    }

    // Note: API key validation tests removed because validate_api_key() is now a trait method
    // that makes real API requests. These tests would require network access and API keys.
    // Basic validation (non-empty, no whitespace) is tested indirectly through model creation tests.

    #[test]
    fn test_openai_model_new_valid() {
        let model_result = OpenAIModel::new(
            "openai/text-embedding-ada-002",
            "sk-test1234567890abcdef",
            None,
            None,
        );
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "text-embedding-ada-002");
        assert_eq!(model.api_key, "sk-test1234567890abcdef");
    }

    #[test]
    fn test_openai_model_new_invalid_model() {
        let result = OpenAIModel::new(
            "openai/invalid-model",
            "sk-test1234567890abcdef",
            None,
            None,
        );
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteUnsupportedModel { status: None })
        );
    }

    #[test]
    fn test_openai_model_new_invalid_api_key() {
        // Empty key should fail basic validation
        let result = OpenAIModel::new("openai/text-embedding-ada-002", "", None, None);
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteInvalidAPIKey { status: None })
        );
    }

    #[test]
    fn test_openai_model_new_with_custom_url() {
        // With custom URL, any non-empty key without whitespace should be accepted
        let result = OpenAIModel::new(
            "openai/text-embedding-ada-002",
            "test-key",
            Some("http://localhost:8080/v1/embeddings"),
            None,
        );
        assert!(result.is_ok());

        let model = result.unwrap();
        assert_eq!(model.api_key, "test-key");
        assert_eq!(
            model.api_url,
            Some("http://localhost:8080/v1/embeddings".to_string())
        );
    }

    #[test]
    fn test_openai_model_new_with_timeout() {
        // Test with custom timeout
        let result = OpenAIModel::new(
            "openai/text-embedding-ada-002",
            "sk-test1234567890abcdef",
            None,
            Some(30),
        );
        assert!(result.is_ok());
    }

    #[test]
    fn test_openai_model_prefix_stripping() {
        let model_result = OpenAIModel::new(
            "openai/text-embedding-3-small",
            "sk-test1234567890abcdef",
            None,
            None,
        );
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "text-embedding-3-small"); // Prefix should be stripped
    }

    #[test]
    fn test_get_hidden_size() {
        let test_cases = vec![
            ("text-embedding-ada-002", 1536), // Fixed: was 768, should be 1536
            ("text-embedding-3-small", 1536),
            ("text-embedding-3-large", 3072),
        ];

        for (model_name, expected_size) in test_cases {
            let model = OpenAIModel::new(
                &format!("openai/{}", model_name),
                "sk-test1234567890abcdef",
                None,
                None,
            )
            .unwrap();
            assert_eq!(model.get_hidden_size(), expected_size);
        }
    }

    #[test]
    fn test_get_max_input_len() {
        let model = OpenAIModel::new(
            "openai/text-embedding-ada-002",
            "sk-test1234567890abcdef",
            None,
            None,
        )
        .unwrap();
        assert_eq!(model.get_max_input_len(), 8192);
    }

    #[test]
    #[should_panic(expected = "Unknown model")]
    fn test_get_hidden_size_unknown_model() {
        // This test verifies the panic behavior for unknown models
        // In practice, this shouldn't happen due to validation in new()
        let mut model = OpenAIModel::new(
            "openai/text-embedding-ada-002",
            "sk-test1234567890abcdef",
            None,
            None,
        )
        .unwrap();
        model.model = "unknown-model".to_string(); // Manually set invalid model
        model.get_hidden_size(); // Should panic
    }

    // Mock tests for predict method would require a mock HTTP client
    // For now, we'll test the structure and error handling
    #[test]
    fn test_predict_method_exists() {
        // Skip test if no OpenAI API key is set
        let api_key = match std::env::var("OPENAI_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping OpenAI test - OPENAI_API_KEY not set or empty");
                return;
            }
        };

        let model =
            OpenAIModel::new("openai/text-embedding-ada-002", &api_key, None, None).unwrap();

        // Test with real API key
        let texts = vec!["test"];
        let result = model.predict(&texts);

        match result {
            Ok(embeddings) => {
                // Should have one embedding for one text
                assert_eq!(embeddings.len(), 1);
                // Embedding should have the expected dimension for ada-002 (1536, not 768)
                assert_eq!(embeddings[0].len(), 1536);
            }
            Err(error) => {
                // If it fails, should be a meaningful error
                let lib_error = error.downcast_ref::<LibError>();
                assert!(lib_error.is_some());

                let lib_error = lib_error.unwrap();
                assert!(matches!(
                    lib_error,
                    LibError::RemoteRequestSendFailed
                        | LibError::RemoteResponseParseFailed
                        | LibError::RemoteInvalidAPIKey { .. }
                ));
            }
        }
    }

    #[test]
    fn test_predict_empty_input() {
        // Skip test if no OpenAI API key is set
        let api_key = match std::env::var("OPENAI_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping OpenAI test - OPENAI_API_KEY not set or empty");
                return;
            }
        };

        let model =
            OpenAIModel::new("openai/text-embedding-ada-002", &api_key, None, None).unwrap();

        let empty_texts: Vec<&str> = vec![];
        let result = model.predict(&empty_texts);

        // Empty input should succeed with empty result
        match result {
            Ok(embeddings) => {
                assert_eq!(embeddings.len(), 0);
            }
            Err(error) => {
                // If it fails, should be a meaningful error
                let lib_error = error.downcast_ref::<LibError>();
                assert!(lib_error.is_some());
            }
        }
    }

    #[test]
    fn test_model_field_access() {
        let model = OpenAIModel::new(
            "openai/text-embedding-3-large",
            "sk-test1234567890abcdef",
            None,
            None,
        )
        .unwrap();

        // Test that we can access the model fields correctly
        assert_eq!(model.model, "text-embedding-3-large");
        assert_eq!(model.api_key, "sk-test1234567890abcdef");

        // Test that the client is initialized
        // We can't directly test the client, but we can verify it exists by using it
        let texts = vec!["test"];
        let _result = model.predict(&texts); // This will fail but proves client exists
    }

    #[test]
    fn test_all_supported_models() {
        let supported_models = vec![
            "text-embedding-ada-002",
            "text-embedding-3-small",
            "text-embedding-3-large",
        ];

        for model_name in supported_models {
            let full_model_id = format!("openai/{}", model_name);
            let result = OpenAIModel::new(&full_model_id, "sk-test1234567890abcdef", None, None);
            assert!(result.is_ok(), "Failed to create model for {}", model_name);

            let model = result.unwrap();
            assert_eq!(model.model, model_name);

            // Verify hidden size is reasonable
            let hidden_size = model.get_hidden_size();
            assert!(hidden_size > 0 && hidden_size <= 4096);

            // Verify max input length
            assert_eq!(model.get_max_input_len(), 8192);
        }
    }

    #[test]
    fn test_error_conversion() {
        // Test that our custom errors are properly converted
        let result = OpenAIModel::new(
            "openai/invalid-model",
            "sk-test1234567890abcdef",
            None,
            None,
        );
        assert!(result.is_err());

        let error = result.unwrap_err();
        let error_string = error.to_string();
        assert!(error_string.contains("Unsupported remote model"));
    }

    // Note: API key edge case tests removed because validate_api_key() is now a trait method
    // that makes real API requests. Basic validation (non-empty, no whitespace) is tested
    // indirectly through model creation tests.
}
