cmake_minimum_required(VERSION 3.20)
project(MarketDataService CXX)

# Standalone CTP market-data service extracted from the trading engine.
# It subscribes the whole futures market (the instrument list is fetched from
# the openctp dict HTTP service, with a local cache fallback), pushes every
# OnRtnDepthMarketData tick into a configurable backend (ClickHouse HTTP or
# local CSV) and fans each tick out over TCP unicast and UDP multicast.
#
# Third-party dependencies are vendored under third_party/ (the Asio and CTP
# archives are unpacked into the build tree at configure time, so a fresh
# checkout configures without any download and without generated files in the
# tree):
#   * CTP market data API  : thostmduserapi_se (headers + import library)
#   * standalone Asio      : third_party/asio-asio-1-30-2.zip (header-only,
#                            ASIO_STANDALONE) - no Boost required
#   * cJSON                : vendored under third_party (single .h/.cpp)
#
# Usage (Windows / Visual Studio):
#   cmake -S . -B build
#   cmake --build build --config Release
#
# Usage (Linux):
#   cmake -S . -B build
#   cmake --build build -j
#
# Use -DCTP_API_DIR=<dir with ThostFtdcMdApi.h + thostmduserapi_se.lib/.a>
# to build against an already extracted CTP SDK instead of the vendored one.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# consumed by clang-tidy / clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug/Release)" FORCE)
endif()

# ------------------------------------------------------------------
# Dependencies
#
# file(ARCHIVE_EXTRACT) needs CMake >= 3.18; keep the extracted copies in
# the build tree so `rm -rf build` always resets them.
# ------------------------------------------------------------------
set(MD_DEP_DIR "${CMAKE_BINARY_DIR}/deps")

# --- standalone Asio (header-only) ---------------------------------
set(ASIO_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio")
set(ASIO_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-asio-1-30-2.zip")

if(EXISTS "${ASIO_VENDOR_DIR}/asio.hpp")
    set(ASIO_INCLUDE_DIR "${ASIO_VENDOR_DIR}")
elseif(EXISTS "${ASIO_ARCHIVE}")
    set(ASIO_INCLUDE_DIR "${MD_DEP_DIR}/asio/include")
    if(NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
        message(STATUS "extracting ${ASIO_ARCHIVE}")
        file(ARCHIVE_EXTRACT INPUT "${ASIO_ARCHIVE}" DESTINATION "${MD_DEP_DIR}/asio")
        file(GLOB_RECURSE _asio_hpp "${MD_DEP_DIR}/asio/asio.hpp")
        if(NOT _asio_hpp)
            message(FATAL_ERROR "asio.hpp not found after extracting ${ASIO_ARCHIVE}")
        endif()
        # .../<archive-root>/asio/include/asio.hpp
        get_filename_component(ASIO_INCLUDE_DIR "${_asio_hpp}" DIRECTORY)
    endif()
else()
    message(FATAL_ERROR
        "standalone Asio not found: neither ${ASIO_VENDOR_DIR}/asio.hpp nor ${ASIO_ARCHIVE}.")
endif()

# --- CTP market data API -------------------------------------------
set(CTP_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/v6.7.10_20250422_traderapi.zip")
set(CTP_API_DIR "" CACHE PATH "CTP API directory (headers + import libraries); empty = use the vendored archive")

if(NOT CTP_API_DIR)
    if(EXISTS "${CTP_ARCHIVE}")
        set(_ctp_root "${MD_DEP_DIR}/ctp")
        if(NOT EXISTS "${_ctp_root}/ThostFtdcMdApi.h")
            message(STATUS "extracting ${CTP_ARCHIVE}")
            file(ARCHIVE_EXTRACT INPUT "${CTP_ARCHIVE}" DESTINATION "${_ctp_root}")
        endif()

        # the archive bundles every platform; pick the mduser package that
        # matches this build (64-bit Windows / 64-bit Linux, both with the
        # trading API's mduser part)
        if(WIN32)
            set(_ctp_pattern "traderapi64_se_windows")
        else()
            set(_ctp_pattern "traderapi_se_linux64")
        endif()
        file(GLOB_RECURSE _ctp_headers "${_ctp_root}/ThostFtdcMdApi.h")
        foreach(_hdr ${_ctp_headers})
            get_filename_component(_dir "${_hdr}" DIRECTORY)
            if(NOT CTP_API_DIR)
                set(CTP_API_DIR "${_dir}")
            endif()
            if(_hdr MATCHES "${_ctp_pattern}")
                set(CTP_API_DIR "${_dir}")
            endif()
        endforeach()
        unset(_ctp_headers)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tradeapidll")
        # legacy layout: the SDK next to the project directory
        set(CTP_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tradeapidll")
    endif()
endif()

if(NOT EXISTS "${CTP_API_DIR}/ThostFtdcMdApi.h")
    message(FATAL_ERROR
        "CTP market-data API headers not found (looked for ${CTP_API_DIR}/ThostFtdcMdApi.h).\n"
        "Provide the SDK either by extracting third_party/v6.7.10_20250422_traderapi.zip "
        "or with -DCTP_API_DIR=<dir containing ThostFtdcMdApi.h>.")
endif()
message(STATUS "CTP_API_DIR: ${CTP_API_DIR}")
message(STATUS "ASIO_INCLUDE_DIR: ${ASIO_INCLUDE_DIR}")

# ------------------------------------------------------------------
# Target
# ------------------------------------------------------------------
file(GLOB MD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(APPEND MD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON.cpp")

add_executable(MarketDataService ${MD_SOURCES})

target_include_directories(MarketDataService PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${ASIO_INCLUDE_DIR}"
    "${CTP_API_DIR}"
)
target_link_directories(MarketDataService PRIVATE "${CTP_API_DIR}")

# standalone Asio: no Boost dependency
target_compile_definitions(MarketDataService PRIVATE ASIO_STANDALONE)

# Sources are UTF-8 encoded (some comments are Chinese). MSVC assumes the
# local ANSI code page unless told otherwise, which would garble or even break
# the parse (C4819 / C2447), so pin the source and execution charset.
# C4828 is silenced on purpose: the vendored CTP headers are GBK-encoded, so
# every Chinese character in them would be reported as an illegal UTF-8
# sequence (thousands of lines of pure noise per build).
if(MSVC)
    target_compile_options(MarketDataService PRIVATE /utf-8 /wd4828)
endif()

if(WIN32)
    # Winsock system libraries (required by standalone Asio on Windows)
    target_compile_definitions(MarketDataService PRIVATE
        WIN32
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0501
        NOMINMAX
    )
    target_link_libraries(MarketDataService PRIVATE thostmduserapi_se ws2_32 mswsock)

    # copy the CTP runtime DLL next to the executable so it can start from the build dir
    foreach(_dll thostmduserapi_se.dll)
        add_custom_command(TARGET MarketDataService POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CTP_API_DIR}/${_dll}" "$<TARGET_FILE_DIR:MarketDataService>"
        )
    endforeach()
else()
    target_compile_definitions(MarketDataService PRIVATE linux)

    # The CTP Linux SDK ships "thostmduserapi_se.so": no "lib" prefix and no
    # SONAME, so -lthostmduserapi_se cannot resolve. Give the linker a
    # lib-prefixed symlink, and copy the real object under that name next to
    # the executable so the DT_NEEDED entry also resolves at run time.
    set(CTP_MD_LIBRARY "${CTP_API_DIR}/thostmduserapi_se.so")
    if(NOT EXISTS "${CTP_MD_LIBRARY}")
        message(FATAL_ERROR
            "CTP mduser shared library not found (looked for ${CTP_MD_LIBRARY}).\n"
            "Extract third_party/v6.7.10_20250422_traderapi.zip or configure with "
            "-DCTP_API_DIR=<dir containing thostmduserapi_se.so>.")
    endif()

    set(CTP_MD_LINK_DIR "${MD_DEP_DIR}/lib")
    if(NOT EXISTS "${CTP_MD_LINK_DIR}/libthostmduserapi_se.so")
        file(MAKE_DIRECTORY "${CTP_MD_LINK_DIR}")
        file(CREATE_LINK "${CTP_MD_LIBRARY}"
             "${CTP_MD_LINK_DIR}/libthostmduserapi_se.so" SYMBOLIC)
    endif()

    target_link_directories(MarketDataService PRIVATE "${CTP_MD_LINK_DIR}")
    target_link_libraries(MarketDataService PRIVATE thostmduserapi_se pthread)
    set_target_properties(MarketDataService PROPERTIES
        BUILD_RPATH "${CTP_API_DIR};${CTP_MD_LINK_DIR};\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
    )

    # keep the CTP runtime next to the executable (same intent as the Windows
    # DLL copy above), under the name recorded in DT_NEEDED
    add_custom_command(TARGET MarketDataService POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_MD_LIBRARY}"
            "$<TARGET_FILE_DIR:MarketDataService>/libthostmduserapi_se.so"
    )
endif()
