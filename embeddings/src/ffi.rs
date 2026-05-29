use crate::model::text_model_wrapper::{
    FloatVecResult, StringItem, TextModelResult, TextModelWrapper,
};
use std::os::raw::c_char;

type LoadModelFn = extern "C" fn(
    *const c_char, // model name (e.g., "openai/text-embedding-ada-002")
    usize,         // model name length
    *const c_char, // cache path (empty string means use default)
    usize,         // cache path length
    *const c_char, // API key (empty string means no key)
    usize,         // API key length
    *const c_char, // api_url: pointer to custom API URL string (empty string means use default URL for the provider)
    usize,         // api_url length
    i32, // api_timeout: timeout in seconds (0 means use default, positive value is timeout in seconds)
    bool, // use_gpu flag
) -> TextModelResult;

type FreeModelResultFn = extern "C" fn(TextModelResult);

type MakeVectEmbeddingsFn =
    extern "C" fn(&TextModelWrapper, *const StringItem, usize) -> FloatVecResult;

type FreeVecResultFn = extern "C" fn(FloatVecResult);

type GetLenFn = extern "C" fn(&TextModelWrapper) -> usize;

type ValidateApiKeyFn = extern "C" fn(&TextModelWrapper) -> *mut c_char;
/// Function pointer type for freeing strings returned by validate_api_key().
///
/// Required for proper memory management in FFI: validate_api_key() returns a Rust-allocated
/// CString via CString::into_raw(). The C++ caller receives ownership and must call free_string()
/// to deallocate the memory, otherwise it will leak. This follows the standard Rust FFI pattern
/// for returning owned strings to C/C++.
type FreeStringFn = extern "C" fn(*mut c_char);

#[repr(C)]
pub struct EmbedLib {
    version: usize,
    version_str: *const c_char,
    load_model: LoadModelFn,
    free_model_result: FreeModelResultFn,
    make_vect_embeddings: MakeVectEmbeddingsFn,
    free_vec_result: FreeVecResultFn,
    get_hidden_size: GetLenFn,
    get_max_input_size: GetLenFn,
    validate_api_key: ValidateApiKeyFn,
    free_string: FreeStringFn,
}
/// Version string with commit hash and timestamp, generated at compile time by build.rs.
///
/// Format: "VERSION commit@timestamp" (e.g., "1.1.0 38f499e@25112313")
/// This matches the format used by other Manticore libraries (columnar, secondary, knn)
/// for consistent version display in searchd -v output.
///
/// The build.rs script generates this string from:
/// - CARGO_PKG_VERSION (from Cargo.toml)
/// - Git commit hash (short format, from GIT_COMMIT_ID env var or git command)
/// - Git commit timestamp (YYMMDDHH format, from GIT_TIMESTAMP_ID env var or git command)
///
/// The string is null-terminated for use as a C string pointer in the EmbedLib struct.
const VERSION_STR: &[u8] = concat!(env!("EMBEDDINGS_VERSION_STR"), "\0").as_bytes();

const LIB: EmbedLib = EmbedLib {
    version: 3usize,
    version_str: VERSION_STR.as_ptr() as *const c_char,
    load_model: TextModelWrapper::load_model,
    free_model_result: TextModelWrapper::free_model_result,
    make_vect_embeddings: TextModelWrapper::make_vect_embeddings,
    free_vec_result: TextModelWrapper::free_vec_result,
    get_hidden_size: TextModelWrapper::get_hidden_size,
    get_max_input_size: TextModelWrapper::get_max_input_len,
    validate_api_key: TextModelWrapper::validate_api_key,
    free_string: TextModelWrapper::free_string,
};

#[no_mangle]
pub extern "C" fn GetLibFuncs() -> *const EmbedLib {
    std::panic::set_hook(Box::new(|_| {}));
    &LIB
}
