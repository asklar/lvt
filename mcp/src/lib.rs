//! The MCP server that `lvt mcp` runs.
//!
//! This crate is deliberately thin. It owns the MCP protocol and the tool
//! schemas; every question about *what* an element is or *how* to act on it is
//! answered by lvt_core through the C ABI in [`ffi`]. Keeping it that way means
//! the Rust here stays reviewable, and that the whole layer could be replaced
//! without touching any of lvt's real logic.

mod ffi;
mod schema;
mod server;

use std::ffi::c_int;

use rmcp::ServiceExt;

use server::LvtServer;

/// Runs an MCP server over stdio until the client disconnects.
///
/// Returns 0 on a clean shutdown and non-zero on failure. Called from C++, so
/// it must not unwind: `catch_unwind` turns a panic into an error code, and the
/// crate's `panic = "abort"` covers anything that escapes even that.
///
/// `allow_input` gates the tools that can change the target application. It is
/// taken as an argument rather than read from the environment so that the
/// decision stays visible in lvt's own argument parsing.
#[unsafe(no_mangle)]
pub extern "C" fn lvt_mcp_serve_stdio(allow_input: bool) -> c_int {
    let result = std::panic::catch_unwind(|| serve(allow_input));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            // stdout is the JSON-RPC stream. Diagnostics go to stderr or they
            // corrupt the protocol.
            eprintln!("lvt: mcp server stopped: {message}");
            1
        }
        Err(_) => {
            eprintln!("lvt: mcp server panicked");
            2
        }
    }
}

fn serve(allow_input: bool) -> Result<(), String> {
    // A dedicated runtime rather than #[tokio::main]: this is a library entered
    // from C++, so it cannot assume it owns the process's async context, and it
    // must tear the runtime down before returning control.
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .map_err(|e| format!("could not start the async runtime: {e}"))?;

    let result = runtime.block_on(async move {
        // `waiting()` finishes only once every in-flight handler has finished
        // too. Tool calls run on the blocking pool and cannot be cancelled, so
        // a `wait_for` part-way through a two-minute deadline would keep the
        // server alive for two minutes after its client had gone. Watching the
        // input stream for end-of-file gives a second, earlier signal that the
        // client is no longer there.
        let (closed_tx, closed_rx) = tokio::sync::oneshot::channel();
        let transport = (EndOfInput::new(tokio::io::stdin(), closed_tx), tokio::io::stdout());

        let service = LvtServer::new(allow_input)
            .serve(transport)
            .await
            .map_err(|e| format!("could not start the stdio transport: {e}"))?;

        tokio::select! {
            // The ordinary path: the service wound itself up, in-flight work
            // included.
            outcome = service.waiting() => {
                outcome.map_err(|e| format!("the connection ended unexpectedly: {e}"))?;
            }
            // The client closed the pipe. Whatever is still running has nobody
            // left to answer, so stop rather than outliving the session.
            _ = closed_rx => {}
        }
        Ok(())
    });

    // Do not block on whatever is still running: `shutdown_timeout` gives it a
    // moment and then lets go.
    runtime.shutdown_timeout(std::time::Duration::from_millis(250));
    result
}

/// Wraps the input stream to report when it reaches end-of-file, which is how a
/// stdio client signals that it has disconnected.
struct EndOfInput<R> {
    inner: R,
    notify: Option<tokio::sync::oneshot::Sender<()>>,
}

impl<R> EndOfInput<R> {
    fn new(inner: R, notify: tokio::sync::oneshot::Sender<()>) -> Self {
        Self { inner, notify: Some(notify) }
    }
}

impl<R: tokio::io::AsyncRead + Unpin> tokio::io::AsyncRead for EndOfInput<R> {
    fn poll_read(
        mut self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
        buf: &mut tokio::io::ReadBuf<'_>,
    ) -> std::task::Poll<std::io::Result<()>> {
        let before = buf.filled().len();
        let polled = std::pin::Pin::new(&mut self.inner).poll_read(cx, buf);
        // A ready read that produced no bytes is end-of-file.
        if matches!(polled, std::task::Poll::Ready(Ok(()))) && buf.filled().len() == before {
            if let Some(notify) = self.notify.take() {
                let _ = notify.send(());
            }
        }
        polled
    }
}
