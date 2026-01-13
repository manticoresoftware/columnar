use super::voyage::{validate_model, VoyageModel};
use super::TextModel;
use crate::LibError;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validate_model_valid() {
        let valid_models = vec![
            "voyage-3-large",
            "voyage-3.5",
            "voyage-3.5-lite",
            "voyage-code-3",
            "voyage-finance-2",
            "voyage-law-2",
            "voyage-code-2",
        ];

        for model in valid_models {
            assert!(
                validate_model(model).is_ok(),
                "Model {} should be valid",
                model
            );
        }
    }

    #[test]
    fn test_validate_model_invalid() {
        let invalid_models = vec![
            "gpt-3.5-turbo",
            "text-embedding-ada-002",
            "invalid-model",
            "",
            "voyage-1",
            "voyage-unknown",
            "voyage-3-medium",       // Non-existent model
            "voyage-3-lite",         // Removed model
            "voyage-large-2",        // Removed model
            "voyage-multilingual-2", // Removed model
        ];

        for model in invalid_models {
            let result = validate_model(model);
            assert!(result.is_err(), "Model {} should be invalid", model);
            assert!(result.unwrap_err().contains("Invalid model"));
        }
    }

    #[test]
    fn test_voyage_model_new_valid() {
        let model_result = VoyageModel::new(
            "voyage/voyage-3-large",
            "pa-test1234567890abcdef",
            None,
            None,
        );
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "voyage-3-large");
        assert_eq!(model.api_key, "pa-test1234567890abcdef");
    }

    #[test]
    fn test_voyage_model_new_invalid_model() {
        let result = VoyageModel::new(
            "voyage/invalid-model",
            "pa-test1234567890abcdef",
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
    fn test_voyage_model_new_invalid_api_key() {
        // Empty key will fail basic validation
        let result = VoyageModel::new("voyage/voyage-3-large", "", None, None);
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteInvalidAPIKey { status: None })
        );
    }

    #[test]
    fn test_voyage_model_new_empty_api_key() {
        let result = VoyageModel::new("voyage/voyage-3-large", "", None, None);
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteInvalidAPIKey { status: None })
        );
    }

    #[test]
    fn test_voyage_model_prefix_stripping() {
        let model_result =
            VoyageModel::new("voyage/voyage-3.5", "pa-test1234567890abcdef", None, None);
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "voyage-3.5"); // Prefix should be stripped
    }

    #[test]
    fn test_get_hidden_size() {
        let test_cases = vec![
            ("voyage-3-large", 1024),
            ("voyage-3.5", 1024),
            ("voyage-3.5-lite", 1024),
            ("voyage-code-3", 1024),
            ("voyage-finance-2", 1024),
            ("voyage-law-2", 1024),
            ("voyage-code-2", 1536),
        ];

        for (model_name, expected_size) in test_cases {
            let model = VoyageModel::new(
                &format!("voyage/{}", model_name),
                "pa-test1234567890abcdef",
                None,
                None,
            )
            .unwrap();
            assert_eq!(
                model.get_hidden_size(),
                expected_size,
                "Model {} should have size {}",
                model_name,
                expected_size
            );
        }
    }

    #[test]
    fn test_get_max_input_len() {
        let test_cases = vec![
            ("voyage-3-large", 32000),
            ("voyage-3.5", 32000),
            ("voyage-3.5-lite", 32000),
            ("voyage-code-3", 32000),
            ("voyage-finance-2", 32000),
            ("voyage-law-2", 16000),
            ("voyage-code-2", 16000),
        ];

        for (model_name, expected_len) in test_cases {
            let model = VoyageModel::new(
                &format!("voyage/{}", model_name),
                "pa-test1234567890abcdef",
                None,
                None,
            )
            .unwrap();
            assert_eq!(
                model.get_max_input_len(),
                expected_len,
                "Model {} should have max input len {}",
                model_name,
                expected_len
            );
        }
    }

    #[test]
    #[should_panic(expected = "Unknown model")]
    fn test_get_hidden_size_unknown_model() {
        // This test verifies the panic behavior for unknown models
        // In practice, this shouldn't happen due to validation in new()
        let mut model = VoyageModel::new(
            "voyage/voyage-3-large",
            "pa-test1234567890abcdef",
            None,
            None,
        )
        .unwrap();
        model.model = "unknown-model".to_string(); // Manually set invalid model
        model.get_hidden_size(); // Should panic
    }

    #[test]
    fn test_predict_method_exists() {
        // Skip test if no Voyage API key is set
        let api_key = match std::env::var("VOYAGE_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping Voyage test - VOYAGE_API_KEY not set or empty");
                return;
            }
        };

        let model = VoyageModel::new("voyage/voyage-3-large", &api_key, None, None).unwrap();

        // Test with real API key
        let texts = vec!["test"];
        let result = model.predict(&texts);

        match result {
            Ok(embeddings) => {
                // Should have one embedding for one text
                assert_eq!(embeddings.len(), 1);
                // Embedding should have the expected dimension for voyage-3-large (1024)
                assert_eq!(embeddings[0].len(), 1024);
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
        // Skip test if no Voyage API key is set
        let api_key = match std::env::var("VOYAGE_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping Voyage test - VOYAGE_API_KEY not set or empty");
                return;
            }
        };

        let model = VoyageModel::new("voyage/voyage-3-large", &api_key, None, None).unwrap();

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
        let model = VoyageModel::new(
            "voyage/voyage-code-2",
            "pa-test1234567890abcdef",
            None,
            None,
        )
        .unwrap();

        // Test that we can access the model fields correctly
        assert_eq!(model.model, "voyage-code-2");
        assert_eq!(model.api_key, "pa-test1234567890abcdef");

        // Test that the client is initialized
        // We can't directly test the client, but we can verify it exists by using it
        let texts = vec!["test"];
        let _result = model.predict(&texts); // This will fail but proves client exists
    }

    #[test]
    fn test_all_supported_models() {
        let supported_models = vec![
            "voyage-3-large",
            "voyage-3.5",
            "voyage-3.5-lite",
            "voyage-code-3",
            "voyage-finance-2",
            "voyage-law-2",
            "voyage-code-2",
        ];

        for model_name in supported_models {
            let full_model_id = format!("voyage/{}", model_name);
            let result = VoyageModel::new(&full_model_id, "pa-test1234567890abcdef", None, None);
            assert!(result.is_ok(), "Failed to create model for {}", model_name);

            let model = result.unwrap();
            assert_eq!(model.model, model_name);

            // Verify hidden size is reasonable
            let hidden_size = model.get_hidden_size();
            assert!(
                hidden_size > 0 && hidden_size <= 2048,
                "Hidden size {} for model {} is not reasonable",
                hidden_size,
                model_name
            );

            // Verify max input length is reasonable
            let max_input = model.get_max_input_len();
            assert!(
                max_input >= 8192,
                "Max input {} for model {} is too small",
                max_input,
                model_name
            );
        }
    }

    #[test]
    fn test_error_conversion() {
        // Test that our custom errors are properly converted
        let result = VoyageModel::new(
            "voyage/invalid-model",
            "pa-test1234567890abcdef",
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

    #[test]
    fn test_voyage_specific_models() {
        // Test Voyage-specific models that don't exist in other providers
        let voyage_specific = vec![
            ("voyage-law-2", 1024, 16000),
            ("voyage-code-2", 1536, 16000),
            ("voyage-finance-2", 1024, 32000),
            ("voyage-code-3", 1024, 32000),
        ];

        for (model_name, expected_dim, expected_max_len) in voyage_specific {
            let model = VoyageModel::new(
                &format!("voyage/{}", model_name),
                "pa-test1234567890abcdef",
                None,
                None,
            )
            .unwrap();
            assert_eq!(model.get_hidden_size(), expected_dim);
            assert_eq!(model.get_max_input_len(), expected_max_len);
        }
    }
}
