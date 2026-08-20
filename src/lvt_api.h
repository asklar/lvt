#pragma once
#include <stdint.h>

// C ABI over lvt_core, consumed by the MCP server.
//
// The surface is deliberately one call: everything is JSON in, JSON out. That
// keeps the FFI boundary trivial to reason about and means adding a tool never
// changes the ABI.
//
// MEMORY OWNERSHIP — this matters more than usual here. The MCP server is Rust,
// which always links the release CRT, while lvt may be built against the debug
// CRT. The two therefore have separate heaps. The rule is absolute:
//
//     each side frees only what it allocated
//
// So `result_json` is allocated by lvt and must be released with
// `lvt_api_free`, never by the caller's allocator; and strings passed *in* are
// only read, never freed, by lvt.

#ifdef __cplusplus
extern "C" {
#endif

// Invoke a method. `params_json` may be NULL for methods that take none.
//
// `allow_input` mirrors the server's --allow-input: it gates every method that
// can change something outside lvt. That is not only the input methods — a
// screenshot written to a caller-chosen path creates or truncates that file, so
// it is a write too, and a server described to a model as read-only must not
// offer one.
//
// Returns 0 on success. On failure returns non-zero and, where possible, still
// writes a JSON object describing the error, so a caller always has something
// structured to report rather than a bare code.
//
// `*result_json` is set to a NUL-terminated UTF-8 JSON string owned by lvt.
int32_t lvt_api_call(const char* method, const char* params_json, int32_t allow_input,
                     char** result_json);

// Release a string produced by lvt_api_call. Safe to call with NULL.
void lvt_api_free(char* result_json);

// Human-readable version of the lvt this ABI belongs to, for the MCP server to
// report in its handshake. Statically allocated; do not free.
const char* lvt_api_version(void);

#ifdef __cplusplus
}
#endif
