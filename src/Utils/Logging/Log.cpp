#include "Log.h"

#ifdef STEAMDADDY_LOGGING_ENABLED

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Log.h"
#include "Utils/Config/Config.h"
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

namespace {
    std::atomic_bool g_mainReady{false};

    spdlog::level::level_enum ToSpdlog(Config::LogLevel lv) noexcept {
        switch (lv) {
        case Config::LogLevel::Trace: return spdlog::level::trace;
        case Config::LogLevel::Debug: return spdlog::level::debug;
        case Config::LogLevel::Info:  return spdlog::level::info;
        case Config::LogLevel::Warn:  return spdlog::level::warn;
        case Config::LogLevel::Error: return spdlog::level::err;
        default: return spdlog::level::info;
        }
    }

    std::shared_ptr<spdlog::logger> MakeLogger(const std::string& dir,
                                                const std::string& name) {
        auto path = OSTPlatform::Encoding::PathFromUtf8(dir) / (name + ".log");
        spdlog::drop(name);

#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
        auto logger = spdlog::basic_logger_mt(name, path.wstring(), /*truncate=*/true);
#else
        const std::string filename = OSTPlatform::Encoding::PathToUtf8(path);
        auto logger = spdlog::basic_logger_mt(name, filename, /*truncate=*/true);
#endif
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid=%t] [%s:%# %!()] %v");
        logger->flush_on(spdlog::level::trace);
        return logger;
    }

    spdlog::level::level_enum ToSpdlog(OSTPlatform::Log::Level lv) noexcept {
        switch (lv) {
        case OSTPlatform::Log::Level::Trace: return spdlog::level::trace;
        case OSTPlatform::Log::Level::Debug: return spdlog::level::debug;
        case OSTPlatform::Log::Level::Info:  return spdlog::level::info;
        case OSTPlatform::Log::Level::Warn:  return spdlog::level::warn;
        case OSTPlatform::Log::Level::Error: return spdlog::level::err;
        }
        return spdlog::level::info;
    }

    void SetLoggerLevel(const std::shared_ptr<spdlog::logger>& logger,
                        spdlog::level::level_enum level) noexcept {
        if (logger) logger->set_level(level);
    }

    void SetAllLoggerLevels(spdlog::level::level_enum level) noexcept {
        SetLoggerLevel(Log::Main, level);
        #define OST_MOD(v, n) SetLoggerLevel(Log::v, level);
        #include "LogModules.def"
        #undef OST_MOD
    }

    // Forwards every OSTPlatform log line into the host's single "platform"
    // logger. The message is already formatted; the originating file/line/
    // function are reconstructed into spdlog's source_loc so the log pattern
    // still shows where in the platform library the line came from.
    void PlatformLogSink(OSTPlatform::Log::Level level,
                         const OSTPlatform::Log::Source& src,
                         std::string_view msg) noexcept {
        const auto spdLevel = ToSpdlog(level);
        if (!Log::Platform || !Log::Platform->should_log(spdLevel)) return;
        try {
            Log::Platform->log(
                spdlog::source_loc{src.file ? src.file : "", src.line,
                                   src.function ? src.function : ""},
                spdLevel, "{}", msg);
        } catch (...) {
        }
    }
}

namespace Log {

    void Init(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) noexcept {
        bool expected = false;
        if (!g_mainReady.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

        try {
            auto dir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
            if (dir.empty()) dir = ".";
            auto logDirPath = dir / "steamdaddy";
            std::error_code ec;
            std::filesystem::create_directories(logDirPath, ec);
            auto logDir = OSTPlatform::Encoding::PathToUtf8(logDirPath);
            Main = MakeLogger(logDir, "main");
            Main->set_level(spdlog::level::trace);  // early boot: log everything
            LOG_INFO("Log initialised at {}", logDir);
        } catch (const std::exception&) {
            Main.reset();
            g_mainReady.store(false, std::memory_order_release);
        } catch (...) {
            Main.reset();
            g_mainReady.store(false, std::memory_order_release);
        }
    }

    void InitModules() noexcept {
        if (!g_mainReady.load(std::memory_order_acquire)) return;

        try {
            std::string logDir = Config::GetLogDir();
            if (logDir.empty()) logDir = ".";
            const Config::LogLevel configLevel = Config::GetLogLevel();
            auto lvl = ToSpdlog(configLevel);

            std::error_code ec;
            std::filesystem::create_directories(OSTPlatform::Encoding::PathFromUtf8(logDir), ec);
            SetLoggerLevel(Main, lvl);

            auto initOne = [&](std::shared_ptr<spdlog::logger>& logger, const char* name) {
                try {
                    logger = MakeLogger(logDir, name);
                    if (logger) logger->set_level(lvl);
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to create logger for {}: {}", name, e.what());
                    logger.reset();
                } catch (...) {
                    LOG_WARN("Failed to create logger for {}: unknown error", name);
                    logger.reset();
                }
            };

            #define OST_MOD(v, n) initOne(v, n);
            #include "LogModules.def"
            #undef OST_MOD

            LOG_INFO("Module loggers initialised at {} level={}", logDir,
                     static_cast<int>(configLevel));
        } catch (const std::exception& e) {
            LOG_WARN("InitModules failed: {}", e.what());
        } catch (...) {
        }
    }

    void ApplyConfigLevel() noexcept {
        if (!g_mainReady.load(std::memory_order_acquire)) return;

        try {
            const Config::LogLevel configLevel = Config::GetLogLevel();
            SetAllLoggerLevels(ToSpdlog(configLevel));
            LOG_INFO("Log level updated to {}", static_cast<int>(configLevel));
        } catch (...) {
        }
    }

    void InstallPlatformLogSink() noexcept {
        try {
            OSTPlatform::Log::SetSink(&PlatformLogSink);
            LOG_INFO("OSTPlatform log sink installed -> platform module");
        } catch (...) {
        }
    }

    void Shutdown() noexcept {
        bool expected = true;
        if (!g_mainReady.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;

        try {
            OSTPlatform::Log::SetSink(nullptr);

            if (Main) {
                Main->flush();
            }
            #define OST_MOD(v, n) if (Log::v) Log::v->flush();
            #include "LogModules.def"
            #undef OST_MOD

            spdlog::drop_all();

            Main.reset();
            #define OST_MOD(v, n) Log::v.reset();
            #include "LogModules.def"
            #undef OST_MOD

            spdlog::shutdown();
        } catch (...) {
        }
    }

}

#else

namespace {
[[maybe_unused]] int s_logTranslationUnit = 0;
}

#endif  // STEAMDADDY_LOGGING_ENABLED
