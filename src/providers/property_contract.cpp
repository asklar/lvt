#include "framework_connection.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace lvt {
namespace {

std::string simple_type_name(std::string_view type) {
    const auto separator = type.find_last_of(".:");
    std::string name(type.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return name;
}

} // namespace

const char* property_editor_kind_name(PropertyEditorKind kind) {
    switch (kind) {
    case PropertyEditorKind::readonly: return "readonly";
    case PropertyEditorKind::string: return "string";
    case PropertyEditorKind::boolean: return "boolean";
    case PropertyEditorKind::integer: return "integer";
    case PropertyEditorKind::number: return "number";
    case PropertyEditorKind::enumeration: return "enum";
    case PropertyEditorKind::command: return "command";
    }
    return "readonly";
}

PropertyEditorKind classify_property_editor(
    std::string_view declaredType, bool writable) {
    if (!writable || declaredType.empty())
        return PropertyEditorKind::readonly;

    const auto type = simple_type_name(declaredType);
    if (type == "boolean" || type == "bool")
        return PropertyEditorKind::boolean;
    if (type == "byte" || type == "sbyte" ||
        type == "int16" || type == "uint16" ||
        type == "int32" || type == "uint32" ||
        type == "int64" || type == "uint64" ||
        type == "integer") {
        return PropertyEditorKind::integer;
    }
    if (type == "single" || type == "double" ||
        type == "decimal" || type == "float" ||
        type == "number") {
        return PropertyEditorKind::number;
    }
    if (type == "enum" || type == "enumeration")
        return PropertyEditorKind::enumeration;

    // xamlOM can report custom scalar type names. The provider still owns
    // conversion, so a plain text editor preserves that existing capability
    // without teaching the Viewer framework-specific type catalogs.
    return PropertyEditorKind::string;
}

const char* property_error_disposition_name(
    PropertyErrorDisposition disposition) {
    switch (disposition) {
    case PropertyErrorDisposition::terminal: return "terminal";
    case PropertyErrorDisposition::transient: return "transient";
    case PropertyErrorDisposition::ownershipLost: return "ownershipLost";
    case PropertyErrorDisposition::unspecified: break;
    }
    return "";
}

PropertyMutationResult property_mutation_failure(
    HRESULT hresult, std::string error, std::string errorCode,
    PropertyErrorDisposition disposition) {
    const auto win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
        ? HRESULT_CODE(hresult)
        : ERROR_SUCCESS;
    const bool canRefineCode =
        errorCode.empty() ||
        errorCode == "typed_property_mutation_failed";
    if (canRefineCode) {
        std::string normalized = error;
        std::transform(
            normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        if (normalized.find("readback") != std::string::npos ||
            normalized.find("actual value is unknown") !=
                std::string::npos) {
            errorCode = "typed_property_readback_failed";
        } else if (
            normalized.find("timeout") != std::string::npos ||
            normalized.find("timed out") != std::string::npos ||
            normalized.find("did not complete") != std::string::npos) {
            errorCode = "typed_property_timeout";
        } else if (
            normalized.find("broken pipe") != std::string::npos ||
            normalized.find("connection is no longer available") !=
                std::string::npos ||
            normalized.find("could not send") != std::string::npos) {
            errorCode = "typed_property_transport_error";
        } else if (
            normalized.find("stale") != std::string::npos ||
            normalized.find("changed since") != std::string::npos ||
            normalized.find("disappeared") != std::string::npos ||
            normalized.find("became unavailable") != std::string::npos ||
            normalized.find("not found") != std::string::npos ||
            normalized.find("unknown or closed") != std::string::npos) {
            errorCode = "typed_property_stale_element";
        }
    }

    if (disposition == PropertyErrorDisposition::unspecified) {
        if (errorCode == "typed_property_readback_failed" ||
            errorCode == "typed_property_timeout" ||
            errorCode == "typed_property_transport_error") {
            disposition = PropertyErrorDisposition::transient;
        } else if (errorCode == "typed_property_stale_element") {
            disposition = PropertyErrorDisposition::terminal;
        } else if (
            hresult == HRESULT_FROM_WIN32(ERROR_INVALID_OWNER) ||
            hresult == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)) {
            disposition = PropertyErrorDisposition::ownershipLost;
        } else if (
            win32 == ERROR_BROKEN_PIPE ||
            win32 == ERROR_PIPE_NOT_CONNECTED ||
            win32 == ERROR_NO_DATA ||
            win32 == ERROR_TIMEOUT ||
            win32 == ERROR_SEM_TIMEOUT ||
            win32 == WAIT_TIMEOUT ||
            win32 == ERROR_BUSY ||
            win32 == ERROR_RETRY ||
            win32 == RPC_S_SERVER_UNAVAILABLE ||
            win32 == RPC_S_CALL_FAILED ||
            win32 == RPC_S_CALL_FAILED_DNE ||
            hresult == RPC_E_CALL_REJECTED ||
            hresult == RPC_E_SERVERCALL_RETRYLATER ||
            hresult == RPC_E_SERVER_DIED ||
            hresult == RPC_E_DISCONNECTED) {
            disposition = PropertyErrorDisposition::transient;
        } else if (
            hresult == E_INVALIDARG ||
            hresult == E_ACCESSDENIED ||
            hresult == E_NOTIMPL ||
            hresult == E_BOUNDS ||
            win32 == ERROR_NOT_SUPPORTED ||
            win32 == ERROR_NOT_FOUND ||
            win32 == ERROR_INVALID_INDEX ||
            win32 == ERROR_INVALID_STATE ||
            win32 == ERROR_INVALID_DATA) {
            disposition = PropertyErrorDisposition::terminal;
        } else {
            disposition = PropertyErrorDisposition::transient;
        }
    }

    if (errorCode.empty() ||
        errorCode == "typed_property_mutation_failed") {
        if (disposition == PropertyErrorDisposition::ownershipLost) {
            errorCode = "typed_property_ownership_lost";
        } else if (hresult == E_INVALIDARG) {
            errorCode = "typed_property_invalid_value";
        } else if (hresult == E_ACCESSDENIED) {
            errorCode = "typed_property_read_only";
        } else if (
            hresult == E_NOTIMPL ||
            win32 == ERROR_NOT_SUPPORTED) {
            errorCode = "typed_property_unsupported";
        } else if (
            hresult == E_BOUNDS ||
            win32 == ERROR_INVALID_INDEX) {
            errorCode = "typed_property_out_of_bounds";
        } else if (
            win32 == ERROR_NOT_FOUND ||
            win32 == ERROR_INVALID_STATE) {
            errorCode = "typed_property_stale_element";
        } else if (
            win32 == ERROR_TIMEOUT ||
            win32 == ERROR_SEM_TIMEOUT ||
            win32 == WAIT_TIMEOUT) {
            errorCode = "typed_property_timeout";
        } else if (
            win32 == ERROR_BUSY ||
            win32 == ERROR_RETRY ||
            hresult == RPC_E_CALL_REJECTED ||
            hresult == RPC_E_SERVERCALL_RETRYLATER) {
            errorCode = "typed_property_provider_busy";
        } else if (
            win32 == ERROR_BROKEN_PIPE ||
            win32 == ERROR_PIPE_NOT_CONNECTED ||
            win32 == ERROR_NO_DATA ||
            win32 == RPC_S_SERVER_UNAVAILABLE ||
            win32 == RPC_S_CALL_FAILED ||
            win32 == RPC_S_CALL_FAILED_DNE ||
            hresult == RPC_E_SERVER_DIED ||
            hresult == RPC_E_DISCONNECTED) {
            errorCode = "typed_property_transport_error";
        } else {
            errorCode = "typed_property_mutation_failed";
        }
    }

    PropertyMutationResult result;
    result.hresult = hresult;
    result.error = std::move(error);
    result.errorCode = std::move(errorCode);
    result.errorDisposition = disposition;
    result.retryable = disposition == PropertyErrorDisposition::transient;
    return result;
}

} // namespace lvt
