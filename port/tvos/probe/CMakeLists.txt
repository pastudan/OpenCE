cmake_minimum_required(VERSION 3.24)
project(OpenCETVProbe LANGUAGES C CXX OBJC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only Boost's headers are needed (JSON includes its implementation in
# libtorrent). Avoid installing a host Boost binary in a device build.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
add_subdirectory("${OPENCE_LIBTORRENT_SOURCE}" libtorrent)
if(NOT webtorrent)
    message(FATAL_ERROR "The dual-transport probe requires webtorrent=ON")
endif()

add_library(opence_swarm_test STATIC swarm_test.cpp)
target_link_libraries(opence_swarm_test PUBLIC torrent-rasterbar)
target_include_directories(opence_swarm_test PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(OPENCE_PROBE_PLATFORM STREQUAL "macos")
    add_executable(opence_swarm_probe swarm_main.cpp)
    target_link_libraries(opence_swarm_probe PRIVATE opence_swarm_test)
    enable_testing()
    add_test(NAME loopback_tcp COMMAND opence_swarm_probe)
    set_tests_properties(loopback_tcp PROPERTIES TIMEOUT 60)
else()
    add_executable(OpenCETVProbe MACOSX_BUNDLE platform_probe.m)
    target_compile_options(OpenCETVProbe PRIVATE -fobjc-arc)
    target_link_libraries(OpenCETVProbe PRIVATE opence_swarm_test
        "-framework UIKit" "-framework Metal" "-framework Foundation")
    set_target_properties(OpenCETVProbe PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
endif()
