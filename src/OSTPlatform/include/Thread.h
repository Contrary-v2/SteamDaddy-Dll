#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

namespace OSTPlatform::Thread {

    using NativeThreadHandle = void*;

    [[nodiscard]] NativeThreadHandle CurrentNativeThreadHandle() noexcept;
    bool StartDetached(std::function<uint32_t()> entry);

    template <typename Fn>
        requires std::invocable<Fn> && std::same_as<std::invoke_result_t<Fn>, void>
    inline bool StartDetached(Fn&& entry) {
        return StartDetached(std::function<uint32_t()>([fn = std::forward<Fn>(entry)]() mutable -> uint32_t {
            fn();
            return 0;
        }));
    }

    // RAII thread wrapper that detaches on destruction if still joinable.
    // In Windows DLL lifecycles, when ExitProcess is invoked, the OS terminates all
    // secondary threads before invoking DllMain(DLL_PROCESS_DETACH). If a std::thread
    // is destructed while joinable, the C++ standard requires std::terminate() -> abort().
    // SafeThread guarantees that the underlying thread is detached rather than terminated.
    class SafeThread {
    private:
        std::thread m_thread;

    public:
        SafeThread() noexcept = default;
        explicit SafeThread(std::thread&& t) noexcept : m_thread(std::move(t)) {}

        template <typename Fn, typename... Args>
            requires (!std::is_same_v<std::remove_cvref_t<Fn>, SafeThread> &&
                      !std::is_same_v<std::remove_cvref_t<Fn>, std::thread>)
        explicit SafeThread(Fn&& fn, Args&&... args)
            : m_thread(std::forward<Fn>(fn), std::forward<Args>(args)...) {}

        SafeThread(SafeThread&& other) noexcept : m_thread(std::move(other.m_thread)) {}

        SafeThread& operator=(SafeThread&& other) noexcept {
            if (this != &other) {
                if (m_thread.joinable()) {
                    m_thread.detach();
                }
                m_thread = std::move(other.m_thread);
            }
            return *this;
        }

        SafeThread& operator=(std::thread&& other) noexcept {
            if (m_thread.joinable()) {
                m_thread.detach();
            }
            m_thread = std::move(other);
            return *this;
        }

        ~SafeThread() {
            if (m_thread.joinable()) {
                m_thread.detach();
            }
        }

        SafeThread(const SafeThread&) = delete;
        SafeThread& operator=(const SafeThread&) = delete;

        [[nodiscard]] bool joinable() const noexcept { return m_thread.joinable(); }

        void join() {
            if (m_thread.joinable()) {
                if (m_thread.get_id() == std::this_thread::get_id()) {
                    m_thread.detach();
                    return;
                }
                m_thread.join();
            }
        }

        void detach() noexcept {
            if (m_thread.joinable()) {
                m_thread.detach();
            }
        }

        [[nodiscard]] std::thread::id get_id() const noexcept { return m_thread.get_id(); }
        [[nodiscard]] std::thread::native_handle_type native_handle() noexcept { return m_thread.native_handle(); }

        void swap(SafeThread& other) noexcept {
            m_thread.swap(other.m_thread);
        }
    };

    inline void swap(SafeThread& lhs, SafeThread& rhs) noexcept {
        lhs.swap(rhs);
    }

} // namespace OSTPlatform::Thread
