cmake_minimum_required(VERSION 3.22)
project(RdpBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RDPBRIDGE_BUILD_EXAMPLES "Build usage examples" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(FreeRDP REQUIRED CONFIG)
find_package(WinPR REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)

find_path(FREERDP_INCLUDE_DIR NAMES freerdp/freerdp.h PATH_SUFFIXES freerdp3)

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
)

target_link_libraries(RdpBridge
    PRIVATE
        freerdp
        winpr
        OpenSSL::Crypto
)

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
