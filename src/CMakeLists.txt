cmake_minimum_required(VERSION 3.20)
project(SteamDaddy VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
string(TIMESTAMP OST_BUILD_TIMESTAMP "%Y%m%d_%H%M")

# Automatic version resolution (Release Tag -> CI PR -> Git short hash -> Build Timestamp)
if(NOT DEFINED STEAMDADDY_VERSION OR STEAMDADDY_VERSION STREQUAL "" OR STEAMDADDY_VERSION STREQUAL "${PROJECT_VERSION}")
    find_package(Git QUIET)
    set(OST_RESOLVED_VERSION "")
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../.git")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE OST_GIT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(OST_GIT_TAG)
            set(OST_RESOLVED_VERSION "${OST_GIT_TAG}")
        else()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE OST_GIT_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(OST_GIT_COMMIT)
                set(OST_RESOLVED_VERSION "${OST_GIT_COMMIT}")
            endif()
        endif()
    endif()

    if(NOT OST_RESOLVED_VERSION)
        set(OST_RESOLVED_VERSION "${OST_BUILD_TIMESTAMP}")
    endif()

    set(STEAMDADDY_VERSION "${OST_RESOLVED_VERSION}" CACHE STRING "Version string embedded into SteamDaddy.dll" FORCE)
else()
    set(STEAMDADDY_VERSION "${STEAMDADDY_VERSION}" CACHE STRING "Version string embedded into SteamDaddy.dll" FORCE)
endif()

# Allow CMAKE_MSVC_RUNTIME_LIBRARY to control runtime selection for all targets,
# including dependencies pulled in via FetchContent.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Static MSVC runtime everywhere, so the resulting DLL has no extra runtime
# dependencies. Must be set BEFORE FetchContent_MakeAvailable so the fetched
# deps (Lua, Detours, spdlog) inherit it.
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Direct all static libraries and import libraries (.lib, .exp) to a dedicated
# lib/ subdirectory so Release and Debug distribution folders contain only runtime artifacts.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib/Debug")


if(NOT WIN32)
    message(STATUS "SteamDaddy: no targets on non-Windows platforms; skipping.")
    return()
endif()

add_compile_definitions(NOMINMAX)
if(MSVC)
    add_compile_options(
        /utf-8
        /EHsc
        $<$<CONFIG:Release>:/O1>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/GF>
    )
    add_link_options(
        "/DYNAMICBASE"
        "/NXCOMPAT"
        "/HIGHENTROPYVA"
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
    )
endif()



file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SteamDaddyBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SteamDaddyBuildInfo.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Proxy DLL targets — dwmapi.dll and version.dll
# Defined early so tools (ost-manager) can embed their binaries.
# ---------------------------------------------------------------------------
add_library(dwmapi SHARED
    dwmapi/dwmapi.cpp
    dwmapi/dwmapi.def
    dwmapi/dwmapi.rc
)

add_library(version SHARED
    version/version.cpp
    version/version.def
    version/version.rc
)

# ---------------------------------------------------------------------------
# Dependency recipes (FetchContent-backed, cached at <repo>/.deps).
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(STEAMDADDY_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
include(Lua)
include(Spdlog)
include(Protobuf)
include(Tomlplusplus)
include(LogMacros)

# Build the host code generators (ipc_codegen) in their own binary subtree so
# their executables never land in the shipped output directory. EXCLUDE_FROM_ALL
# means only the tools actually depended on (ipc_codegen) get built here.
add_subdirectory("${STEAMDADDY_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
include("${STEAMDADDY_TOOLS_DIR}/cmake/IPCCodegen.cmake")

set(IPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd")
# Per-config output dir, mirroring the protobuf recipe below: a Multi-Config
# build invokes the generator once per config, and a single shared output file
# would ping-pong between the Release/Debug graphs and force needless rebuilds.
steamdaddy_add_ipc_codegen(IPC_GEN
    IDL "${IPC_IDL}"
    CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Protobuf code generation — one variant per config, from the same .proto:
#
#   Debug   → full Message     (protoc --cpp_out)       → links libprotobuf
#   Release → lite MessageLite (protoc --cpp_out=lite:) → links libprotobuf-lite
#
# Output lands in the per-config generated/ dir (same root as the IPC header),
# so each config owns its own files. A shared output path would ping-pong
# between the Release/Debug graphs (the command embeds the config-specific
# protoc path) and force a regen + recompile on every config switch.
# ---------------------------------------------------------------------------
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
file(RELATIVE_PATH PROTO_REL_GEN_BASE "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_REL_GEN_BASE}/$<CONFIG>"
        "-I."
        "steam_messages.proto"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf C++ sources"
    VERBATIM
)

# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns the OS-specific backends.
# Its build is defined in its own module CMakeLists so this top-level file stays
# focused on host-level products (the DLLs) and code generation.
# ---------------------------------------------------------------------------
add_subdirectory(OSTPlatform)

# ---------------------------------------------------------------------------
# Windows host artifacts: the injected hook DLL and the two loader-hijack DLLs.
# All Windows-only by construction (PE DLL hijacking + Win32 hooking).
# ---------------------------------------------------------------------------

# SteamDaddy — the hook DLL injected into Steam (always 64-bit).
add_library(SteamDaddy SHARED
    dllmain.cpp
    OpenSteamTool.rc

    # Shared utilities
    Utils/Tickets/AppTicket.cpp
    Utils/Tickets/EticketClient.cpp
    Utils/Tickets/LegacyCDKey.cpp
    Utils/Config/Config.cpp
    Utils/Config/ConfigFileWatcher.cpp
    Utils/Config/LuaConfig.cpp
    Utils/Config/LuaFileWatcher.cpp
    Utils/CloudRedirect/CloudRedirectHost.cpp
    Utils/HookSupport/VehCommon.cpp
    Utils/Logging/Log.cpp
    Utils/SteamMetadata/IPCLoader.cpp
    Utils/SteamMetadata/ManifestClient.cpp
    Utils/SteamMetadata/PatternLoader.cpp
    Utils/SteamMetadata/RemoteToml.cpp
    Utils/SteamMetadata/StatsClient.cpp
    Utils/SteamMetadata/SteamDiagnostics.cpp

    # Pipe subsystem: generic handshake registry + ProcessInspector core, plus
    # self-contained features that own their primitives.
    Pipe/PipeManager.cpp
    Pipe/ProcessInspector.cpp
    Pipe/Features/DenuvoAuth/DenuvoAuth.cpp
    Pipe/Features/DenuvoAuth/ProtectionScan.cpp
    Pipe/Features/DenuvoAuth/DenuvoSync.cpp
    Pipe/Features/Injection/Injection.cpp

    # Generated IPC message structs (depends on IPCMessages.steamd via add_custom_command)
    "${IPC_GEN}"

    # Per-category hook modules
    Hook/HookManager.cpp
    Hook/Hooks_CallBack.cpp
    Hook/Hooks_KeyValues.cpp
    Hook/Hooks_Decryption.cpp
    Hook/Hooks_Diversion.cpp
    Hook/Hooks_IPC.cpp
    Hook/Hooks_IPC_ISteamUser.cpp
    Hook/Hooks_IPC_ISteamUtils.cpp
    Hook/Hooks_Manifest.cpp
    Hook/Hooks_Misc.cpp
    Hook/Hooks_NetPacket.cpp
    Hook/PendingAPICalls.cpp
    Hook/Hooks_SteamUI.cpp
    Hook/Hooks_Package.cpp

    # protobuf generated sources (full in Debug, lite in Release)
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
)

# Header search path — per-config include directory
target_include_directories(SteamDaddy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)

target_link_libraries(SteamDaddy PRIVATE
    OSTPlatform
    lua_static
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
)

# Logging is compiled in only for Debug; Release reduces LOG_* to no-ops.
target_compile_definitions(SteamDaddy PRIVATE
    $<$<CONFIG:Debug>:STEAMDADDY_LOGGING_ENABLED>
)

# Backend endpoint for on-demand eticket minting (strict Denuvo titles that bind
# their encrypted app ticket to a launch nonce). Empty by default: the feature is
# off and the DLL never makes a network request. Point your own build at your own
# backend and the resulting DLL is self-contained, no Lua config needed:
#
#   cmake -B build -DOST_ETICKET_URL="https://your-host/eticket"
#
# Left out of the source deliberately so no single deployment's backend ships
# baked into a public tree. seteticketurl() in the Lua config overrides it.
set(OST_ETICKET_URL "" CACHE STRING
    "Backend URL for on-demand eticket minting (empty disables the feature)")
if(OST_ETICKET_URL)
    target_compile_definitions(SteamDaddy PRIVATE
        OST_ETICKET_URL="${OST_ETICKET_URL}")
endif()


if(TARGET extract_tickets)
    add_dependencies(SteamDaddy extract_tickets)
endif()
if(TARGET ost-manager)
    add_dependencies(ost-manager SteamDaddy)
    add_custom_target(ost-manager-all ALL DEPENDS ost-manager)
endif()
if(TARGET version)
    add_dependencies(SteamDaddy version)
endif()
if(TARGET dwmapi)
    add_dependencies(SteamDaddy dwmapi)
endif()


