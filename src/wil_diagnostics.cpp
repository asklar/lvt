#include "wil_diagnostics.h"
#include "debug.h"

#include <wil/result.h>

#include <cstdio>

namespace lvt {
namespace {

const char* failure_type_name(wil::FailureType type) {
    switch (type) {
    case wil::FailureType::Exception:  return "exception";
    case wil::FailureType::Return:     return "return";
    case wil::FailureType::Log:        return "log";
    case wil::FailureType::FailFast:   return "failfast";
    }
    return "unknown";
}

// Strip the build-machine path prefix so output is stable across machines.
const char* short_file(const char* path) {
    if (!path)
        return "";
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/')
            last = p + 1;
    }
    return last;
}

void __stdcall on_wil_failure(const wil::FailureInfo& failure) noexcept {
    if (!g_debug)
        return;

    fprintf(stderr, "lvt: [wil/%s] hr=0x%08X at %s:%u",
            failure_type_name(failure.type),
            static_cast<unsigned>(failure.hr),
            short_file(failure.pszFile),
            failure.uLineNumber);

    if (failure.pszFunction)
        fprintf(stderr, " in %s", failure.pszFunction);
    if (failure.pszCode)
        fprintf(stderr, " [%s]", failure.pszCode);
    if (failure.pszMessage)
        fwprintf(stderr, L" - %ls", failure.pszMessage);

    fputc('\n', stderr);
    fflush(stderr);
}

} // namespace

void install_wil_result_logger() {
    wil::SetResultLoggingCallback(&on_wil_failure);
}

} // namespace lvt
