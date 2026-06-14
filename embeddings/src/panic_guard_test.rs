use crate::panic_guard::{catch_panic, install_hook};

#[test]
fn ok_result_passes_through() {
    assert_eq!(catch_panic(|| 42), Ok(42));
}

#[test]
fn panic_is_caught_with_message_and_location() {
    install_hook();
    let err = catch_panic(|| -> () { panic!("boom {}", 7) }).unwrap_err();
    assert!(err.contains("boom 7"), "payload missing: {err}");
    assert!(
        err.contains("panic_guard_test.rs"),
        "location missing: {err}"
    );
}

#[test]
fn str_payload_is_reported() {
    install_hook();
    let err = catch_panic(|| -> () { panic!("static message") }).unwrap_err();
    assert!(err.contains("static message"), "got: {err}");
}

#[test]
fn worker_thread_panic_falls_back_to_payload() {
    install_hook();
    // The hook fires on the worker thread, so the caller's thread-local stays
    // empty and catch_panic must recover the message from the unwind payload.
    let err = catch_panic(|| {
        if let Err(payload) = std::thread::spawn(|| panic!("worker boom")).join() {
            std::panic::resume_unwind(payload);
        }
    })
    .unwrap_err();
    assert!(err.contains("worker boom"), "got: {err}");
}
