cmake_minimum_required(VERSION 3.22)
project(RdpBridgeMinimalExample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When built standalone (outside the SDK tree) find the installed package.
# When built as part of the top-level CMakeLists.txt the target already exists.
if(NOT TARGET RdpBridge)
    find_package(RdpBridge REQUIRED CONFIG)
    set(RDPBRIDGE_TARGET RdpBridge::RdpBridge)
else()
    set(RDPBRIDGE_TARGET RdpBridge)
endif()

add_executable(rdp_example main.c)
target_link_libraries(rdp_example PRIVATE ${RDPBRIDGE_TARGET})
