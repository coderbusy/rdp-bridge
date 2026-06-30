cmake_minimum_required(VERSION 3.22)
project(RdpBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RDPBRIDGE_BUILD_EXAMPLES "Build usage examples" ON)

# Auto-enable static linking when a *-static vcpkg triplet is active.
if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(FREERDP_STATIC ON)
endif()
option(FREERDP_STATIC "Link FreeRDP/WinPR/OpenSSL statically into RdpBridge" ${FREERDP_STATIC})

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(FreeRDP REQUIRED CONFIG)
find_package(WinPR REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)
# cjson is a FreeRDP dependency that vcpkg doesn't propagate transitively
find_package(cJSON QUIET CONFIG)
if(NOT cJSON_FOUND)
    find_library(CJSON_LIB NAMES cjson)
endif()

find_path(FREERDP_INCLUDE_DIR NAMES freerdp/freerdp.h PATH_SUFFIXES freerdp3)
find_path(WINPR_INCLUDE_DIR   NAMES winpr/stream.h   PATH_SUFFIXES winpr3)

# ---------------------------------------------------------------------------
# RdpBridge shared library
# ---------------------------------------------------------------------------
add_library(RdpBridge SHARED
    src/RdpBridge/rdp_bridge.cpp
    include/rdp_bridge.h
)

target_include_directories(RdpBridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${FREERDP_INCLUDE_DIR}
        ${WINPR_INCLUDE_DIR}
)

# On Linux, wrap static archives so all symbols are pulled into the .so.
# On Windows (vcpkg static triplet) and for shared linking, plain linking suffices.
if(UNIX AND NOT APPLE AND FREERDP_STATIC)
    target_link_libraries(RdpBridge PRIVATE
        -Wl,--whole-archive
        freerdp
        winpr
        OpenSSL::Crypto
        -Wl,--no-whole-archive
    )
else()
    target_link_libraries(RdpBridge PRIVATE
        freerdp
        winpr
        OpenSSL::Crypto
    )
endif()

# Link cjson if vcpkg exposes it as a separate target (not propagated by FreeRDP)
if(TARGET cjson)
    target_link_libraries(RdpBridge PRIVATE cjson)
elseif(CJSON_LIB)
    target_link_libraries(RdpBridge PRIVATE "${CJSON_LIB}")
endif()

target_compile_definitions(RdpBridge PRIVATE RDP_BRIDGE_EXPORTS)

# Platform-specific library naming and RPATH
if(WIN32)
    set_target_properties(RdpBridge PROPERTIES PREFIX "")
    set_target_properties(RdpBridge PROPERTIES OUTPUT_NAME "RdpBridge")
elseif(APPLE)
    set_target_properties(RdpBridge PROPERTIES
        INSTALL_RPATH "@loader_path"
        BUILD_WITH_INSTALL_RPATH TRUE
        OUTPUT_NAME "RdpBridge"
    )
else()
    set_target_properties(RdpBridge PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
        OUTPUT_NAME "RdpBridge"
    )
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS RdpBridge
    EXPORT RdpBridgeTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/rdp_bridge.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT RdpBridgeTargets
    FILE RdpBridgeTargets.cmake
    NAMESPACE RdpBridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RdpBridge
)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(RDPBRIDGE_BUILD_EXAMPLES)
    add_subdirectory(examples/minimal)
endif()
