use crate::model::{create_model, Model, ModelOptions, TextModel};
use std::os::raw::c_char;
use std::{ffi::c_void, ptr};

/// cbindgen:field-names=[m_pModel, m_szError]
#[repr(C)]
pub struct TextModelResult {
    pub model: *mut c_void,
    pub error: *mut c_char,
}

#[repr(transparent)]
pub struct TextModelWrapper(*mut c_void);

#[repr(C)]
pub struct FloatVec {
    pub ptr: *const f32,
    pub len: usize,
    pub cap: usize,
}

/// cbindgen:field-names=[m_szError,m_tEmbedding,len,cap]
#[repr(C)]
pub struct FloatVecResult {
    pub error: *mut c_char,
    pub ptr: *const FloatVec,
    pub len: usize,
    pub cap: usize,
}

#[repr(C)]
pub struct StringItem {
    pub ptr: *const c_char,
    pub len: usize,
}

impl TextModelWrapper {
    pub extern "C" fn load_model(
        name_ptr: *const c_char,
        name_len: usize,
        cache_path_ptr: *const c_char,
        cache_path_len: usize,
        api_key_ptr: *const c_char,
        api_key_len: usize,
        use_gpu: bool,
    ) -> TextModelResult {
        let name = unsafe {
            let slice = std::slice::from_raw_parts(name_ptr as *mut u8, name_len);
            std::str::from_utf8_unchecked(slice)
        };

        let cache_path = unsafe {
            let slice = std::slice::from_raw_parts(cache_path_ptr as *mut u8, cache_path_len);
            std::str::from_utf8_unchecked(slice)
        };

        let api_key = unsafe {
            let slice = std::slice::from_raw_parts(api_key_ptr as *mut u8, api_key_len);
            std::str::from_utf8_unchecked(slice)
        };

        let options = ModelOptions {
            model_id: name.to_string(),
            cache_path: if cache_path.is_empty() {
                None
            } else {
                Some(cache_path.to_string())
            },
            api_key: if api_key.is_empty() {
                None
            } else {
                Some(api_key.to_string())
            },
            use_gpu: Some(use_gpu),
        };

        match create_model(options) {
            Ok(model) => TextModelResult {
                model: Box::into_raw(Box::new(model)) as *mut c_void,
                error: ptr::null_mut(),
            },
            Err(e) => {
                let c_error = std::ffi::CString::new(e.to_string()).unwrap();
                TextModelResult {
                    model: ptr::null_mut(),
                    error: c_error.into_raw(),
                }
            }
        }
    }

    pub extern "C" fn free_model_result(res: TextModelResult) {
        unsafe {
            if !res.model.is_null() {
                drop(Box::from_raw(res.model as *mut Model));
            }

            if !res.error.is_null() {
                let _ = std::ffi::CString::from_raw(res.error);
            }
        }
    }

    fn as_model(&self) -> &Model {
        unsafe { &*(self.0 as *const Model) }
    }

    pub extern "C" fn make_vect_embeddings(
        &self,
        texts: *const StringItem,
        count: usize,
    ) -> FloatVecResult {
        let string_slice = unsafe { std::slice::from_raw_parts(texts, count) };

        // Convert bytes to strings, handling invalid UTF-8 gracefully
        // Use from_utf8_lossy to replace invalid sequences with replacement characters
        let strings: Vec<String> = string_slice
            .iter()
            .map(|item| {
                let bytes = unsafe {
                    std::slice::from_raw_parts(
                        item.ptr as *const u8,
                        item.len,
                    )
                };
                // Use from_utf8_lossy to handle invalid UTF-8 gracefully
                // This replaces invalid sequences with the replacement character (U+FFFD)
                String::from_utf8_lossy(bytes).into_owned()
            })
            .collect();
        
        // Convert Vec<String> to Vec<&str> for the predict function
        let string_refs: Vec<&str> = strings.iter().map(|s| s.as_str()).collect();

        let mut float_vec_list: Vec<FloatVec> = Vec::new();
        let model = self.as_model();
        let embeddings_list = model.predict(&string_refs);
        let c_error = match embeddings_list {
            Ok(embeddings_list) => {
                for embeddings in embeddings_list.iter() {
                    let ptr = embeddings.as_ptr();
                    let len = embeddings.len();
                    let cap = embeddings.capacity();
                    let vec = FloatVec { ptr, len, cap };
                    float_vec_list.push(vec);
                }

                std::mem::forget(embeddings_list);
                ptr::null_mut()
            }
            Err(e) => {
                // Don't push empty vector on error - return error through szError pattern
                let c_error = std::ffi::CString::new(e.to_string()).unwrap();
                c_error.into_raw()
            }
        };

        let vec_result = FloatVecResult {
            ptr: float_vec_list.as_ptr(),
            len: float_vec_list.len(),
            cap: float_vec_list.capacity(),
            error: c_error,
        };
        std::mem::forget(float_vec_list);
        vec_result
    }

    pub extern "C" fn free_vec_result(result: FloatVecResult) {
        unsafe {
            // Only process if we have valid data
            if !result.ptr.is_null() && result.len > 0 {
                let slice = std::slice::from_raw_parts(result.ptr, result.len);

                for vec in slice {
                    // Free the FloatVec's inner buffer
                    if !vec.ptr.is_null() && vec.len > 0 {
                        let _ = Vec::from_raw_parts(vec.ptr as *mut f32, vec.len, vec.cap);
                    }
                }

                // Free the FloatVecList's array of FloatVecResult
                let _ = Vec::from_raw_parts(result.ptr as *mut FloatVec, result.len, result.cap);
            }

            // Free the error string if it exists
            if !result.error.is_null() {
                let _ = std::ffi::CString::from_raw(result.error);
            }
        }
    }

    pub extern "C" fn get_hidden_size(&self) -> usize {
        self.as_model().get_hidden_size()
    }

    pub extern "C" fn get_max_input_len(&self) -> usize {
        self.as_model().get_max_input_len()
    }
}
