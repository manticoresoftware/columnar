use crate::model::{create_model, Model, ModelOptions, TextModel};
use crate::panic_guard;
use std::os::raw::c_char;
use std::{ffi::c_void, ptr};

/// Sentinel written at offset 0 of every live model handle. Lets FFI entry
/// points detect garbage, null, or freed pointers handed in by the C++ caller
/// and return a clean error instead of dereferencing into UB.
const MODEL_MAGIC: u64 = 0xC0FF_EE5E_E7BE_EFDE;

/// Sentinel written over MODEL_MAGIC in `Drop` before the inner fields are
/// destroyed. A concurrent reader racing with `free_model_result` either sees
/// MAGIC (and proceeds safely) or DEAD (and gets a clean error).
const MODEL_DEAD: u64 = 0xDEAD_DEAD_DEAD_DEAD;

/// Heap-allocated wrapper that the FFI hands to C++ as `*mut c_void`. The C++
/// side stores the raw pointer and passes it back into every call; we use the
/// `magic` field to validate that the pointer still references a live handle.
///
/// Layout note: `#[repr(C)]` and `magic` as the first field guarantee that the
/// first 8 bytes of the allocation are the canary, regardless of what the inner
/// `Model` enum's discriminant looks like.
#[repr(C)]
struct ModelHandle {
    magic: u64,
    inner: Model,
}

impl ModelHandle {
    fn new(inner: Model) -> Self {
        Self {
            magic: MODEL_MAGIC,
            inner,
        }
    }
}

impl Drop for ModelHandle {
    fn drop(&mut self) {
        // Tombstone before the inner Model is dropped so any concurrent FFI
        // reader sees MODEL_DEAD rather than MODEL_MAGIC.
        self.magic = MODEL_DEAD;
    }
}

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

/// Build a heap CString for the FFI error channel. Panic messages can contain
/// interior NULs; strip them rather than fail — a panic on this path would
/// unwind out of the catch handler and abort the process.
fn to_c_error(msg: &str) -> *mut c_char {
    std::ffi::CString::new(msg.replace('\0', "?"))
        .map(|c| c.into_raw())
        // Unreachable after the NUL strip, but never panic on this path.
        .unwrap_or(ptr::null_mut())
}

impl TextModelWrapper {
    pub extern "C" fn load_model(
        name_ptr: *const c_char,
        name_len: usize,
        cache_path_ptr: *const c_char,
        cache_path_len: usize,
        api_key_ptr: *const c_char,
        api_key_len: usize,
        api_url_ptr: *const c_char,
        api_url_len: usize,
        api_timeout: i32, // 0 = unlimited, >0 = timeout in seconds
        use_gpu: bool,
    ) -> TextModelResult {
        panic_guard::catch_panic(|| {
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

            let api_url = unsafe {
                let slice = std::slice::from_raw_parts(api_url_ptr as *mut u8, api_url_len);
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
                api_url: if api_url.is_empty() {
                    None
                } else {
                    Some(api_url.to_string())
                },
                api_timeout: if api_timeout > 0 {
                    Some(api_timeout as u64) // Specific timeout
                } else {
                    None // Unlimited (no timeout)
                },
                use_gpu: Some(use_gpu),
            };

            match create_model(options) {
                Ok(model) => TextModelResult {
                    model: Box::into_raw(Box::new(ModelHandle::new(model))) as *mut c_void,
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
        })
        .unwrap_or_else(|msg| TextModelResult {
            model: ptr::null_mut(),
            error: to_c_error(&format!("embeddings: internal error (panic): {msg}")),
        })
    }

    pub extern "C" fn free_model_result(res: TextModelResult) {
        // A panic mid-free leaks the allocation; that beats unwinding into
        // C++, which aborts the daemon. The panic hook has already logged it.
        let _ = panic_guard::catch_panic(|| unsafe {
            if !res.model.is_null() {
                // Drop runs ModelHandle::drop first (tombstones magic to
                // MODEL_DEAD), then destroys the inner Model.
                drop(Box::from_raw(res.model as *mut ModelHandle));
            }

            if !res.error.is_null() {
                let _ = std::ffi::CString::from_raw(res.error);
            }
        });
    }

    /// Validate the handle pointer before dereferencing. Returns a static error
    /// string the caller can surface to C++ instead of crashing on a bad ptr.
    /// Catches null, double-free / freed (MODEL_DEAD), and garbage handles.
    /// Cannot catch a free that happens mid-call — that requires shared
    /// ownership on the C++ side and is out of scope here.
    fn as_model(&self) -> Result<&Model, &'static str> {
        if self.0.is_null() {
            return Err("embeddings: model handle is null");
        }
        // Read the magic without forming a &ModelHandle reference first — that
        // would already be UB if the pointer is invalid. ptr::read of an
        // 8-byte aligned u64 is a single atomic load on every target Manticore
        // ships on, so this is safe against a concurrent Drop tombstone write.
        let magic = unsafe { std::ptr::read(self.0 as *const u64) };
        match magic {
            MODEL_MAGIC => Ok(unsafe { &(*(self.0 as *const ModelHandle)).inner }),
            MODEL_DEAD => Err("embeddings: model has been freed (use-after-free)"),
            _ => Err("embeddings: model handle is corrupted (invalid magic)"),
        }
    }

    pub extern "C" fn make_vect_embeddings(
        &self,
        texts: *const StringItem,
        count: usize,
    ) -> FloatVecResult {
        panic_guard::catch_panic(|| {
            let model = match self.as_model() {
                Ok(m) => m,
                Err(msg) => {
                    let c_error = std::ffi::CString::new(msg).unwrap();
                    return FloatVecResult {
                        error: c_error.into_raw(),
                        ptr: ptr::null(),
                        len: 0,
                        cap: 0,
                    };
                }
            };

            let string_slice = unsafe { std::slice::from_raw_parts(texts, count) };

            // Zero-copy: borrow C++ strings directly as &str.
            // Input is already valid UTF-8 (passed through SQL parser on the C++ side).
            let string_refs: Vec<&str> = string_slice
                .iter()
                .map(|item| unsafe {
                    let bytes = std::slice::from_raw_parts(item.ptr as *const u8, item.len);
                    std::str::from_utf8_unchecked(bytes)
                })
                .collect();

            let mut float_vec_list: Vec<FloatVec> = Vec::new();
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
        })
        .unwrap_or_else(|msg| FloatVecResult {
            error: to_c_error(&format!("embeddings: internal error (panic): {msg}")),
            ptr: ptr::null(),
            len: 0,
            cap: 0,
        })
    }

    pub extern "C" fn free_vec_result(result: FloatVecResult) {
        // A panic mid-free leaks the buffers; that beats unwinding into C++,
        // which aborts the daemon. The panic hook has already logged it.
        let _ = panic_guard::catch_panic(|| unsafe {
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
        });
    }

    pub extern "C" fn get_hidden_size(&self) -> usize {
        // No error channel here; return 0 on a bad handle or a panic so the
        // C++ caller sees an obviously-wrong dimension instead of UB or an
        // abort. The handle is already validated before any real work, so a 0
        // here means the C++ side handed us an invalid pointer.
        panic_guard::catch_panic(|| self.as_model().map(|m| m.get_hidden_size()).unwrap_or(0))
            .unwrap_or(0)
    }

    pub extern "C" fn get_max_input_len(&self) -> usize {
        panic_guard::catch_panic(|| self.as_model().map(|m| m.get_max_input_len()).unwrap_or(0))
            .unwrap_or(0)
    }

    /// Validates the API key by making a minimal test request to the API.
    /// Returns null on success, or an error message string on failure.
    /// The caller is responsible for freeing the error string using free_string().
    pub extern "C" fn validate_api_key(&self) -> *mut c_char {
        panic_guard::catch_panic(|| {
            let model = match self.as_model() {
                Ok(m) => m,
                Err(msg) => {
                    return std::ffi::CString::new(msg)
                        .map(|c| c.into_raw())
                        .unwrap_or(ptr::null_mut());
                }
            };
            match model.validate_api_key() {
                Ok(()) => ptr::null_mut(),
                Err(e) => {
                    let error_str = e.to_string();
                    let c_error = match std::ffi::CString::new(error_str) {
                        Ok(cstr) => cstr,
                        Err(_) => {
                            return ptr::null_mut();
                        }
                    };
                    c_error.into_raw()
                }
            }
        })
        .unwrap_or_else(|msg| to_c_error(&format!("embeddings: internal error (panic): {msg}")))
    }

    /// Frees a string returned by validate_api_key().
    ///
    /// This function is required for proper memory management in FFI:
    /// - validate_api_key() returns a Rust-allocated CString via CString::into_raw()
    /// - The C++ caller receives ownership of this string pointer
    /// - After copying the error message, the C++ code must call free_string() to deallocate
    /// - Without this, the Rust-allocated memory would leak
    ///
    /// This follows the standard Rust FFI pattern for returning owned strings to C/C++.
    pub extern "C" fn free_string(s: *mut c_char) {
        if !s.is_null() {
            let _ = panic_guard::catch_panic(|| unsafe {
                let _ = std::ffi::CString::from_raw(s);
            });
        }
    }
}
