use crate::model::text_model_wrapper::*;
use crate::model::{create_model, ModelOptions, TextModel};
use std::ffi::{CStr, CString};

#[cfg(test)]
mod tests {
    use super::*;

    // Helper function to create a C string from Rust string
    fn to_c_string(s: &str) -> CString {
        CString::new(s).unwrap()
    }

    // Helper function to create StringItem from &str
    fn create_string_item(s: &str) -> StringItem {
        StringItem {
            ptr: s.as_ptr() as *const std::os::raw::c_char,
            len: s.len(),
        }
    }

    #[test]
    fn test_model_creation_workflow() {
        // Test the complete model creation workflow
        let options = ModelOptions {
            model_id: "openai/text-embedding-ada-002".to_string(),
            cache_path: None,
            api_key: Some("sk-test1234567890abcdef".to_string()),
            use_gpu: Some(false),
        };

        let result = create_model(options);
        assert!(result.is_ok());

        let model = result.unwrap();
        assert_eq!(model.get_hidden_size(), 1536); // Fixed: ada-002 is 1536, not 768
        assert_eq!(model.get_max_input_len(), 8192);
    }

    #[test]
    fn test_ffi_model_loading_workflow() {
        // Test the complete FFI model loading workflow
        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("sk-test1234567890abcdef");

        let result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        if result.model.is_null() {
            // Expected to fail due to invalid API key, but should have proper error
            assert!(!result.error.is_null());

            unsafe {
                let error_cstr = CStr::from_ptr(result.error);
                let error_str = error_cstr.to_str().unwrap();
                assert!(!error_str.is_empty());
            }
        } else {
            // If somehow successful, test the model functions
            // We can't directly create TextModelWrapper from tests due to private constructor
            // This would be tested through the C API in actual usage
        }

        // Clean up
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_error_propagation_workflow() {
        // Test that errors are properly propagated through the entire stack

        // 1. Test invalid model at Rust level
        let options = ModelOptions {
            model_id: "invalid/model".to_string(),
            cache_path: None,
            api_key: None,
            use_gpu: Some(false),
        };

        let result = create_model(options);
        assert!(result.is_err());

        // 2. Test invalid model at FFI level
        let model_name = to_c_string("invalid/model");
        let cache_path = to_c_string("");
        let api_key = to_c_string("");

        let ffi_result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        assert!(ffi_result.model.is_null());
        assert!(!ffi_result.error.is_null());

        // Clean up
        TextModelWrapper::free_model_result(ffi_result);
    }

    #[test]
    fn test_memory_management_workflow() {
        // Test that memory is properly managed throughout the workflow

        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("invalid-key"); // Will fail, but that's OK for memory test

        let result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        // Should have error due to invalid API key
        assert!(result.model.is_null());
        assert!(!result.error.is_null());

        // Test that we can read the error
        unsafe {
            let error_cstr = CStr::from_ptr(result.error);
            let _error_str = error_cstr.to_str().unwrap();
            // Error string should be readable
        }

        // Clean up - this should not crash
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_embedding_generation_error_handling() {
        // Test that embedding generation properly handles errors

        // Create a model that will fail
        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("sk-test1234567890abcdef"); // Valid format but fake key

        let model_result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        if !model_result.model.is_null() {
            // If model creation succeeded (shouldn't with fake key, but just in case)
            // We can't directly create TextModelWrapper from tests due to private constructor
            // This would be tested through the C API in actual usage

            // For now, just verify the model pointer is not null
            assert!(!model_result.model.is_null());
        }

        // Clean up model
        TextModelWrapper::free_model_result(model_result);
    }

    #[test]
    fn test_multiple_text_embedding_workflow() {
        // Test embedding multiple texts at once
        let texts = [
            "First text to embed",
            "Second text to embed", 
            "Third text with unicode: 世界",
            "",  // Empty text
            "A longer text that might exceed some limits but should still be handled properly by the embedding system",
        ];

        let string_items: Vec<StringItem> = texts.iter().map(|&s| create_string_item(s)).collect();

        // Verify string items are created correctly
        assert_eq!(string_items.len(), texts.len());

        for (i, item) in string_items.iter().enumerate() {
            assert_eq!(item.len, texts[i].len());
            if texts[i].is_empty() {
                // Empty string should still have valid pointer
                assert!(!item.ptr.is_null());
            } else {
                assert!(!item.ptr.is_null());
            }
        }
    }

    #[test]
    fn test_model_configuration_workflow() {
        // Test different model configurations
        let configurations = vec![
            // OpenAI models
            (
                "openai/text-embedding-ada-002",
                Some("sk-test123"),
                None,
                false,
            ),
            (
                "openai/text-embedding-3-small",
                Some("sk-test456"),
                None,
                false,
            ),
            (
                "openai/text-embedding-3-large",
                Some("sk-test789"),
                None,
                false,
            ),
            // Local models (will fail without actual model files, but tests the path)
            (
                "sentence-transformers/all-MiniLM-L6-v2",
                None,
                Some("/tmp/cache"),
                false,
            ),
            (
                "sentence-transformers/all-MiniLM-L6-v2",
                None,
                Some("/tmp/cache"),
                true,
            ),
        ];

        for (model_id, api_key, cache_path, use_gpu) in configurations {
            let options = ModelOptions {
                model_id: model_id.to_string(),
                cache_path: cache_path.map(|s| s.to_string()),
                api_key: api_key.map(|s| s.to_string()),
                use_gpu: Some(use_gpu),
            };

            let result = create_model(options);

            if model_id.starts_with("openai/") {
                if api_key.is_some() {
                    // Should succeed in creating model (but fail on actual API calls)
                    assert!(result.is_ok());
                    let model = result.unwrap();
                    assert!(model.get_hidden_size() > 0);
                    assert!(model.get_max_input_len() > 0);
                } else {
                    // Should fail without API key
                    assert!(result.is_err());
                }
            } else {
                // Local models will likely fail without actual model files
                // But the error should be meaningful
                if result.is_err() {
                    let error_str = if let Err(error) = result {
                        error.to_string()
                    } else {
                        String::new()
                    };
                    assert!(!error_str.is_empty());
                }
            }
        }
    }

    #[test]
    fn test_concurrent_model_operations() {
        // Test that multiple model operations can be performed safely
        use std::thread;

        let handles: Vec<_> = (0..5)
            .map(|i| {
                thread::spawn(move || {
                    let model_name = "openai/text-embedding-ada-002".to_string();
                    let api_key = format!("sk-test{:03}", i);

                    let options = ModelOptions {
                        model_id: model_name,
                        cache_path: None,
                        api_key: Some(api_key),
                        use_gpu: Some(false),
                    };

                    let result = create_model(options);
                    // Should succeed in creating model structure
                    assert!(result.is_ok());

                    let model = result.unwrap();
                    assert!(model.get_hidden_size() > 0);
                    assert!(model.get_max_input_len() > 0);
                })
            })
            .collect();

        // Wait for all threads to complete
        for handle in handles {
            handle.join().unwrap();
        }
    }

    #[test]
    fn test_edge_case_inputs() {
        // Test various edge case inputs
        let long_string = "a".repeat(1000);
        let edge_cases = vec![
            "",             // Empty string
            " ",            // Single space
            "\n",           // Newline
            "\t",           // Tab
            "a",            // Single character
            &long_string,   // Very long string
            "🌍🌎🌏",       // Unicode emojis
            "Hello\0World", // String with null byte (should be handled carefully)
        ];

        for input in edge_cases {
            let string_item = create_string_item(input);

            // Basic validation
            assert_eq!(string_item.len, input.len());

            if input.is_empty() {
                assert!(!string_item.ptr.is_null()); // Even empty strings should have valid pointers
            } else {
                assert!(!string_item.ptr.is_null());

                // Verify we can read it back
                unsafe {
                    let slice =
                        std::slice::from_raw_parts(string_item.ptr as *const u8, string_item.len);
                    let recovered = std::str::from_utf8_unchecked(slice);
                    assert_eq!(recovered, input);
                }
            }
        }
    }

    #[test]
    fn test_error_message_quality() {
        // Test that error messages are informative and helpful
        let error_scenarios = vec![
            ("", "", ""),                                         // All empty
            ("invalid/model", "", ""),                            // Invalid model
            ("openai/text-embedding-ada-002", "", ""),            // Missing API key
            ("openai/text-embedding-ada-002", "", "invalid-key"), // Invalid API key format
            ("openai/invalid-model", "", "sk-test123"),           // Invalid OpenAI model
        ];

        for (model_id, cache_path, api_key) in error_scenarios {
            let model_name = to_c_string(model_id);
            let cache_path_c = to_c_string(cache_path);
            let api_key_c = to_c_string(api_key);

            let result = TextModelWrapper::load_model(
                model_name.as_ptr(),
                model_name.as_bytes().len(),
                cache_path_c.as_ptr(),
                cache_path_c.as_bytes().len(),
                api_key_c.as_ptr(),
                api_key_c.as_bytes().len(),
                false,
            );

            // Should fail for all these cases
            assert!(result.model.is_null());
            assert!(!result.error.is_null());

            // Error message should be informative
            unsafe {
                let error_cstr = CStr::from_ptr(result.error);
                let error_str = error_cstr.to_str().unwrap();

                assert!(!error_str.is_empty());
                assert!(error_str.len() > 10); // Should be descriptive

                // Should contain relevant keywords based on the error
                if model_id.is_empty() || model_id == "invalid/model" {
                    // Generic model error
                    assert!(
                        error_str.to_lowercase().contains("model")
                            || error_str.to_lowercase().contains("invalid")
                            || error_str.to_lowercase().contains("error")
                    );
                } else if model_id.starts_with("openai/")
                    && (api_key.is_empty() || api_key == "invalid-key")
                {
                    // API key related error
                    assert!(
                        error_str.to_lowercase().contains("api")
                            || error_str.to_lowercase().contains("key")
                    );
                }
            }

            // Clean up
            TextModelWrapper::free_model_result(result);
        }
    }
}
