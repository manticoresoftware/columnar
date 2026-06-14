//! Panic containment for the `extern "C"` boundary.
//!
//! A Rust panic must never unwind into the C++ daemon: there is no Rust
//! handler on the caller's stack, so the unwinder runs off the end of the
//! 128 KiB boost coroutine stack searchd serves queries on and the runtime
//! aborts the whole process (`fatal runtime error: failed to initiate panic,
//! error 5`). Every `extern "C"` entry point therefore wraps its body in
//! [`catch_panic`] and converts a panic into the same error-string channel
//! used for ordinary failures.

use std::cell::RefCell;
use std::panic::{self, AssertUnwindSafe};
use std::sync::Once;

thread_local! {
    /// Message of the most recent panic on this thread, captured by the hook
    /// installed by [`install_hook`]. The hook runs on the panicking thread
    /// before unwinding starts, so it sees the `file:line` location that the
    /// unwind payload alone does not carry.
    static LAST_PANIC: RefCell<Option<String>> = const { RefCell::new(None) };
}

static HOOK: Once = Once::new();

/// Install the process-wide panic hook (idempotent). Records the panic
/// message and location for [`catch_panic`] and mirrors it to stderr so the
/// panic site stays visible even when the caller only surfaces the returned
/// error string.
pub fn install_hook() {
    HOOK.call_once(|| {
        panic::set_hook(Box::new(|info| {
            let msg = match info.location() {
                Some(loc) => format!(
                    "{} (at {}:{})",
                    payload_str(info.payload()),
                    loc.file(),
                    loc.line()
                ),
                None => payload_str(info.payload()).to_string(),
            };
            eprintln!("manticore-knn-embeddings: panic: {msg}");
            LAST_PANIC.with(|slot| *slot.borrow_mut() = Some(msg));
        }));
    });
}

/// Run `f`, converting a panic into `Err(message)`.
///
/// `AssertUnwindSafe` is sound here: after a panic we only return an error —
/// no state captured by the closure is observed again. Model handles are
/// re-validated on every FFI call via the MODEL_MAGIC canary, and a poisoned
/// internal lock turns later calls into further clean errors, not UB.
pub fn catch_panic<T>(f: impl FnOnce() -> T) -> Result<T, String> {
    // Drop any stale message so an Err below can't pick up the location of an
    // older, internally-handled panic from this thread.
    LAST_PANIC.with(|slot| slot.borrow_mut().take());
    match panic::catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => Ok(v),
        Err(payload) => Err(LAST_PANIC
            .with(|slot| slot.borrow_mut().take())
            // The hook may have fired on another thread (e.g. a panicking
            // rayon/gemm worker re-thrown at the join site); fall back to the
            // payload, which travels with the unwind.
            .unwrap_or_else(|| payload_str(payload.as_ref()).to_string())),
    }
}

fn payload_str(payload: &(dyn std::any::Any + Send)) -> &str {
    if let Some(s) = payload.downcast_ref::<&str>() {
        s
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s
    } else {
        "unknown panic payload"
    }
}
