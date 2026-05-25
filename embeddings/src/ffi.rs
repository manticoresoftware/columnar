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

/// Path the diagnostics hooks write to. The daemon's stderr is not captured
/// by the test harness, so we write to a known file path that CI can upload
/// as an artifact. CI step:
///
///   - name: Capture embeddings diagnostics
///     if: always()
///     run: cat /tmp/manticore-embeddings-diag.log || echo "no diag log"
const DIAG_LOG_PATH: &str = "/tmp/manticore-embeddings-diag.log";

/// Append one line to DIAG_LOG_PATH plus a process-local stderr copy.
/// Errors are intentionally swallowed — this is a diagnostics path, we
/// must not panic from within it.
fn diag_log(line: &str) {
    use std::io::Write;
    eprintln!("{line}");
    if let Ok(mut f) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(DIAG_LOG_PATH)
    {
        let _ = writeln!(f, "{line}");
        let _ = f.flush();
    }
}

/// One-shot installation of panic and signal diagnostics. Called from
/// GetLibFuncs() — runs once when the daemon dlopens the lib.
fn install_diagnostics() {
    use std::sync::Once;
    static INIT: Once = Once::new();
    INIT.call_once(|| {
        // RUST_BACKTRACE=full so std::backtrace::Backtrace::force_capture()
        // produces a full unwind regardless of the daemon's env.
        std::env::set_var("RUST_BACKTRACE", "full");

        // Rust panic hook: write to file with full backtrace.
        std::panic::set_hook(Box::new(|info| {
            let loc = info
                .location()
                .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
                .unwrap_or_else(|| "<unknown>".to_string());
            let payload = info
                .payload()
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| info.payload().downcast_ref::<String>().map(|s| s.as_str()))
                .unwrap_or("<non-string payload>");
            let bt = std::backtrace::Backtrace::force_capture();
            diag_log(&format!(
                "===== manticore-embeddings PANIC =====\n\
                 location: {loc}\n\
                 payload: {payload}\n\
                 backtrace:\n{bt}\n\
                 ====================================="
            ));
        }));

        // Native signal handler for SIGSEGV/SIGBUS/SIGILL/SIGABRT. These are
        // what a stack overflow or heap-corruption abort looks like at the
        // OS level. We write a marker to the diag log so we can correlate
        // the OS-level event with the daemon's crash dump.
        //
        // Inside a signal handler we are *very* restricted (async-signal-safe
        // only). We deliberately use the lowest-level write(2) syscall via
        // libc and avoid Rust formatting / allocation.
        install_signal_diag();

        // Stamp on lib load so the file always has at least one line.
        diag_log("===== manticore-embeddings loaded =====");
    });
}

#[cfg(unix)]
fn install_signal_diag() {
    // We install handlers for the signals that wrap up our crash scenarios:
    //   SIGSEGV / SIGBUS — bad memory access (stack overflow past guard,
    //                       NULL deref, unmapped page, etc.)
    //   SIGABRT          — glibc malloc consistency abort, assertion fail
    //   SIGILL           — undefined-behaviour sanitiser trip on some setups
    //
    // The handler writes a short prefix to the diag log (via raw write(2),
    // async-signal-safe) and then re-raises the signal with the default
    // disposition so the daemon's own crash handler still runs.
    use std::sync::atomic::{AtomicBool, Ordering};
    static INSTALLED: AtomicBool = AtomicBool::new(false);
    if INSTALLED.swap(true, Ordering::SeqCst) {
        return;
    }

    extern "C" fn handler(signum: libc::c_int) {
        // Async-signal-safe: no allocation, no formatted IO, just raw write.
        let prefix: &[u8] = match signum {
            libc::SIGSEGV => b"===== manticore-embeddings SIGSEGV =====\n",
            libc::SIGBUS => b"===== manticore-embeddings SIGBUS =====\n",
            libc::SIGABRT => b"===== manticore-embeddings SIGABRT =====\n",
            libc::SIGILL => b"===== manticore-embeddings SIGILL =====\n",
            _ => b"===== manticore-embeddings SIGNAL =====\n",
        };
        // O_WRONLY|O_CREAT|O_APPEND; mode 0644 — async-signal-safe via libc.
        unsafe {
            let path = b"/tmp/manticore-embeddings-diag.log\0";
            let fd = libc::open(
                path.as_ptr() as *const libc::c_char,
                libc::O_WRONLY | libc::O_CREAT | libc::O_APPEND,
                0o644,
            );
            if fd >= 0 {
                let _ = libc::write(fd, prefix.as_ptr() as *const _, prefix.len());
                let _ = libc::close(fd);
            }
        }

        // Re-raise with default disposition so the daemon's own handler still
        // runs (it produces the existing FATAL CRASH DUMP we already see).
        unsafe {
            libc::signal(signum, libc::SIG_DFL);
            libc::raise(signum);
        }
    }

    // sigaction with SA_ONSTACK so the handler runs even when the original
    // stack is overflowed — vital for diagnosing stack overflow specifically.
    unsafe {
        // 8 KB alternate signal stack — enough for our 1-line write.
        const ALT_STACK_SIZE: usize = 16 * 1024;
        let stack_mem = Box::leak(Box::new([0u8; ALT_STACK_SIZE]));
        let mut altstack: libc::stack_t = std::mem::zeroed();
        altstack.ss_sp = stack_mem.as_mut_ptr() as *mut libc::c_void;
        altstack.ss_size = ALT_STACK_SIZE;
        altstack.ss_flags = 0;
        let _ = libc::sigaltstack(&altstack, std::ptr::null_mut());

        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handler as *const () as usize;
        sa.sa_flags = libc::SA_ONSTACK | libc::SA_RESETHAND;
        libc::sigemptyset(&mut sa.sa_mask);

        for sig in [libc::SIGSEGV, libc::SIGBUS, libc::SIGABRT, libc::SIGILL] {
            libc::sigaction(sig, &sa, std::ptr::null_mut());
        }
    }
}

#[cfg(not(unix))]
fn install_signal_diag() {}

#[no_mangle]
pub extern "C" fn GetLibFuncs() -> *const EmbedLib {
    install_diagnostics();
    &LIB
}
