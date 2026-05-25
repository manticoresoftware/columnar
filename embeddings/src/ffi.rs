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
pub(crate) fn diag_log(line: &str) {
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

/// Find which mapped VMA in /proc/self/maps contains the given address.
/// Returns (start, end, tag) of that mapping. Works for both pthread stacks
/// AND boost-coroutine stacks. pthread stacks have the `[stack]` tag for
/// the main thread or anonymous for spawned threads; boost-coroutine
/// stacks are always anonymous rw-p mappings invisible to
/// pthread_getattr_np. Either way /proc/self/maps shows them.
///
/// Reading /proc/self/maps is NOT async-signal-safe and may allocate. Do
/// not call from signal handlers. From normal probe sites it's fine.
#[cfg(target_os = "linux")]
fn stack_region_for(addr: usize) -> Option<(usize, usize, String)> {
    let maps = std::fs::read_to_string("/proc/self/maps").ok()?;
    for line in maps.lines() {
        // Format: "7f277d95a000-7f277d97a000 rw-p 00000000 00:00 0   [stack]"
        // or just "...rw-p ..." for anonymous (including boost coroutines).
        let mut parts = line.splitn(2, ' ');
        let range = parts.next()?;
        let rest = parts.next().unwrap_or("");
        let mut bounds = range.splitn(2, '-');
        let start = usize::from_str_radix(bounds.next()?, 16).ok()?;
        let end = usize::from_str_radix(bounds.next()?, 16).ok()?;
        if addr >= start && addr < end {
            // Tag: text after the last whitespace — "[stack]", "[heap]",
            // a path, or treat empty as "[anon]" (= likely boost coroutine).
            let tag = rest
                .split_whitespace()
                .last()
                .filter(|s| s.starts_with('[') || s.starts_with('/'))
                .unwrap_or("[anon]")
                .to_string();
            return Some((start, end, tag));
        }
    }
    None
}

#[cfg(not(target_os = "linux"))]
fn stack_region_for(_addr: usize) -> Option<(usize, usize, String)> {
    None
}

/// Log a stack-usage snapshot at the current point. Captures:
///   - current $sp (estimated via address of a stack-allocated local)
///   - the VMA from /proc/self/maps that currently contains $sp — i.e. the
///     ACTUAL stack region we are on. For pthread workers this is the
///     "[stack]"-tagged mapping; for boost-context coroutines it's an
///     anonymous rw-p mapping. Either way the bounds are real.
///   - bytes used (region_end - sp)
///   - bytes remaining (sp - region_start)
///   - the VMA tag so we can tell "[stack]" vs "[anon]" (= coroutine)
///
/// Call this at suspected stack-pressure choke points (every FFI entry,
/// before each candle op) to map out where exactly the budget is burnt.
/// Safe to call from any thread; no global state mutation.
pub(crate) fn stack_probe(label: &str) {
    let probe: u8 = 0;
    let sp = &probe as *const u8 as usize;
    let tid = unsafe { libc::gettid() };
    match stack_region_for(sp) {
        Some((start, end, tag)) => {
            let size = end - start;
            let used = end.saturating_sub(sp);
            let remaining = sp.saturating_sub(start);
            diag_log(&format!(
                "[stack_probe] {label}: tid={tid} sp=0x{sp:016x} \
                 region=0x{start:016x}-0x{end:016x} tag={tag} \
                 size={size} used={used} remaining={remaining}"
            ));
        }
        None => {
            diag_log(&format!(
                "[stack_probe] {label}: tid={tid} sp=0x{sp:016x} \
                 (no /proc/self/maps match — sp may be in a freshly-allocated region)"
            ));
        }
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
