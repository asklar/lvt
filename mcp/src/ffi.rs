//! The C ABI over `lvt_core`, and the safe wrapper the rest of the crate uses.
//!
//! Everything unsafe in this crate lives here. The two rules being enforced are
//! the ones documented in `src/lvt_api.h`:
//!
//! 1. **Nothing frees across the boundary.** lvt allocates `result_json` on its
//!    own heap and only `lvt_api_free` may release it. That is not pedantry:
//!    Rust always links the release CRT while lvt may be built against the
//!    debug CRT, so the two genuinely have different heaps and a cross-free is
//!    a real crash, not a theoretical one. [`Response`] exists purely so that
//!    rule cannot be forgotten — its `Drop` is the only place the pointer is
//!    released, and it runs even if the caller returns early or panics.
//!
//! 2. **No unwinding either way.** The C++ side wraps its body so no exception
//!    escapes; this side has `panic = "abort"` and the crate's entry point uses
//!    `catch_unwind`.

use std::ffi::{c_char, CStr, CString};

unsafe extern "C" {
    fn lvt_api_call(method: *const c_char, params_json: *const c_char, result_json: *mut *mut c_char)
        -> i32;
    fn lvt_api_free(result_json: *mut c_char);
    fn lvt_api_version() -> *const c_char;
}

/// An owned response string from lvt, released on drop via `lvt_api_free`.
struct Response(*mut c_char);

impl Drop for Response {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: `self.0` came from lvt_api_call and has not been freed;
            // the field is private and never copied out, so this runs once.
            unsafe { lvt_api_free(self.0) };
            self.0 = std::ptr::null_mut();
        }
    }
}

impl Response {
    /// Copies the response into a Rust `String`, replacing invalid UTF-8 rather
    /// than failing: a malformed byte in an app's window title should not take
    /// down the tool call that reported it.
    fn to_string_lossy(&self) -> String {
        if self.0.is_null() {
            return String::new();
        }
        // SAFETY: lvt_api_call guarantees a NUL-terminated buffer on success,
        // and we only reach here while `self` still owns it.
        unsafe { CStr::from_ptr(self.0) }.to_string_lossy().into_owned()
    }
}

/// What a tool call produced: lvt's JSON, plus whether lvt considered it an error.
#[derive(Debug)]
pub struct ApiResult {
    pub json: String,
    pub ok: bool,
}

/// Calls into lvt.
///
/// Never returns `Err` for an application-level failure — those come back as
/// `ok: false` with lvt's own JSON error, which is far more useful to a model
/// than a transport error. `Err` is reserved for the boundary itself failing.
pub fn call(method: &str, params: &serde_json::Value) -> Result<ApiResult, String> {
    // An interior NUL cannot reach C, so reject it here rather than truncating
    // silently and calling a different method than was asked for.
    let method_c = CString::new(method).map_err(|_| "method name contains a NUL byte".to_string())?;
    let params_c = CString::new(params.to_string())
        .map_err(|_| "arguments contain a NUL byte".to_string())?;

    let mut raw: *mut c_char = std::ptr::null_mut();
    // SAFETY: both pointers are valid NUL-terminated strings that outlive the
    // call, and `raw` is a valid out-pointer. lvt does not retain either input.
    let status = unsafe { lvt_api_call(method_c.as_ptr(), params_c.as_ptr(), &mut raw) };

    // Constructed before inspecting `status` so the buffer is owned — and so
    // will be freed — no matter which branch we take below.
    let response = Response(raw);

    if status < 0 {
        return Err(format!("lvt could not service '{method}' (status {status})"));
    }
    let json = response.to_string_lossy();
    if json.is_empty() {
        return Err(format!("lvt returned nothing for '{method}'"));
    }
    Ok(ApiResult { json, ok: status == 0 })
}

/// The lvt version string, for the MCP handshake.
pub fn version() -> String {
    // SAFETY: lvt_api_version returns a pointer to a static string literal,
    // valid for the process lifetime and never freed.
    let ptr = unsafe { lvt_api_version() };
    if ptr.is_null() {
        return "unknown".to_string();
    }
    unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    // `cargo test` builds a test binary, which still has to resolve the three
    // C symbols even though lvt itself is not linked into it. Standing in a
    // fake implementation is not just a linker workaround: it is what makes the
    // ownership contract testable at all, since FREES counts the frees the
    // wrapper performs and so proves `Response::drop` actually runs on every
    // path — the one bug in this file that would otherwise only show up as a
    // slow leak in production.
    pub(super) static FREES: AtomicUsize = AtomicUsize::new(0);
    pub(super) static NEXT_STATUS: AtomicUsize = AtomicUsize::new(0);
    pub(super) static RETURN_NULL: AtomicUsize = AtomicUsize::new(0);

    #[unsafe(no_mangle)]
    extern "C" fn lvt_api_call(
        method: *const c_char,
        _params: *const c_char,
        result: *mut *mut c_char,
    ) -> i32 {
        if RETURN_NULL.load(Ordering::SeqCst) == 1 {
            unsafe { *result = std::ptr::null_mut() };
            return 0;
        }
        let name = unsafe { CStr::from_ptr(method) }.to_string_lossy().into_owned();
        let payload = CString::new(format!("{{\"method\":\"{name}\"}}")).unwrap();
        unsafe { *result = payload.into_raw() };
        NEXT_STATUS.load(Ordering::SeqCst) as i32
    }

    #[unsafe(no_mangle)]
    extern "C" fn lvt_api_free(result: *mut c_char) {
        if !result.is_null() {
            FREES.fetch_add(1, Ordering::SeqCst);
            drop(unsafe { CString::from_raw(result) });
        }
    }

    #[unsafe(no_mangle)]
    extern "C" fn lvt_api_version() -> *const c_char {
        c"9.9.9-test".as_ptr()
    }

    fn reset() {
        NEXT_STATUS.store(0, Ordering::SeqCst);
        RETURN_NULL.store(0, Ordering::SeqCst);
    }

    // The stub's state is process-global, and cargo runs tests in parallel, so
    // anything that touches it has to hold this. Without it the free-count
    // assertions fail intermittently against each other rather than against the
    // code under test.
    static STUB: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn with_stub<T>(body: impl FnOnce() -> T) -> T {
        // A poisoned lock just means an earlier test panicked; the stub state is
        // reset below regardless, so recovering is safe and keeps one failure
        // from cascading into every later test.
        let _guard = STUB.lock().unwrap_or_else(|e| e.into_inner());
        reset();
        let result = body();
        reset();
        result
    }

    #[test]
    fn rejects_method_with_interior_nul() {
        let err = call("get\0tree", &serde_json::json!({})).unwrap_err();
        assert!(err.contains("NUL"), "unexpected error: {err}");
    }

    #[test]
    fn nul_inside_argument_text_is_escaped_rather_than_rejected() {
        // Worth pinning: a NUL in a *value* cannot reach C as an interior NUL
        // because serde escapes it to \u0000 first. So the call must succeed,
        // and the escape must survive into what C receives. If serde_json ever
        // stopped escaping it, this test fails and the CString::new guard in
        // `call` becomes the thing that catches it.
        let params = serde_json::json!({ "text": "a\0b" });
        assert_eq!(params.to_string(), r#"{"text":"a\u0000b"}"#);
        with_stub(|| {
            let result = call("set_value", &params).expect("escaped NUL must not fail the call");
            assert!(result.ok);
        });
    }

    #[test]
    fn passes_method_through_and_frees_once() {
        with_stub(|| {
            let before = FREES.load(Ordering::SeqCst);
            let result = call("list_apps", &serde_json::json!({})).unwrap();
            assert!(result.ok);
            assert_eq!(result.json, "{\"method\":\"list_apps\"}");
            assert_eq!(
                FREES.load(Ordering::SeqCst),
                before + 1,
                "response must be freed exactly once"
            );
        });
    }

    #[test]
    fn nonzero_status_is_reported_as_not_ok_but_keeps_the_payload() {
        with_stub(|| {
            NEXT_STATUS.store(1, Ordering::SeqCst);
            let before = FREES.load(Ordering::SeqCst);
            let result = call("connect", &serde_json::json!({})).unwrap();
            // An application-level failure must still surface lvt's own JSON:
            // that message is the whole value of the call to a model.
            assert!(!result.ok);
            assert_eq!(result.json, "{\"method\":\"connect\"}");
            assert_eq!(FREES.load(Ordering::SeqCst), before + 1);
        });
    }

    #[test]
    fn negative_status_is_a_boundary_error_and_still_frees() {
        with_stub(|| {
            NEXT_STATUS.store(usize::MAX, Ordering::SeqCst); // truncates to -1 as i32
            let before = FREES.load(Ordering::SeqCst);
            let err = call("connect", &serde_json::json!({})).unwrap_err();
            assert!(err.contains("could not service"), "unexpected error: {err}");
            assert_eq!(
                FREES.load(Ordering::SeqCst),
                before + 1,
                "must free even on the error path"
            );
        });
    }

    #[test]
    fn null_response_is_an_error_not_a_crash() {
        with_stub(|| {
            RETURN_NULL.store(1, Ordering::SeqCst);
            let err = call("connect", &serde_json::json!({})).unwrap_err();
            assert!(err.contains("returned nothing"), "unexpected error: {err}");
        });
    }

    #[test]
    fn version_reads_the_static_string() {
        assert_eq!(version(), "9.9.9-test");
    }
}
