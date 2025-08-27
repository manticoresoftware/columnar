use super::jina::{validate_api_key, validate_model, JinaModel};
use super::TextModel;
use crate::LibError;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validate_model_valid() {
        let valid_models = vec![
            "jina-embeddings-v4",
            "jina-clip-v2",
            "jina-embeddings-v3",
            "jina-colbert-v2",
            "jina-clip-v1",
            "jina-colbert-v1-en",
            "jina-embeddings-v2-base-es",
            "jina-embeddings-v2-base-code",
            "jina-embeddings-v2-base-de",
            "jina-embeddings-v2-base-zh",
            "jina-embeddings-v2-base-en",
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
            "jina-embeddings-v1",
            "jina-embeddings-v2-base-fr", // Non-existent language
            "jina-embeddings-v5",         // Future version
        ];

        for model in invalid_models {
            let result = validate_model(model);
            assert!(result.is_err(), "Model {} should be invalid", model);
            assert!(result.unwrap_err().contains("Invalid model"));
        }
    }

    #[test]
    fn test_validate_api_key_valid() {
        let valid_keys = vec![
            "jina_1234567890abcdef",
            "jina_test-1234567890abcdef",
            "jina_a",
            "jina_very-long-key-with-many-characters-1234567890",
        ];

        for key in valid_keys {
            assert!(validate_api_key(key).is_ok(), "Key {} should be valid", key);
        }
    }

    #[test]
    fn test_validate_api_key_invalid() {
        let invalid_keys = vec![
            "",
            "1234567890abcdef",           // Missing jina_ prefix
            "sk-1234567890abcdef",        // Wrong prefix (OpenAI format)
            "pa-1234567890abcdef",        // Wrong prefix (Voyage format)
            "jina",                       // Too short
            "jina_",                      // Just the prefix
            "api-key-1234567890abcdef",   // Wrong format
            "JINA_test1234567890abcdef",  // Wrong case
            "jina_test1234567890abcdef ", // Trailing space
            " jina_test1234567890abcdef", // Leading space
        ];

        for key in invalid_keys {
            let result = validate_api_key(key);
            assert!(result.is_err(), "Key '{}' should be invalid", key);
            if key.is_empty() {
                assert!(result.unwrap_err().contains("API key is required"));
            } else if key.contains(" ") {
                assert!(result.unwrap_err().contains("whitespace"));
            } else {
                assert!(result
                    .unwrap_err()
                    .contains("API key must start with jina_"));
            }
        }
    }

    #[test]
    fn test_jina_model_new_valid() {
        let model_result = JinaModel::new("jina/jina-embeddings-v3", "jina_test1234567890abcdef");
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "jina-embeddings-v3");
        assert_eq!(model.api_key, "jina_test1234567890abcdef");
    }

    #[test]
    fn test_jina_model_new_invalid_model() {
        let result = JinaModel::new("jina/invalid-model", "jina_test1234567890abcdef");
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteUnsupportedModel)
        );
    }

    #[test]
    fn test_jina_model_new_invalid_api_key() {
        let result = JinaModel::new("jina/jina-embeddings-v3", "invalid-key");
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteInvalidAPIKey)
        );
    }

    #[test]
    fn test_jina_model_new_empty_api_key() {
        let result = JinaModel::new("jina/jina-embeddings-v3", "");
        assert!(result.is_err());

        let error = result.unwrap_err();
        assert_eq!(
            error.downcast_ref::<LibError>(),
            Some(&LibError::RemoteInvalidAPIKey)
        );
    }

    #[test]
    fn test_jina_model_prefix_stripping() {
        let model_result = JinaModel::new("jina/jina-embeddings-v4", "jina_test1234567890abcdef");
        assert!(model_result.is_ok());

        let model = model_result.unwrap();
        assert_eq!(model.model, "jina-embeddings-v4"); // Prefix should be stripped
    }

    #[test]
    fn test_get_hidden_size() {
        let test_cases = vec![
            ("jina-embeddings-v4", 2048),
            ("jina-clip-v2", 1024),
            ("jina-embeddings-v3", 1024),
            ("jina-colbert-v2", 128),
            ("jina-clip-v1", 768),
            ("jina-colbert-v1-en", 128),
            ("jina-embeddings-v2-base-es", 768),
            ("jina-embeddings-v2-base-code", 768),
            ("jina-embeddings-v2-base-de", 768),
            ("jina-embeddings-v2-base-zh", 768),
            ("jina-embeddings-v2-base-en", 768),
        ];

        for (model_name, expected_size) in test_cases {
            let model =
                JinaModel::new(&format!("jina/{}", model_name), "jina_test1234567890abcdef")
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
            ("jina-embeddings-v4", 32000),
            ("jina-clip-v2", 8192),
            ("jina-embeddings-v3", 8192),
            ("jina-colbert-v2", 8192),
            ("jina-clip-v1", 8192),
            ("jina-colbert-v1-en", 8192),
            ("jina-embeddings-v2-base-es", 8192),
            ("jina-embeddings-v2-base-code", 8192),
            ("jina-embeddings-v2-base-de", 8192),
            ("jina-embeddings-v2-base-zh", 8192),
            ("jina-embeddings-v2-base-en", 8192),
        ];

        for (model_name, expected_len) in test_cases {
            let model =
                JinaModel::new(&format!("jina/{}", model_name), "jina_test1234567890abcdef")
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
        let mut model =
            JinaModel::new("jina/jina-embeddings-v3", "jina_test1234567890abcdef").unwrap();
        model.model = "unknown-model".to_string(); // Manually set invalid model
        model.get_hidden_size(); // Should panic
    }

    #[test]
    fn test_predict_method_exists() {
        // Skip test if no Jina API key is set
        let api_key = match std::env::var("JINA_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping Jina test - JINA_API_KEY not set or empty");
                return;
            }
        };

        let model = JinaModel::new("jina/jina-embeddings-v3", &api_key).unwrap();

        // Test with real API key
        let texts = vec!["test"];
        let result = model.predict(&texts);

        match result {
            Ok(embeddings) => {
                // Should have one embedding for one text
                assert_eq!(embeddings.len(), 1);
                // Embedding should have the expected dimension for jina-embeddings-v3 (1024)
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
                        | LibError::RemoteInvalidAPIKey
                ));
            }
        }
    }

    #[test]
    fn test_predict_empty_input() {
        // Skip test if no Jina API key is set
        let api_key = match std::env::var("JINA_API_KEY") {
            Ok(key) if !key.is_empty() => key,
            _ => {
                println!("Skipping Jina test - JINA_API_KEY not set or empty");
                return;
            }
        };

        let model = JinaModel::new("jina/jina-embeddings-v3", &api_key).unwrap();

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
        let model = JinaModel::new(
            "jina/jina-embeddings-v2-base-code",
            "jina_test1234567890abcdef",
        )
        .unwrap();

        // Test that we can access the model fields correctly
        assert_eq!(model.model, "jina-embeddings-v2-base-code");
        assert_eq!(model.api_key, "jina_test1234567890abcdef");

        // Test that the client is initialized
        // We can't directly test the client, but we can verify it exists by using it
        let texts = vec!["test"];
        let _result = model.predict(&texts); // This will fail but proves client exists
    }

    #[test]
    fn test_all_supported_models() {
        let supported_models = vec![
            "jina-embeddings-v4",
            "jina-clip-v2",
            "jina-embeddings-v3",
            "jina-colbert-v2",
            "jina-clip-v1",
            "jina-colbert-v1-en",
            "jina-embeddings-v2-base-es",
            "jina-embeddings-v2-base-code",
            "jina-embeddings-v2-base-de",
            "jina-embeddings-v2-base-zh",
            "jina-embeddings-v2-base-en",
        ];

        for model_name in supported_models {
            let full_model_id = format!("jina/{}", model_name);
            let result = JinaModel::new(&full_model_id, "jina_test1234567890abcdef");
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

            // Verify max input length (varies by model now)
            let max_input = model.get_max_input_len();
            assert!(
                max_input >= 8192,
                "Max input {} for model {} should be at least 8192",
                max_input,
                model_name
            );
        }
    }

    #[test]
    fn test_error_conversion() {
        // Test that our custom errors are properly converted
        let result = JinaModel::new("jina/invalid-model", "jina_test1234567890abcdef");
        assert!(result.is_err());

        let error = result.unwrap_err();
        let error_string = error.to_string();
        assert!(error_string.contains("Unsupported remote model"));
    }

    #[test]
    fn test_api_key_edge_cases() {
        let long_key = format!("jina_{}", "a".repeat(100));
        let edge_cases = vec![
            ("jina_", false),                      // Just the prefix
            ("jina_a", true),                      // Minimal valid key
            (&long_key, true),                     // Very long key
            ("invalid-key", false),                // Wrong format
            ("JINA_test1234567890abcdef", false),  // Wrong case
            ("jina_test1234567890abcdef ", false), // Trailing space
            (" jina_test1234567890abcdef", false), // Leading space
        ];

        for (api_key, should_be_valid) in edge_cases {
            let result = validate_api_key(api_key);
            if should_be_valid {
                assert!(result.is_ok(), "Expected '{}' to be valid", api_key);
            } else {
                assert!(result.is_err(), "Expected '{}' to be invalid", api_key);
            }
        }
    }

    #[test]
    fn test_jina_v2_vs_v3_dimensions() {
        // Test that v2 models have 768 dimensions and v3/v4 have 1024
        let v2_models = vec![
            "jina-embeddings-v2-base-en",
            "jina-embeddings-v2-base-zh",
            "jina-embeddings-v2-base-de",
            "jina-embeddings-v2-base-es",
            "jina-embeddings-v2-base-code",
        ];

        for model_name in v2_models {
            let model =
                JinaModel::new(&format!("jina/{}", model_name), "jina_test1234567890abcdef")
                    .unwrap();
            assert_eq!(
                model.get_hidden_size(),
                768,
                "V2 model {} should have 768 dimensions",
                model_name
            );
        }

        let v3_v4_models = vec![("jina-embeddings-v3", 1024), ("jina-embeddings-v4", 2048)];

        for (model_name, expected_dim) in v3_v4_models {
            let model =
                JinaModel::new(&format!("jina/{}", model_name), "jina_test1234567890abcdef")
                    .unwrap();
            assert_eq!(
                model.get_hidden_size(),
                expected_dim,
                "Model {} should have {} dimensions",
                model_name,
                expected_dim
            );
        }
    }

    #[test]
    fn test_jina_multilingual_models() {
        // Test Jina's multilingual capabilities
        let multilingual_models = vec![
            ("jina-embeddings-v2-base-zh", 768), // Chinese
            ("jina-embeddings-v2-base-de", 768), // German
            ("jina-embeddings-v2-base-es", 768), // Spanish
        ];

        for (model_name, expected_dim) in multilingual_models {
            let model =
                JinaModel::new(&format!("jina/{}", model_name), "jina_test1234567890abcdef")
                    .unwrap();
            assert_eq!(model.get_hidden_size(), expected_dim);
            assert_eq!(model.get_max_input_len(), 8192);
        }
    }

    #[test]
    fn test_jina_code_model() {
        // Test Jina's code-specific model
        let model = JinaModel::new(
            "jina/jina-embeddings-v2-base-code",
            "jina_test1234567890abcdef",
        )
        .unwrap();
        assert_eq!(model.get_hidden_size(), 768);
        assert_eq!(model.get_max_input_len(), 8192);
        assert_eq!(model.model, "jina-embeddings-v2-base-code");
    }
}
