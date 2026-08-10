//! The MCP server that `lvt mcp` runs.
//!
//! This crate is deliberately thin. It owns the MCP protocol and the tool
//! schemas; every question about *what* an element is or *how* to act on it is
//! answered by lvt_core through the C ABI in [`ffi`]. Keeping it that way means
//! the Rust here stays reviewable, and that the whole layer could be replaced
//! without touching any of lvt's real logic.

mod ffi;
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

    runtime.block_on(async move {
        let service = LvtServer::new(allow_input)
            .serve(rmcp::transport::stdio())
            .await
            .map_err(|e| format!("could not start the stdio transport: {e}"))?;

        // A client closing the pipe is how these servers normally end, so it is
        // a clean exit rather than an error.
        service.waiting().await.map_err(|e| format!("the connection ended unexpectedly: {e}"))?;
        Ok(())
    })
}
