use super::text_model_wrapper::*;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
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
            ptr: s.as_ptr() as *const c_char,
            len: s.len(),
        }
    }

    fn run_concurrent_ffi_embeddings(model_id: &str) {
        use std::sync::{Arc, Barrier};
        use std::thread;

        let model_name = to_c_string(model_id);
        let cache_path = to_c_string("");
        let api_key = to_c_string("");

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
            let error_message = if result.error.is_null() {
                "unknown error".to_string()
            } else {
                unsafe {
                    CStr::from_ptr(result.error)
                        .to_str()
                        .unwrap_or("unknown error")
                        .to_string()
                }
            };
            TextModelWrapper::free_model_result(result);
            panic!("failed to load model {}: {}", model_id, error_message);
        }

        let model_ptr = result.model as usize;
        let start = Arc::new(Barrier::new(4));
        let handles: Vec<_> = (0..3)
            .map(|i| {
                let start = Arc::clone(&start);
                let model_id = model_id.to_string();
                thread::spawn(move || {
                    start.wait();
                    let text = format!("Concurrent embedding test {} - {}", model_id, i);
                    let item = create_string_item(&text);
                    let items = [item];

                    // Safety: emulate FFI callers that share a model pointer across threads.
                    let wrapper = unsafe {
                        std::mem::transmute::<*mut std::ffi::c_void, TextModelWrapper>(
                            model_ptr as *mut std::ffi::c_void,
                        )
                    };
                    let vec_result =
                        TextModelWrapper::make_vect_embeddings(&wrapper, items.as_ptr(), 1);
                    assert!(vec_result.error.is_null());
                    assert_eq!(vec_result.len, 1);
                    TextModelWrapper::free_vec_result(vec_result);
                })
            })
            .collect();

        start.wait();
        for handle in handles {
            handle.join().unwrap();
        }

        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_text_model_result_structure() {
        // Test that TextModelResult has the expected structure
        let result = TextModelResult {
            model: ptr::null_mut(),
            error: ptr::null_mut(),
        };

        assert!(result.model.is_null());
        assert!(result.error.is_null());
    }

    #[test]
    fn test_float_vec_structure() {
        let vec = vec![1.0f32, 2.0, 3.0];
        let float_vec = FloatVec {
            ptr: vec.as_ptr(),
            len: vec.len(),
            cap: vec.capacity(),
        };

        assert_eq!(float_vec.len, 3);
        assert!(float_vec.cap >= 3);
        assert!(!float_vec.ptr.is_null());

        // Verify we can read the data back
        unsafe {
            let slice = std::slice::from_raw_parts(float_vec.ptr, float_vec.len);
            assert_eq!(slice, &[1.0, 2.0, 3.0]);
        }
    }

    #[test]
    fn test_float_vec_result_structure() {
        let result = FloatVecResult {
            error: ptr::null_mut(),
            ptr: ptr::null(),
            len: 0,
            cap: 0,
        };

        assert!(result.error.is_null());
        assert!(result.ptr.is_null());
        assert_eq!(result.len, 0);
        assert_eq!(result.cap, 0);
    }

    #[test]
    fn test_string_item_structure() {
        let test_str = "hello world";
        let string_item = create_string_item(test_str);

        assert_eq!(string_item.len, test_str.len());
        assert!(!string_item.ptr.is_null());

        // Verify we can read the string back
        unsafe {
            let slice = std::slice::from_raw_parts(string_item.ptr as *const u8, string_item.len);
            let recovered_str = std::str::from_utf8_unchecked(slice);
            assert_eq!(recovered_str, test_str);
        }
    }

    #[test]
    fn test_load_model_invalid_model() {
        let model_name = to_c_string("invalid/model");
        let cache_path = to_c_string("");
        let api_key = to_c_string("");

        let result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        // Should fail with invalid model
        assert!(result.model.is_null());
        assert!(!result.error.is_null());

        // Check error message
        unsafe {
            let error_cstr = CStr::from_ptr(result.error);
            let error_str = error_cstr.to_str().unwrap();
            assert!(!error_str.is_empty());
        }

        // Clean up
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_load_model_openai_invalid_api_key() {
        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("invalid-key");

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

        // Check error message contains API key error
        unsafe {
            let error_cstr = CStr::from_ptr(result.error);
            let error_str = error_cstr.to_str().unwrap();
            assert!(error_str.to_lowercase().contains("api key"));
        }

        // Clean up
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_load_model_openai_empty_api_key() {
        let model_name = to_c_string("openai/text-embedding-ada-002");
        let cache_path = to_c_string("");
        let api_key = to_c_string("");

        let result = TextModelWrapper::load_model(
            model_name.as_ptr(),
            model_name.as_bytes().len(),
            cache_path.as_ptr(),
            cache_path.as_bytes().len(),
            api_key.as_ptr(),
            api_key.as_bytes().len(),
            false,
        );

        // Should fail with missing API key
        assert!(result.model.is_null());
        assert!(!result.error.is_null());

        // Clean up
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_free_model_result_null_pointers() {
        let result = TextModelResult {
            model: ptr::null_mut(),
            error: ptr::null_mut(),
        };

        // Should not crash with null pointers
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_free_model_result_with_error() {
        let error_msg = to_c_string("Test error message");
        let result = TextModelResult {
            model: ptr::null_mut(),
            error: error_msg.into_raw(),
        };

        // Should properly free the error string
        TextModelWrapper::free_model_result(result);
    }

    #[test]
    fn test_make_vect_embeddings_empty_input() {
        // We can't easily test this without a valid model, but we can test
        // the structure with empty input
        let empty_texts: Vec<StringItem> = vec![];

        // This documents that empty input should be handled gracefully
        assert_eq!(empty_texts.len(), 0);
    }

    #[test]
    fn test_string_item_with_empty_string() {
        let empty_str = "";
        let string_item = create_string_item(empty_str);

        assert_eq!(string_item.len, 0);
        assert!(!string_item.ptr.is_null()); // Pointer should still be valid
    }

    #[test]
    fn test_string_item_with_unicode() {
        let unicode_str = "Hello 世界 🌍";
        let string_item = create_string_item(unicode_str);

        assert_eq!(string_item.len, unicode_str.len()); // Byte length, not char length
        assert!(!string_item.ptr.is_null());

        // Verify we can read it back correctly
        unsafe {
            let slice = std::slice::from_raw_parts(string_item.ptr as *const u8, string_item.len);
            let recovered_str = std::str::from_utf8_unchecked(slice);
            assert_eq!(recovered_str, unicode_str);
        }
    }

    #[test]
    fn test_multiple_string_items() {
        let texts = ["first", "second", "third"];
        let string_items: Vec<StringItem> = texts.iter().map(|&s| create_string_item(s)).collect();

        assert_eq!(string_items.len(), 3);

        // Verify each string item
        for (i, item) in string_items.iter().enumerate() {
            assert_eq!(item.len, texts[i].len());
            assert!(!item.ptr.is_null());

            unsafe {
                let slice = std::slice::from_raw_parts(item.ptr as *const u8, item.len);
                let recovered_str = std::str::from_utf8_unchecked(slice);
                assert_eq!(recovered_str, texts[i]);
            }
        }
    }

    #[test]
    fn test_float_vec_result_error_handling() {
        let error_msg = to_c_string("Test embedding error");
        let result = FloatVecResult {
            error: error_msg.into_raw(),
            ptr: ptr::null(),
            len: 0,
            cap: 0,
        };

        // Verify error is set
        assert!(!result.error.is_null());
        assert!(result.ptr.is_null());

        // Read error message
        unsafe {
            let error_cstr = CStr::from_ptr(result.error);
            let error_str = error_cstr.to_str().unwrap();
            assert_eq!(error_str, "Test embedding error");
        }

        // Clean up (simulate what free_vec_result would do)
        unsafe {
            let _ = CString::from_raw(result.error);
        }
    }

    #[test]
    fn test_free_vec_result_null_pointers() {
        let result = FloatVecResult {
            error: ptr::null_mut(),
            ptr: ptr::null(),
            len: 0,
            cap: 0,
        };

        // Should not crash with null pointers
        TextModelWrapper::free_vec_result(result);
    }

    #[test]
    fn test_memory_layout_compatibility() {
        // Test that our structures have the expected memory layout for C interop
        use std::mem;

        // TextModelResult should be two pointers
        assert_eq!(
            mem::size_of::<TextModelResult>(),
            mem::size_of::<*mut std::ffi::c_void>() * 2
        );

        // FloatVec should be pointer + two usizes
        assert_eq!(
            mem::size_of::<FloatVec>(),
            mem::size_of::<*const f32>() + mem::size_of::<usize>() * 2
        );

        // StringItem should be pointer + usize
        assert_eq!(
            mem::size_of::<StringItem>(),
            mem::size_of::<*const c_char>() + mem::size_of::<usize>()
        );

        // FloatVecResult should be pointer + two usizes + error pointer
        assert_eq!(
            mem::size_of::<FloatVecResult>(),
            mem::size_of::<*mut c_char>()
                + mem::size_of::<*const FloatVec>()
                + mem::size_of::<usize>() * 2
        );
    }

    #[test]
    fn test_c_string_conversion() {
        let test_strings = vec![
            "simple",
            "with spaces",
            "with-dashes",
            "with_underscores",
            "with123numbers",
            "",
        ];

        for test_str in test_strings {
            let c_string = to_c_string(test_str);
            let c_str = c_string.as_c_str();
            let recovered = c_str.to_str().unwrap();
            assert_eq!(recovered, test_str);
        }
    }

    #[test]
    fn test_model_options_structure() {
        // Test that we can create ModelOptions with various combinations
        use crate::model::ModelOptions;

        let options1 = ModelOptions {
            model_id: "test-model".to_string(),
            cache_path: Some("/tmp/cache".to_string()),
            api_key: Some("sk-test123".to_string()),
            use_gpu: Some(true),
        };

        let options2 = ModelOptions {
            model_id: "openai/text-embedding-ada-002".to_string(),
            cache_path: None,
            api_key: Some("sk-test456".to_string()),
            use_gpu: None,
        };

        assert_eq!(options1.model_id, "test-model");
        assert_eq!(options1.cache_path, Some("/tmp/cache".to_string()));
        assert_eq!(options1.api_key, Some("sk-test123".to_string()));
        assert_eq!(options1.use_gpu, Some(true));

        assert_eq!(options2.model_id, "openai/text-embedding-ada-002");
        assert_eq!(options2.cache_path, None);
        assert_eq!(options2.api_key, Some("sk-test456".to_string()));
        assert_eq!(options2.use_gpu, None);
    }

    #[test]
    fn test_concurrent_qwen_embeddings_via_ffi() {
        run_concurrent_ffi_embeddings("Qwen/Qwen3-Embedding-0.6B");
    }
}
