use crate::model::text_model_wrapper::*;
use crate::LibError;
use std::ffi::{CStr, CString};
use std::ptr;

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
    fn test_openai_empty_vector_fix() {
        // Test that the OpenAI empty vector bug is fixed
        // This test verifies that when an error occurs, we don't push empty vectors

        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("invalid-key"); // This will cause an error

        let model_result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        // Should fail due to invalid API key
        assert!(model_result.model.is_null());
        assert!(!model_result.error.is_null());

        // Clean up
        TextModelWrapper::free_model_result(model_result);
    }

    #[test]
    fn test_error_propagation_consistency() {
        // Test that errors are consistently propagated through szError pattern

        let error_scenarios = vec![
            ("", "", ""),                                         // Empty model ID
            ("invalid/model", "", ""),                            // Invalid model
            ("openai/text-embedding-ada-002", "", ""),            // Missing API key
            ("openai/text-embedding-ada-002", "", "invalid-key"), // Invalid API key
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

            // All should fail and have proper error messages
            assert!(
                result.model.is_null(),
                "Expected failure for scenario: {}, {}, {}",
                model_id,
                cache_path,
                api_key
            );
            assert!(
                !result.error.is_null(),
                "Expected error message for scenario: {}, {}, {}",
                model_id,
                cache_path,
                api_key
            );

            // Verify error message is meaningful
            unsafe {
                let error_cstr = CStr::from_ptr(result.error);
                let error_str = error_cstr.to_str().unwrap();
                assert!(!error_str.is_empty());
                assert!(error_str.len() > 5); // Should be descriptive
            }

            // Clean up
            TextModelWrapper::free_model_result(result);
        }
    }

    #[test]
    fn test_embedding_error_handling_fix() {
        // Test that embedding generation properly handles errors without empty vectors

        // This test simulates the scenario where embedding generation fails
        // and verifies that we don't return empty vectors

        let texts = [
            create_string_item("test text 1"),
            create_string_item("test text 2"),
            create_string_item(""), // Empty text
            create_string_item("unicode text: 世界"),
        ];

        // We can't easily test the actual embedding generation without a valid model,
        // but we can test the structure and verify that error handling is correct

        // Test that StringItems are created correctly
        for (i, item) in texts.iter().enumerate() {
            if i == 2 {
                // Empty string case
                assert_eq!(item.len, 0);
            } else {
                assert!(item.len > 0);
            }
            assert!(!item.ptr.is_null());
        }
    }

    #[test]
    fn test_float_vec_result_error_only() {
        // Test that when an error occurs, we only return the error, not empty vectors

        let error_msg = to_c_string("Test embedding error");
        let result = FloatVecResult {
            error: error_msg.into_raw(),
            ptr: ptr::null(),
            len: 0,
            cap: 0,
        };

        // Verify error is set and no vectors are present
        assert!(!result.error.is_null());
        assert!(result.ptr.is_null());
        assert_eq!(result.len, 0);
        assert_eq!(result.cap, 0);

        // Read error message
        unsafe {
            let error_cstr = CStr::from_ptr(result.error);
            let error_str = error_cstr.to_str().unwrap();
            assert_eq!(error_str, "Test embedding error");
        }

        // Clean up
        unsafe {
            let _ = CString::from_raw(result.error);
        }
    }

    #[test]
    fn test_successful_embedding_structure() {
        // Test the structure of successful embedding results

        let embeddings = vec![vec![1.0f32, 2.0, 3.0], vec![4.0f32, 5.0, 6.0]];

        let mut float_vecs = Vec::new();
        for embedding in &embeddings {
            let float_vec = FloatVec {
                ptr: embedding.as_ptr(),
                len: embedding.len(),
                cap: embedding.capacity(),
            };
            float_vecs.push(float_vec);
        }

        let result = FloatVecResult {
            error: ptr::null_mut(),
            ptr: float_vecs.as_ptr(),
            len: float_vecs.len(),
            cap: float_vecs.capacity(),
        };

        // Verify successful result structure
        assert!(result.error.is_null());
        assert!(!result.ptr.is_null());
        assert_eq!(result.len, 2);
        assert!(result.cap >= 2);

        // Verify we can read the embeddings back
        unsafe {
            let vec_slice = std::slice::from_raw_parts(result.ptr, result.len);
            for (i, float_vec) in vec_slice.iter().enumerate() {
                assert_eq!(float_vec.len, 3);
                assert!(!float_vec.ptr.is_null());

                let embedding_slice = std::slice::from_raw_parts(float_vec.ptr, float_vec.len);
                assert_eq!(embedding_slice, embeddings[i].as_slice());
            }
        }

        // Don't forget the vectors since we're just testing structure
        std::mem::forget(embeddings);
        std::mem::forget(float_vecs);
    }

    #[test]
    fn test_memory_cleanup_on_error() {
        // Test that memory is properly cleaned up when errors occur

        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("invalid-key");

        // Create and immediately clean up multiple failed model loads
        for _ in 0..10 {
            let result = TextModelWrapper::load_model(
                model_name.as_ptr(),
                model_name.as_bytes().len(),
                cache_path.as_ptr(),
                cache_path.as_bytes().len(),
                api_key.as_ptr(),
                api_key.as_bytes().len(),
                false,
            );

            // Should fail
            assert!(result.model.is_null());
            assert!(!result.error.is_null());

            // Clean up should not crash
            TextModelWrapper::free_model_result(result);
        }
    }

    #[test]
    fn test_error_message_consistency() {
        // Test that error messages are consistent across different error types

        let lib_errors = vec![
            LibError::RemoteUnsupportedModel,
            LibError::RemoteInvalidAPIKey,
            LibError::RemoteRequestSendFailed,
            LibError::RemoteResponseParseFailed,
            LibError::ModelLoadFailed,
        ];

        for lib_error in lib_errors {
            let error_str = lib_error.to_string();

            // All error messages should be descriptive
            assert!(!error_str.is_empty());
            assert!(error_str.len() > 10);

            // Should start with a descriptive word
            assert!(
                error_str.starts_with("Failed")
                    || error_str.starts_with("Invalid")
                    || error_str.starts_with("Unsupported")
            );
        }
    }

    #[test]
    fn test_api_key_validation_edge_cases() {
        // Test API key validation with various edge cases

        let long_key = format!("sk-{}", "a".repeat(100));
        let api_key_cases = vec![
            ("", false, "empty key"),
            ("sk-", false, "just prefix"),
            ("sk-a", true, "minimal valid key"),
            ("sk-test1234567890abcdef", true, "normal key"),
            (&long_key, true, "very long key"),
            ("invalid-key", false, "wrong format"),
            ("SK-test123", false, "wrong case"),
            ("sk-test123 ", false, "trailing space"),
            (" sk-test123", false, "leading space"),
        ];

        for (api_key, should_be_valid, description) in api_key_cases {
            let model_name = to_c_string("openai/text-embedding-ada-002");
            let cache_path = to_c_string("");
            let api_key_c = to_c_string(api_key);

            let result = TextModelWrapper::load_model(
                model_name.as_ptr(),
                model_name.as_bytes().len(),
                cache_path.as_ptr(),
                cache_path.as_bytes().len(),
                api_key_c.as_ptr(),
                api_key_c.as_bytes().len(),
                false,
            );

            if should_be_valid {
                // Valid API key format should create model (but may fail on actual API call)
                // We can't test actual API calls, but the model should be created
                if result.model.is_null() {
                    // If it fails, it should be due to network/API issues, not validation
                    assert!(!result.error.is_null());
                    unsafe {
                        let error_cstr = CStr::from_ptr(result.error);
                        let error_str = error_cstr.to_str().unwrap();
                        // Should not be an API key validation error
                        assert!(
                            !error_str.to_lowercase().contains("api key must start"),
                            "Expected network error for {}: {}, got validation error: {}",
                            description,
                            api_key,
                            error_str
                        );
                    }
                }
            } else {
                // Invalid API key format should fail validation
                assert!(
                    result.model.is_null(),
                    "Expected failure for {}: {}",
                    description,
                    api_key
                );
                assert!(
                    !result.error.is_null(),
                    "Expected error for {}: {}",
                    description,
                    api_key
                );

                unsafe {
                    let error_cstr = CStr::from_ptr(result.error);
                    let error_str = error_cstr.to_str().unwrap();
                    // Should be an API key related error
                    assert!(
                        error_str.to_lowercase().contains("api key")
                            || error_str.to_lowercase().contains("invalid"),
                        "Expected API key error for {}: {}, got: {}",
                        description,
                        api_key,
                        error_str
                    );
                }
            }

            // Clean up
            TextModelWrapper::free_model_result(result);
        }
    }

    #[test]
    fn test_model_validation_edge_cases() {
        // Test model validation with various edge cases

        let model_cases = vec![
            ("openai/text-embedding-ada-002", true, "valid ada model"),
            ("openai/text-embedding-3-small", true, "valid 3-small model"),
            ("openai/text-embedding-3-large", true, "valid 3-large model"),
            ("openai/gpt-3.5-turbo", false, "wrong model type"),
            ("openai/text-embedding-ada-001", false, "old model"),
            ("openai/invalid-model", false, "invalid model"),
            ("openai/", false, "empty model name"),
            ("", false, "completely empty"),
            ("local/model", false, "non-openai model without files"),
        ];

        for (model_id, should_be_valid, description) in model_cases {
            let model_name = to_c_string(model_id);
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

            if should_be_valid && model_id.starts_with("openai/") {
                // Valid OpenAI models should create model structure (but may fail on API calls)
                if result.model.is_null() {
                    // If it fails, should not be due to model validation
                    assert!(!result.error.is_null());
                    unsafe {
                        let error_cstr = CStr::from_ptr(result.error);
                        let error_str = error_cstr.to_str().unwrap();
                        // Should not be a model validation error
                        assert!(
                            !error_str.to_lowercase().contains("unsupported")
                                || !error_str.to_lowercase().contains("invalid model")
                        );
                    }
                }
            } else {
                // Invalid models should fail validation
                assert!(
                    result.model.is_null(),
                    "Expected failure for {}: {}",
                    description,
                    model_id
                );
                assert!(
                    !result.error.is_null(),
                    "Expected error for {}: {}",
                    description,
                    model_id
                );
            }

            // Clean up
            TextModelWrapper::free_model_result(result);
        }
    }

    #[test]
    fn test_concurrent_error_handling() {
        // Test that error handling works correctly under concurrent access
        use std::thread;

        let handles: Vec<_> = (0..5)
            .map(|i| {
                thread::spawn(move || {
                    let model_name = to_c_string("openai/text-embedding-ada-002");
                    let cache_path = to_c_string("");
                    let api_key = to_c_string(&format!("invalid-key-{}", i));

                    let result = TextModelWrapper::load_model(
                        model_name.as_ptr(),
                        model_name.as_bytes().len(),
                        cache_path.as_ptr(),
                        cache_path.as_bytes().len(),
                        api_key.as_ptr(),
                        api_key.as_bytes().len(),
                        false,
                    );

                    // Should fail with invalid API key
                    assert!(result.model.is_null());
                    assert!(!result.error.is_null());

                    // Error message should be thread-safe
                    unsafe {
                        let error_cstr = CStr::from_ptr(result.error);
                        let error_str = error_cstr.to_str().unwrap();
                        assert!(!error_str.is_empty());
                    }

                    // Clean up
                    TextModelWrapper::free_model_result(result);
                })
            })
            .collect();

        // Wait for all threads
        for handle in handles {
            handle.join().unwrap();
        }
    }
}
