#pragma once

#include <format>
#include <string>
#include <string_view>
#include <windows.h>

namespace OST::Manager {

enum class Language : size_t {
    English = 0,
    Chinese = 1,
    Spanish = 2,
    COUNT
};

inline constexpr size_t kLanguageCount = static_cast<size_t>(Language::COUNT);

enum class StringId {
    AppTitle,
    StatusGroup,
    StatusNotInstalled,
    StatusInstalledPortable,
    StatusInstalledRoot,
    StatusConflictOtherInstance,
    StatusBrokenOrphaned,
    MsgConflictConfirmTitle,
    MsgConflictConfirmPrompt,
    MsgUninstallOtherConfirm,
    SteamProcGroup,
    SteamRunningInjected,
    SteamRunningNotInjected,
    SteamNotRunning,
    PathsGroup,
    SteamDirLabel,
    SteamDirNotFound,
    PayloadLabel,
    PayloadNotFound,
    ActionsGroup,
    BtnInstallPortable,
    BtnInstallRoot,
    BtnUninstall,
    BtnRefresh,
    BtnOpenLogs,
    LangLabel,
    TipPortable,
    TipRoot,
    TipSteamRunningLocked,
    MsgSteamRunningBlocked,
    MsgSuccessTitle,
    MsgErrorTitle,
    MsgInstallPortableSuccess,
    MsgInstallRootSuccess,
    MsgUninstallSuccess,
    MsgDllMissing,
    MsgSteamDirMissing,
    MsgInstallFailed,
    MsgDeployFailed,
    MsgUninstallFailed,
    StatusReady,
    StatusProcessing,
    BtnInjectNow,
    BtnStartWatcher,
    BtnStopWatcher,
    MsgInjectSuccess,
    MsgInjectFailed,
    MsgWatcherStarted,
    MsgWatcherStopped,
    WatcherStatusRunning,
    WatcherStatusStopped,
    CheckAutoStart,
    MsgAutoStartSuccess,
    MsgAutoStartRemoved,
    MsgAutoStartFailed,
    BtnDeployDll,
    BtnRemoveDll,
    MsgDeployDllSuccess,
    MsgDeployDllFailed,
    MsgRemoveDllSuccess,
    MsgRemoveDllFailed,
    DllStatusGroup,
    DllStatusNotDeployed,
    DllStatusDeployedBoth,
    DllStatusDeployedVersion,
    DllStatusDeployedDwmapi,
    DllStatusMissingPayload
};

class I18n {
public:
    static void SetLanguage(Language lang) noexcept {
        if (static_cast<size_t>(lang) < kLanguageCount) {
            s_currentLang = lang;
        }
    }

    static Language GetLanguage() noexcept {
        return s_currentLang;
    }

    template <typename... Args>
    [[nodiscard]] static std::wstring Format(StringId id, const Args&... args) {
        return std::vformat(Get(id), std::make_wformat_args(args...));
    }

    static Language DetectSystemLanguage() noexcept {
        // 1. Try GetUserDefaultLocaleName (e.g. "zh-CN", "es-ES", "en-US")
        wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
        if (::GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
            if (localeName[0] == L'z' && localeName[1] == L'h') {
                return Language::Chinese;
            }
            if (localeName[0] == L'e' && localeName[1] == L's') {
                return Language::Spanish;
            }
        }

        // 2. Try GetSystemDefaultLocaleName
        if (::GetSystemDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
            if (localeName[0] == L'z' && localeName[1] == L'h') {
                return Language::Chinese;
            }
            if (localeName[0] == L'e' && localeName[1] == L's') {
                return Language::Spanish;
            }
        }

        // 3. Fallback to UI languages and LCID
        const LANGID userUi = ::GetUserDefaultUILanguage();
        const LANGID sysUi  = ::GetSystemDefaultUILanguage();
        const LCID   lcid   = ::GetUserDefaultLCID();

        const WORD p1 = PRIMARYLANGID(userUi);
        const WORD p2 = PRIMARYLANGID(sysUi);
        const WORD p3 = PRIMARYLANGID(LANGIDFROMLCID(lcid));

        if (p1 == LANG_CHINESE || p2 == LANG_CHINESE || p3 == LANG_CHINESE) {
            return Language::Chinese;
        }
        if (p1 == LANG_SPANISH || p2 == LANG_SPANISH || p3 == LANG_SPANISH) {
            return Language::Spanish;
        }
        return Language::English;
    }

    static std::wstring_view Get(StringId id) noexcept {
        const size_t langIdx = static_cast<size_t>(s_currentLang);
        switch (id) {
            case StringId::AppTitle: {
                static constexpr std::wstring_view s[] = {
                    L"OpenSteamTool Manager",
                    L"OpenSteamTool 管理器",
                    L"Administrador OpenSteamTool"
                };
                return s[langIdx];
            }
            case StringId::StatusGroup: {
                static constexpr std::wstring_view s[] = {
                    L"System IFEO Status",
                    L"系统 IFEO 状态",
                    L"Estado de IFEO"
                };
                return s[langIdx];
            }
            case StringId::StatusNotInstalled: {
                static constexpr std::wstring_view s[] = {
                    L"[○] Not Installed",
                    L"[○] 未安装",
                    L"[○] No Instalado"
                };
                return s[langIdx];
            }
            case StringId::StatusInstalledPortable: {
                static constexpr std::wstring_view s[] = {
                    L"[●] Installed (Portable)",
                    L"[●] 已安装 (便携模式)",
                    L"[●] Instalado (Portátil)"
                };
                return s[langIdx];
            }
            case StringId::StatusInstalledRoot: {
                static constexpr std::wstring_view s[] = {
                    L"[●] Installed (Steam Root)",
                    L"[●] 已安装 (根目录模式)",
                    L"[●] Instalado (Modo Raíz)"
                };
                return s[langIdx];
            }
            case StringId::StatusConflictOtherInstance: {
                static constexpr std::wstring_view s[] = {
                    L"[▲] Conflict (Other Path)",
                    L"[▲] 冲突 (其他路径接管)",
                    L"[▲] Conflicto (Otra Ruta)"
                };
                return s[langIdx];
            }
            case StringId::StatusBrokenOrphaned: {
                static constexpr std::wstring_view s[] = {
                    L"[×] Target Not Found",
                    L"[×] 目标程序不存在",
                    L"[×] Destino no encontrado"
                };
                return s[langIdx];
            }
            case StringId::SteamProcGroup: {
                static constexpr std::wstring_view s[] = {
                    L"Steam Client Status",
                    L"Steam 运行状态",
                    L"Estado de Steam"
                };
                return s[langIdx];
            }
            case StringId::SteamRunningInjected: {
                static constexpr std::wstring_view s[] = {
                    L"Steam: Running [OpenSteamTool Loaded] (PID: {})",
                    L"Steam：运行中 [已加载 OpenSteamTool] (PID: {})",
                    L"Steam: En ejecución [OpenSteamTool Cargado] (PID: {})"
                };
                return s[langIdx];
            }
            case StringId::SteamRunningNotInjected: {
                static constexpr std::wstring_view s[] = {
                    L"Steam: Running [OpenSteamTool Not Loaded] (PID: {})",
                    L"Steam：运行中 [未加载 OpenSteamTool] (PID: {})",
                    L"Steam: En ejecución [OpenSteamTool No Cargado] (PID: {})"
                };
                return s[langIdx];
            }
            case StringId::SteamNotRunning: {
                static constexpr std::wstring_view s[] = {
                    L"Steam: Not Running",
                    L"Steam：未运行",
                    L"Steam: No iniciado"
                };
                return s[langIdx];
            }
            case StringId::PathsGroup: {
                static constexpr std::wstring_view s[] = {
                    L"Target & Configuration Paths",
                    L"目标与配置路径",
                    L"Rutas y Configuración"
                };
                return s[langIdx];
            }
            case StringId::SteamDirLabel: {
                static constexpr std::wstring_view s[] = {
                    L"Steam Path:",
                    L"Steam 路径：",
                    L"Ruta Steam:"
                };
                return s[langIdx];
            }
            case StringId::SteamDirNotFound: {
                static constexpr std::wstring_view s[] = {
                    L"Steam installation not detected in registry",
                    L"未在注册表中检测到 Steam 安装路径",
                    L"No se detectó la instalación de Steam en el registro"
                };
                return s[langIdx];
            }
            case StringId::PayloadLabel: {
                static constexpr std::wstring_view s[] = {
                    L"OST Payload:",
                    L"OST 载荷：",
                    L"DLL de OST:"
                };
                return s[langIdx];
            }
            case StringId::PayloadNotFound: {
                static constexpr std::wstring_view s[] = {
                    L"OpenSteamTool.dll not found in current folder",
                    L"当前目录下未找到 OpenSteamTool.dll",
                    L"OpenSteamTool.dll no encontrado en la carpeta actual"
                };
                return s[langIdx];
            }
            case StringId::ActionsGroup: {
                static constexpr std::wstring_view s[] = {
                    L"Operations",
                    L"操作选项",
                    L"Operaciones"
                };
                return s[langIdx];
            }
            case StringId::BtnInstallPortable: {
                static constexpr std::wstring_view s[] = {
                    L"Install IFEO (Portable Mode)",
                    L"安装 IFEO (便携模式)",
                    L"Instalar IFEO (Modo Portátil)"
                };
                return s[langIdx];
            }
            case StringId::BtnInstallRoot: {
                static constexpr std::wstring_view s[] = {
                    L"Install IFEO (Steam Root Mode)",
                    L"安装 IFEO (Steam 根目录模式)",
                    L"Instalar IFEO (Modo Raíz de Steam)"
                };
                return s[langIdx];
            }
            case StringId::BtnUninstall: {
                static constexpr std::wstring_view s[] = {
                    L"Uninstall IFEO",
                    L"卸载 IFEO",
                    L"Desinstalar IFEO"
                };
                return s[langIdx];
            }
            case StringId::BtnRefresh: {
                static constexpr std::wstring_view s[] = {
                    L"Refresh Status",
                    L"刷新状态",
                    L"Actualizar"
                };
                return s[langIdx];
            }
            case StringId::BtnOpenLogs: {
                static constexpr std::wstring_view s[] = {
                    L"Open Logs",
                    L"查看运行日志",
                    L"Ver Registros"
                };
                return s[langIdx];
            }
            case StringId::LangLabel: {
                static constexpr std::wstring_view s[] = {
                    L"Language:",
                    L"界面语言：",
                    L"Idioma:"
                };
                return s[langIdx];
            }
            case StringId::TipPortable: {
                static constexpr std::wstring_view s[] = {
                    L"Tip: Portable mode leaves the Steam folder untouched and intercepts launch via Manager.",
                    L"说明：便携模式不修改 Steam 目录文件，由外部管理器接管启动与载荷注入。",
                    L"Nota: El modo portátil no modifica los archivos de Steam y gestiona la inyección desde el administrador."
                };
                return s[langIdx];
            }
            case StringId::TipRoot: {
                static constexpr std::wstring_view s[] = {
                    L"Tip: Copies OpenSteamTool.dll and ost-loader.exe to Steam directory; ost-loader handles launch injection.",
                    L"说明：将 OpenSteamTool.dll 与专用加载器 ost-loader.exe 部署到 Steam 目录，由加载器负责注入。",
                    L"Nota: Copia OpenSteamTool.dll y ost-loader.exe a Steam para inyección automática."
                };
                return s[langIdx];
            }
            case StringId::TipSteamRunningLocked: {
                static constexpr std::wstring_view s[] = {
                    L"Note: Steam is currently running. Operation buttons are locked. Please exit Steam.",
                    L"提示：检测到 Steam 正在运行，相关修改操作已锁定。请先退出 Steam。",
                    L"Nota: Steam está en ejecución. Operaciones bloqueadas. Cierre Steam primero."
                };
                return s[langIdx];
            }
            case StringId::MsgSteamRunningBlocked: {
                static constexpr std::wstring_view s[] = {
                    L"Steam is currently running (PID: {}).\n\n"
                    L"Please exit Steam before performing install, uninstall, or modification operations.",

                    L"检测到 Steam 正在运行 (PID: {})。\n\n"
                    L"为避免文件占用冲突及安全拦截，请在退出 Steam 后再执行安装、卸载或修改操作。",

                    L"Steam está en ejecución (PID: {}).\n\n"
                    L"Cierre Steam antes de realizar operaciones de instalación, desinstalación o modificación."
                };
                return s[langIdx];
            }
            case StringId::MsgSuccessTitle: {
                static constexpr std::wstring_view s[] = {
                    L"Operation Successful",
                    L"操作成功",
                    L"Operación Exitosa"
                };
                return s[langIdx];
            }
            case StringId::MsgErrorTitle: {
                static constexpr std::wstring_view s[] = {
                    L"Operation Failed",
                    L"操作失败",
                    L"Operación Fallida"
                };
                return s[langIdx];
            }
            case StringId::MsgInstallPortableSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Portable Mode IFEO installed successfully.\n\n"
                    L"• OpenSteamTool.dll remains in the manager directory.\n"
                    L"• Steam directory is unchanged.\n"
                    L"• Launching Steam will trigger injection via the manager.",

                    L"已成功注册便携模式 IFEO。\n\n"
                    L"• OpenSteamTool.dll 保持在当前管理器目录；\n"
                    L"• Steam 目录未做修改；\n"
                    L"• 启动 Steam 时将通过本管理器完成载荷加载。",

                    L"IFEO en Modo Portátil instalado con éxito.\n\n"
                    L"• OpenSteamTool.dll permanece en el directorio del administrador.\n"
                    L"• El directorio de Steam no ha sido modificado.\n"
                    L"• Al abrir Steam se gestionará la inyección mediante el administrador."
                };
                return s[langIdx];
            }
            case StringId::MsgInstallRootSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Successfully deployed to Steam directory and registered IFEO.\n\n"
                    L"• OpenSteamTool.dll and ost-loader.exe copied to Steam folder.\n"
                    L"• IFEO configured to launch ost-loader.exe.\n"
                    L"• Launching Steam will trigger injection via ost-loader.",

                    L"已成功部署至 Steam 根目录并注册 IFEO。\n\n"
                    L"• 已将 OpenSteamTool.dll 与 ost-loader.exe 复制到 Steam 目录；\n"
                    L"• IFEO 已配置为由 ost-loader.exe 接管启动；\n"
                    L"• 启动 Steam 时将由同目录加载器自动加载载荷。",

                    L"Desplegado con éxito al directorio de Steam e IFEO registrado.\n\n"
                    L"• OpenSteamTool.dll y ost-loader.exe copiados a la carpeta de Steam.\n"
                    L"• IFEO configurado para ejecutar ost-loader.exe.\n"
                    L"• Al abrir Steam se gestionará la inyección mediante ost-loader."
                };
                return s[langIdx];
            }
            case StringId::MsgUninstallSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"IFEO unregistered successfully.\n\n"
                    L"• Registry IFEO entry removed.\n"
                    L"• Deployed files (OpenSteamTool.dll, ost-loader.exe, config) in Steam folder cleaned up if present.",

                    L"IFEO 已注销，相关部署文件已清理。\n\n"
                    L"• 注册表 IFEO 项已删除；\n"
                    L"• Steam 根目录下的 OpenSteamTool.dll、ost-loader.exe 及配置文件已清理（若存在）。",

                    L"IFEO desinstalado con éxito.\n\n"
                    L"• Entrada de IFEO eliminada del registro.\n"
                    L"• Archivos desplegados en la carpeta de Steam eliminados si existían."
                };
                return s[langIdx];
            }
            case StringId::MsgDllMissing: {
                static constexpr std::wstring_view s[] = {
                    L"OpenSteamTool.dll was not found in the current directory:\n{}\n\n"
                    L"Please ensure the DLL is compiled and placed alongside ost-manager.exe.",

                    L"未在当前目录下找到 OpenSteamTool.dll：\n{}\n\n"
                    L"请确保该 DLL 已完成构建并与 ost-manager.exe 位于同一文件夹。",

                    L"No se encontró OpenSteamTool.dll en el directorio actual:\n{}\n\n"
                    L"Asegúrese de que el DLL esté compilado y junto a ost-manager.exe."
                };
                return s[langIdx];
            }
            case StringId::MsgSteamDirMissing: {
                static constexpr std::wstring_view s[] = {
                    L"Cannot detect Steam installation path. Please ensure Steam is installed on this computer.",
                    L"无法自动定位 Steam 安装路径，请确认本机已安装 Steam 客户端。",
                    L"No se pudo detectar la instalación de Steam. Asegúrese de que esté instalado."
                };
                return s[langIdx];
            }
            case StringId::MsgInstallFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to install IFEO:\n{}",
                    L"安装启动 IFEO 失败：\n{}",
                    L"Error al instalar IFEO:\n{}"
                };
                return s[langIdx];
            }
            case StringId::MsgDeployFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to deploy files to Steam directory:\n{}",
                    L"部署文件到 Steam 目录失败：\n{}",
                    L"Error al copiar archivos a la carpeta de Steam:\n{}"
                };
                return s[langIdx];
            }
            case StringId::MsgUninstallFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to uninstall IFEO:\n{}",
                    L"卸载启动 IFEO 失败：\n{}",
                    L"Error al desinstalar IFEO:\n{}"
                };
                return s[langIdx];
            }
            case StringId::StatusReady: {
                static constexpr std::wstring_view s[] = {
                    L"Ready. Please select an operation.",
                    L"就绪。请选择所需操作。",
                    L"Listo. Seleccione una operación."
                };
                return s[langIdx];
            }
            case StringId::StatusProcessing: {
                static constexpr std::wstring_view s[] = {
                    L"Processing... Please wait.",
                    L"正在处理，请稍候...",
                    L"Procesando... Por favor espere."
                };
                return s[langIdx];
            }
            case StringId::BtnInjectNow: {
                static constexpr std::wstring_view s[] = {
                    L"Launch & Inject Steam",
                    L"冷启动拉起并注入",
                    L"Iniciar e inyectar Steam"
                };
                return s[langIdx];
            }
            case StringId::BtnStartWatcher: {
                static constexpr std::wstring_view s[] = {
                    L"Start Background Watcher",
                    L"启动后台守护监听",
                    L"Iniciar monitor en segundo plano"
                };
                return s[langIdx];
            }
            case StringId::BtnStopWatcher: {
                static constexpr std::wstring_view s[] = {
                    L"Stop Background Watcher",
                    L"停止后台守护监听",
                    L"Detener monitor en segundo plano"
                };
                return s[langIdx];
            }
            case StringId::MsgInjectSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Steam launched and OpenSteamTool injected successfully (PID: {}).",
                    L"已启动 Steam 并加载 OpenSteamTool (PID: {})。",
                    L"Steam iniciado y OpenSteamTool inyectado con éxito (PID: {})."
                };
                return s[langIdx];
            }
            case StringId::MsgInjectFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to launch or inject Steam:\n{}",
                    L"启动或注入 Steam 失败：\n{}",
                    L"Error al iniciar o inyectar en Steam:\n{}"
                };
                return s[langIdx];
            }
            case StringId::MsgWatcherStarted: {
                static constexpr std::wstring_view s[] = {
                    L"Background watcher daemon started successfully!\n\n"
                    L"• Monitors Steam process in background and performs injection.\n"
                    L"• Configured auto-start on logon (HKCU Run).",

                    L"后台守护监听已成功启动！\n\n"
                    L"• 守护进程已在后台运行，检测到 Steam 启动时将自动加载载荷；\n"
                    L"• 已配置开机免 UAC 自启 (HKCU Run)。",

                    L"¡Monitor en segundo plano iniciado con éxito!\n\n"
                    L"• Se ejecuta en segundo plano e inyecta al iniciar Steam.\n"
                    L"• Configurado para iniciar al iniciar sesión (HKCU Run)."
                };
                return s[langIdx];
            }
            case StringId::MsgWatcherStopped: {
                static constexpr std::wstring_view s[] = {
                    L"Background watcher daemon stopped.\n\n"
                    L"• Background watcher process exited.\n"
                    L"• Auto-start entry removed from registry.",

                    L"后台守护监听已停止。\n\n"
                    L"• 后台守护进程已安全退出；\n"
                    L"• 已注销并清除开机自启注册表项 (HKCU Run)。",

                    L"El monitor en segundo plano ha sido detenido.\n\n"
                    L"• Proceso detenido correctamente.\n"
                    L"• Entrada de inicio automático eliminada."
                };
                return s[langIdx];
            }
            case StringId::WatcherStatusRunning: {
                static constexpr std::wstring_view s[] = {
                    L"Watcher: Running",
                    L"守护状态：运行中",
                    L"Monitor: En ejecución"
                };
                return s[langIdx];
            }
            case StringId::WatcherStatusStopped: {
                static constexpr std::wstring_view s[] = {
                    L"Watcher: Stopped",
                    L"守护状态：未运行",
                    L"Monitor: Detenido"
                };
                return s[langIdx];
            }
            case StringId::CheckAutoStart: {
                static constexpr std::wstring_view s[] = {
                    L"Auto-start watcher on logon (HKCU Run)",
                    L"登录系统时开机自启守护进程 (HKCU Run)",
                    L"Iniciar monitor al iniciar sesión (HKCU Run)"
                };
                return s[langIdx];
            }
            case StringId::MsgAutoStartSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Auto-start on logon configured successfully.\n\n"
                    L"Registered in current user startup registry (HKCU Run).",

                    L"已配置开机自启守护。\n\n"
                    L"已写入当前用户注册表启动项 (HKCU Run)。",

                    L"Inicio automático configurado con éxito en HKCU Run."
                };
                return s[langIdx];
            }
            case StringId::MsgAutoStartRemoved: {
                static constexpr std::wstring_view s[] = {
                    L"Auto-start on logon removed successfully.\n\n"
                    L"Startup item deleted from current user registry (HKCU Run).",

                    L"开机自启守护已注销。\n\n"
                    L"已从当前用户启动项 (HKCU Run) 中清除。",

                    L"Inicio automático eliminado con éxito de HKCU Run."
                };
                return s[langIdx];
            }
            case StringId::MsgAutoStartFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to configure auto-start registry item.",
                    L"写入或修改开机自启注册表失败。",
                    L"Error al configurar el registro de inicio automático."
                };
                return s[langIdx];
            }
            case StringId::MsgConflictConfirmTitle: {
                static constexpr std::wstring_view s[] = {
                    L"IFEO Target Conflict Detected",
                    L"检测到系统 IFEO 路径冲突",
                    L"Conflicto de destino IFEO"
                };
                return s[langIdx];
            }
            case StringId::MsgConflictConfirmPrompt: {
                static constexpr std::wstring_view s[] = {
                    L"Current IFEO registration points to another location:\n\n"
                    L"【Existing Registered Path】:\n{}\n\n"
                    L"【Current Program Path】:\n{}\n\n"
                    L"Do you want to overwrite and rebind IFEO to this location?",

                    L"检测到当前系统 IFEO 已指向其他路径：\n\n"
                    L"【已注册的目标路径】:\n{}\n\n"
                    L"【当前程序所在路径】:\n{}\n\n"
                    L"是否确认覆盖原有的 IFEO 注册项并重新绑定到当前位置？",

                    L"Se detectó un registro IFEO existente en otra ubicación:\n\n"
                    L"[Ubicación registrada]:\n{}\n\n"
                    L"[Ubicación actual]:\n{}\n\n"
                    L"¿Desea sobrescribir y reasignar el IFEO?"
                };
                return s[langIdx];
            }
            case StringId::MsgUninstallOtherConfirm: {
                static constexpr std::wstring_view s[] = {
                    L"The currently registered IFEO points to another location:\n\n{}\n\n"
                    L"Do you still want to remove this registration?",

                    L"当前系统注册的 IFEO 指向其他路径：\n\n{}\n\n"
                    L"是否仍要注销此 IFEO 注册项？",

                    L"El IFEO registrado actualmente apunta a otra ubicación:\n\n{}\n\n"
                    L"¿Desea desinstalarlo?"
                };
                return s[langIdx];
            }
            case StringId::BtnDeployDll: {
                static constexpr std::wstring_view s[] = {
                    L"Deploy Standard DLL",
                    L"部署标准DLL",
                    L"Desplegar DLL estándar"
                };
                return s[langIdx];
            }
            case StringId::BtnRemoveDll: {
                static constexpr std::wstring_view s[] = {
                    L"Remove DLL",
                    L"移除DLL",
                    L"Eliminar DLL"
                };
                return s[langIdx];
            }
            case StringId::MsgDeployDllSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Standard DLLs deployed successfully.\n\n"
                    L"• Steam root: OpenSteamTool.dll, version.dll\n"
                    L"• Steam\\bin\\: dwmapi.dll",

                    L"标准 DLL 部署成功。\n\n"
                    L"• Steam 根目录：OpenSteamTool.dll、version.dll\n"
                    L"• Steam\\bin\\ 目录：dwmapi.dll",

                    L"DLLs estándar desplegados con éxito.\n\n"
                    L"• Raíz de Steam: OpenSteamTool.dll, version.dll\n"
                    L"• Carpeta Steam\\bin\\: dwmapi.dll"
                };
                return s[langIdx];
            }
            case StringId::DllStatusGroup: {
                static constexpr std::wstring_view s[] = {
                    L"Standard DLL Status",
                    L"标准 DLL 状态",
                    L"Estado DLL Estándar"
                };
                return s[langIdx];
            }
            case StringId::DllStatusNotDeployed: {
                static constexpr std::wstring_view s[] = {
                    L"[-] Not Deployed",
                    L"[-] 未部署",
                    L"[-] No Desplegado"
                };
                return s[langIdx];
            }
            case StringId::DllStatusDeployedBoth: {
                static constexpr std::wstring_view s[] = {
                    L"[√] Deployed (version+dwmapi)",
                    L"[√] 已部署 (version + dwmapi)",
                    L"[√] Desplegado (ambos)"
                };
                return s[langIdx];
            }
            case StringId::DllStatusDeployedVersion: {
                static constexpr std::wstring_view s[] = {
                    L"[√] Deployed (version.dll)",
                    L"[√] 已部署 (version.dll)",
                    L"[√] Desplegado (version.dll)"
                };
                return s[langIdx];
            }
            case StringId::DllStatusDeployedDwmapi: {
                static constexpr std::wstring_view s[] = {
                    L"[√] Deployed (dwmapi.dll)",
                    L"[√] 已部署 (dwmapi.dll)",
                    L"[√] Desplegado (dwmapi.dll)"
                };
                return s[langIdx];
            }
            case StringId::DllStatusMissingPayload: {
                static constexpr std::wstring_view s[] = {
                    L"[!] Missing OpenSteamTool",
                    L"[!] 缺少 OpenSteamTool.dll",
                    L"[!] Falta OpenSteamTool.dll"
                };
                return s[langIdx];
            }
            case StringId::MsgDeployDllFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to deploy standard DLL: {}",
                    L"部署标准 DLL 失败：{}",
                    L"Error al desplegar DLL estándar: {}"
                };
                return s[langIdx];
            }
            case StringId::MsgRemoveDllSuccess: {
                static constexpr std::wstring_view s[] = {
                    L"Standard DLL files removed successfully.",
                    L"标准 DLL 文件已成功移除。",
                    L"Archivos DLL estándar eliminados con éxito."
                };
                return s[langIdx];
            }
            case StringId::MsgRemoveDllFailed: {
                static constexpr std::wstring_view s[] = {
                    L"Failed to remove DLL files: {}",
                    L"移除 DLL 文件失败：{}",
                    L"Error al eliminar archivos DLL: {}"
                };
                return s[langIdx];
            }
        }
        return L"";
    }

private:
    inline static Language s_currentLang = Language::English;
};

} // namespace OST::Manager
