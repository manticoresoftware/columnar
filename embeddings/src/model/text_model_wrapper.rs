use crate::chunk::ChunkSettings;
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

/// One emitted chunk's byte span into the original input document.
#[repr(C)]
pub struct ChunkSpan {
    pub start: usize,
    pub end: usize,
}

/// Maps one input document to its run of chunks in the flat embeddings/spans
/// arrays: the document's chunks are `[first, first + count)`.
#[repr(C)]
pub struct DocChunks {
    pub first: usize,
    pub count: usize,
}

/// Result of [`TextModelWrapper::make_vect_embeddings_chunked`]: a flat array of
/// chunk embeddings, a parallel array of byte spans, and a per-input-document
/// grouping so the C++ caller can rebuild "these N chunks belong to document i".
///
/// cbindgen:field-names=[m_szError,m_tEmbedding,emb_len,emb_cap,m_tSpans,spans_cap,m_tDocs,docs_len,docs_cap]
#[repr(C)]
pub struct ChunkedVecResult {
    pub error: *mut c_char,
    pub embeddings: *const FloatVec,
    pub emb_len: usize,
    pub emb_cap: usize,
    pub spans: *const ChunkSpan,
    pub spans_cap: usize,
    pub docs: *const DocChunks,
    pub docs_len: usize,
    pub docs_cap: usize,
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

/// An all-null [`ChunkedVecResult`] carrying only an error string.
fn chunked_error(msg: &str) -> ChunkedVecResult {
    ChunkedVecResult {
        error: to_c_error(msg),
        embeddings: ptr::null(),
        emb_len: 0,
        emb_cap: 0,
        spans: ptr::null(),
        spans_cap: 0,
        docs: ptr::null(),
        docs_len: 0,
        docs_cap: 0,
    }
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
        threads: i32, // 0 = use all available CPUs, >0 = cap thread count
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

            let threads = if threads > 0 { threads as usize } else { 0 };

            let mut float_vec_list: Vec<FloatVec> = Vec::new();
            let embeddings_list = model.predict(&string_refs, threads);
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

    /// Chunk each input document into embedding-sized pieces, embed every chunk,
    /// and return the flat embeddings + per-chunk byte spans + per-document
    /// grouping. A null or `STRATEGY_NONE` `settings` yields exactly one chunk
    /// per document (the whole text), i.e. the same shape as `make_vect_embeddings`.
    /// `predict` is reused unchanged: every chunk is embedded in one batched call.
    /// `threads` matches `make_vect_embeddings`: 0 = all CPUs, >0 = cap.
    pub extern "C" fn make_vect_embeddings_chunked(
        &self,
        texts: *const StringItem,
        count: usize,
        settings: *const ChunkSettings,
        threads: i32,
    ) -> ChunkedVecResult {
        panic_guard::catch_panic(|| {
            let model = match self.as_model() {
                Ok(m) => m,
                Err(msg) => return chunked_error(msg),
            };

            let string_slice = unsafe { std::slice::from_raw_parts(texts, count) };
            // Zero-copy: borrow C++ strings directly as &str (already valid UTF-8).
            let string_refs: Vec<&str> = string_slice
                .iter()
                .map(|item| unsafe {
                    let bytes = std::slice::from_raw_parts(item.ptr as *const u8, item.len);
                    std::str::from_utf8_unchecked(bytes)
                })
                .collect();

            let settings_ref = unsafe { settings.as_ref() };
            let enabled = settings_ref.map(ChunkSettings::enabled).unwrap_or(false);
            let max_chunks = settings_ref.map(|s| s.max_chunks as usize).unwrap_or(0);
            let threads = if threads > 0 { threads as usize } else { 0 };

            // 1. chunk every document, recording per-doc spans + grouping.
            let mut flat_chunks: Vec<&str> = Vec::new();
            let mut spans: Vec<ChunkSpan> = Vec::new();
            let mut docs: Vec<DocChunks> = Vec::with_capacity(string_refs.len());
            for &text in &string_refs {
                let doc_spans = if enabled {
                    crate::chunk::cap_chunks(model.chunk(text, settings_ref.unwrap()), max_chunks)
                } else {
                    vec![(0usize, text.len())]
                };
                let first = flat_chunks.len();
                for (start, end) in doc_spans {
                    flat_chunks.push(&text[start..end]);
                    spans.push(ChunkSpan { start, end });
                }
                docs.push(DocChunks {
                    first,
                    count: flat_chunks.len() - first,
                });
            }

            // 2. one batched predict over every chunk (predict itself is untouched).
            let embeddings_list = match model.predict(&flat_chunks, threads) {
                Ok(e) => e,
                Err(e) => return chunked_error(&e.to_string()),
            };

            // 3. hand the flat FloatVec[] + spans + docs to C++. Same ownership
            //    pattern as make_vect_embeddings: forget the buffers here, free
            //    them in free_chunked_result.
            let mut float_vec_list: Vec<FloatVec> = Vec::with_capacity(embeddings_list.len());
            for embeddings in embeddings_list.iter() {
                float_vec_list.push(FloatVec {
                    ptr: embeddings.as_ptr(),
                    len: embeddings.len(),
                    cap: embeddings.capacity(),
                });
            }
            std::mem::forget(embeddings_list);

            let result = ChunkedVecResult {
                error: ptr::null_mut(),
                embeddings: float_vec_list.as_ptr(),
                emb_len: float_vec_list.len(),
                emb_cap: float_vec_list.capacity(),
                spans: spans.as_ptr(),
                spans_cap: spans.capacity(),
                docs: docs.as_ptr(),
                docs_len: docs.len(),
                docs_cap: docs.capacity(),
            };
            std::mem::forget(float_vec_list);
            std::mem::forget(spans);
            std::mem::forget(docs);
            result
        })
        .unwrap_or_else(|msg| chunked_error(&format!("embeddings: internal error (panic): {msg}")))
    }

    pub extern "C" fn free_chunked_result(result: ChunkedVecResult) {
        // A panic mid-free leaks the buffers; that beats unwinding into C++,
        // which aborts the daemon. The panic hook has already logged it.
        let _ = panic_guard::catch_panic(|| unsafe {
            if !result.embeddings.is_null() && result.emb_len > 0 {
                let slice = std::slice::from_raw_parts(result.embeddings, result.emb_len);
                for vec in slice {
                    if !vec.ptr.is_null() && vec.len > 0 {
                        let _ = Vec::from_raw_parts(vec.ptr as *mut f32, vec.len, vec.cap);
                    }
                }
                let _ = Vec::from_raw_parts(
                    result.embeddings as *mut FloatVec,
                    result.emb_len,
                    result.emb_cap,
                );
            }

            // spans is parallel to embeddings, so its length is emb_len.
            if !result.spans.is_null() && result.spans_cap > 0 {
                let _ = Vec::from_raw_parts(
                    result.spans as *mut ChunkSpan,
                    result.emb_len,
                    result.spans_cap,
                );
            }

            if !result.docs.is_null() && result.docs_cap > 0 {
                let _ = Vec::from_raw_parts(
                    result.docs as *mut DocChunks,
                    result.docs_len,
                    result.docs_cap,
                );
            }

            if !result.error.is_null() {
                let _ = std::ffi::CString::from_raw(result.error);
            }
        });
    }
}
