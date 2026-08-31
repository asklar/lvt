#pragma once

#include <Windows.h>

namespace lvt::detail {

inline void cancel_and_complete_overlapped(HANDLE handle, OVERLAPPED& ov) {
    // OVERLAPPED and its associated buffers are often stack-owned. CancelIoEx
    // only requests cancellation; wait for this exact operation and consume
    // its terminal result before allowing that storage to leave scope.
    (void)CancelIoEx(handle, &ov);
    WaitForSingleObject(ov.hEvent, INFINITE);
    DWORD completed = 0;
    (void)GetOverlappedResult(handle, &ov, &completed, FALSE);
}

} // namespace lvt::detail
