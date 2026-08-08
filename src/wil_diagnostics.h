#pragma once

namespace lvt {

// Route WIL failure reports (RETURN_IF_FAILED, LOG_IF_FAILED, CATCH_LOG, ...) to
// stderr, gated on lvt::g_debug.
//
// stderr is not incidental: lvt's stdout carries machine-readable payloads (the
// JSON/XML tree, and later the MCP JSON-RPC stream), so a diagnostic written to
// stdout corrupts the protocol. Everything diagnostic goes to stderr.
//
// Call once during startup, after --debug has been parsed. Safe to call again;
// WIL only permits installing the same callback repeatedly, never a different one.
void install_wil_result_logger();

} // namespace lvt
