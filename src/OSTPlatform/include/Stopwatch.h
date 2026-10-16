#pragma once

#include <chrono>

namespace OSTPlatform {

class Stopwatch {
public:
    [[nodiscard]] double ElapsedMs() const noexcept {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

} // namespace OSTPlatform
