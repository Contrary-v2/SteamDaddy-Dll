#pragma once

namespace OSTPlatform::Detour {

    [[nodiscard]] bool BeginTransaction();
    [[nodiscard]] bool CommitTransaction();
    [[nodiscard]] bool AbortTransaction();
    [[nodiscard]] bool Attach(void** target, void* detour);
    [[nodiscard]] bool Detach(void** target, void* detour);

    class TransactionGuard {
    public:
        TransactionGuard() : active_(BeginTransaction()) {}
        ~TransactionGuard() {
            if (active_) {
                static_cast<void>(AbortTransaction());
            }
        }

        TransactionGuard(const TransactionGuard&) = delete;
        TransactionGuard& operator=(const TransactionGuard&) = delete;

        TransactionGuard(TransactionGuard&& other) noexcept : active_(other.active_) {
            other.active_ = false;
        }
        TransactionGuard& operator=(TransactionGuard&& other) noexcept {
            if (this != &other) {
                if (active_) static_cast<void>(AbortTransaction());
                active_ = other.active_;
                other.active_ = false;
            }
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return active_; }

        [[nodiscard]] bool Commit() {
            if (!active_) return false;
            const bool ok = CommitTransaction();
            active_ = false;
            return ok;
        }

        void Abort() {
            if (active_) {
                static_cast<void>(AbortTransaction());
                active_ = false;
            }
        }

    private:
        bool active_ = false;
    };

} // namespace OSTPlatform::Detour
