#include "include/Thread.h"

#include "include/Log.h"

#include <windows.h>
#include <process.h>

#include <memory>

namespace OSTPlatform::Thread {

NativeThreadHandle CurrentNativeThreadHandle() noexcept {
    return ::GetCurrentThread();
}

bool StartDetached(std::function<uint32_t()> entry) {
    if (!entry) {
        OSTP_LOG_WARN("StartDetached: entry is empty");
        return false;
    }

    auto* heapEntry = new std::function<uint32_t()>(std::move(entry));
    const uintptr_t thread = ::_beginthreadex(
        nullptr,
        0,
        [](void* param) -> unsigned int {
            std::unique_ptr<std::function<uint32_t()>> fn(static_cast<std::function<uint32_t()>*>(param));
            try {
                return static_cast<unsigned int>((*fn)());
            } catch (const std::exception& e) {
                OSTP_LOG_ERROR("StartDetached thread caught exception: {}", e.what());
                return 1;
            } catch (...) {
                OSTP_LOG_ERROR("StartDetached thread caught unknown exception");
                return 1;
            }
        },
        heapEntry,
        0,
        nullptr);

    if (thread == 0) {
        OSTP_LOG_WARN("_beginthreadex failed (error={})", ::GetLastError());
        delete heapEntry;
        return false;
    }

    ::CloseHandle(reinterpret_cast<HANDLE>(thread));
    return true;
}

} // namespace OSTPlatform::Thread
