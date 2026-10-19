cmake_minimum_required(VERSION 3.20)
project(OpenSteamToolTools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

add_executable(ipc_codegen
    ipc_codegen/ipc_codegen.cpp
)
target_compile_features(ipc_codegen PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ipc_codegen PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(ipc_codegen PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(ipc_codegen PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)


add_executable(extract_tickets
    extract_tickets/main.cpp
    extract_tickets/Utils.cpp
    extract_tickets/VdfParser.cpp
    extract_tickets/AppInfoParser.cpp
    extract_tickets/SteamSession.cpp
    extract_tickets/OutputWriter.cpp
    extract_tickets/Log.cpp
    extract_tickets/Crypto.cpp
    extract_tickets/TokenStorage.cpp
    extract_tickets/WinHttpTransport.cpp
    extract_tickets/JsonHelper.cpp
    extract_tickets/SteamAuthService.cpp
    extract_tickets/SteamCmClient.cpp
    extract_tickets/GameListManager.cpp
    extract_tickets/OnlineSession.cpp
    extract_tickets/TuiEngine.cpp
    extract_tickets/I18n.cpp
    extract_tickets/I18n.h
    extract_tickets/tinflate.c
    extract_tickets/tinfgzip.c
    extract_tickets/crc32.c
)
target_compile_features(extract_tickets PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(extract_tickets PRIVATE /utf-8 /Zc:preprocessor /W4 /permissive- /wd4189)
    set_source_files_properties(
        extract_tickets/tinflate.c
        extract_tickets/tinfgzip.c
        extract_tickets/crc32.c
        PROPERTIES COMPILE_OPTIONS "/W3"
    )
    set_source_files_properties(
            PROPERTIES COMPILE_OPTIONS "/wd4456"
    )
endif()
target_link_libraries(extract_tickets PRIVATE
    advapi32
    winhttp
    crypt32
    bcrypt
    user32
)
target_compile_definitions(extract_tickets PRIVATE
    $<$<CONFIG:Debug>:EXTRACT_TICKETS_LOGGING_ENABLED>
)
set_target_properties(extract_tickets PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)


if(WIN32)
    add_executable(ost-loader
        ost-loader/main.cpp
        ost-loader/ost-loader.rc
        ost-loader/ost-loader.manifest
    )
    target_compile_features(ost-loader PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ost-loader PRIVATE /utf-8 /W4 /permissive-)
    endif()
    # Output ost-loader ONLY to an internal directory so it NEVER lands in shipped Release/Debug folders
    set_target_properties(ost-loader PROPERTIES
        OUTPUT_NAME "ost-loader"
        WIN32_EXECUTABLE TRUE
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/internal/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/internal/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/internal/Debug"
    )
    target_link_libraries(ost-loader PRIVATE
        kernel32
        advapi32
    )
    if(MSVC)
        target_compile_options(ost-loader PRIVATE
            /utf-8 /W4 /permissive-
            $<$<CONFIG:Release>:/O1>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/GR->
        )
        target_link_options(ost-loader PRIVATE
            "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
            "/DYNAMICBASE"
            "/NXCOMPAT"
            "/HIGHENTROPYVA"
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
        )
    endif()

    # Generate embedded C++ headers containing compressed binaries
    set(EMBEDDED_LOADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedLoader.h")
    add_custom_command(
        OUTPUT "${EMBEDDED_LOADER_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DIN_FILE="$<TARGET_FILE:ost-loader>"
            -DOUT_FILE="${EMBEDDED_LOADER_HEADER}"
            -DVAR_NAME="kEmbeddedOstLoader"
            -DCOMPRESS=TRUE
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS ost-loader "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compress_file.ps1"
        COMMENT "Embedding compressed ost-loader.exe binary into EmbeddedLoader.h"
        VERBATIM
    )

    set(EMBEDDED_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedVersion.h")
    add_custom_command(
        OUTPUT "${EMBEDDED_VERSION_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DIN_FILE="$<TARGET_FILE:version>"
            -DOUT_FILE="${EMBEDDED_VERSION_HEADER}"
            -DVAR_NAME="kEmbeddedVersionDll"
            -DCOMPRESS=TRUE
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS version "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compress_file.ps1"
        COMMENT "Embedding compressed version.dll binary into EmbeddedVersion.h"
        VERBATIM
    )

    set(EMBEDDED_DWMAPI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedDwmapi.h")
    add_custom_command(
        OUTPUT "${EMBEDDED_DWMAPI_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DIN_FILE="$<TARGET_FILE:dwmapi>"
            -DOUT_FILE="${EMBEDDED_DWMAPI_HEADER}"
            -DVAR_NAME="kEmbeddedDwmapiDll"
            -DCOMPRESS=TRUE
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS dwmapi "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compress_file.ps1"
        COMMENT "Embedding compressed dwmapi.dll binary into EmbeddedDwmapi.h"
        VERBATIM
    )

    set(EMBEDDED_OPENSTEAMTOOL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedOpenSteamTool.h")
    add_custom_command(
        OUTPUT "${EMBEDDED_OPENSTEAMTOOL_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DIN_FILE="$<TARGET_FILE:SteamDaddy>"
            -DOUT_FILE="${EMBEDDED_OPENSTEAMTOOL_HEADER}"
            -DVAR_NAME="kEmbeddedOpenSteamToolDll"
            -DCOMPRESS=TRUE
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS SteamDaddy "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compress_file.ps1"
        COMMENT "Embedding compressed OpenSteamTool.dll binary into EmbeddedOpenSteamTool.h"
        VERBATIM
    )

    set_source_files_properties(ost-manager/IfeoEngine.cpp PROPERTIES
        OBJECT_DEPENDS "${EMBEDDED_LOADER_HEADER};${EMBEDDED_VERSION_HEADER};${EMBEDDED_DWMAPI_HEADER};${EMBEDDED_OPENSTEAMTOOL_HEADER}"
    )

    add_executable(ost-manager
        ost-manager/main.cpp
        ost-manager/IfeoEngine.cpp
        ost-manager/IfeoEngine.h
        ost-manager/InjectorEngine.cpp
        ost-manager/InjectorEngine.h
        ost-manager/I18n.h
        ost-manager/ost-manager.rc
        ost-manager/ost-manager.manifest
        extract_tickets/tinflate.c
        extract_tickets/tinfgzip.c
        extract_tickets/crc32.c
        "${EMBEDDED_LOADER_HEADER}"
        "${EMBEDDED_VERSION_HEADER}"
        "${EMBEDDED_DWMAPI_HEADER}"
        "${EMBEDDED_OPENSTEAMTOOL_HEADER}"
    )
    target_include_directories(ost-manager PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/extract_tickets"
    )
    target_compile_features(ost-manager PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ost-manager PRIVATE
            /utf-8 /W4 /permissive-
            $<$<CONFIG:Release>:/O1>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/GR->
        )
        target_link_options(ost-manager PRIVATE
            "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
            "/DYNAMICBASE"
            "/NXCOMPAT"
            "/HIGHENTROPYVA"
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
        )
        set_source_files_properties(
            extract_tickets/tinflate.c
            extract_tickets/tinfgzip.c
            extract_tickets/crc32.c
            PROPERTIES COMPILE_OPTIONS "/W3"
        )
    endif()
    set_target_properties(ost-manager PROPERTIES
        OUTPUT_NAME "ost-manager"
        WIN32_EXECUTABLE TRUE
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    )
    target_link_libraries(ost-manager PRIVATE
        kernel32
        user32
        advapi32
        shell32
        comctl32
    )
    add_dependencies(ost-manager ost-loader version dwmapi SteamDaddy)
endif()

