cmake_minimum_required(VERSION 3.24)
project(OpenCETV LANGUAGES C OBJC)
set(CMAKE_C_STANDARD 11)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(APPLE_HOST "${ROOT}/port/macos/host")
set(PLATFORM "${ROOT}/port/linux/src")
set(THIRD_PARTY "${ROOT}/port/third_party")

# Guest code has no native Apple ABI: reuse wasm2c's C, then compile every
# object for this SDK. Never link a Mac .o or libSDL3.a into the TV app.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPENCE_SDL_SOURCE}" sdl)
file(GLOB TRANSLATED CONFIGURE_DEPENDS "${OPENCE_W2C}/halo_*.c")
file(GLOB HOST_C CONFIGURE_DEPENDS "${APPLE_HOST}/*.c")
file(GLOB HOST_M CONFIGURE_DEPENDS "${APPLE_HOST}/*.m")
file(GLOB MBED_C CONFIGURE_DEPENDS "${THIRD_PARTY}/mbedtls/library/*.c")
# Match the native dependencies in tools/macos_build.py; no game-side
# source enumeration or guest ABI configuration is duplicated here.
file(GLOB UPNP_C CONFIGURE_DEPENDS "${THIRD_PARTY}/miniupnpc/src/*.c")
add_executable(OpenCETV MACOSX_BUNDLE
    "${ROOT}/port/tvos/tv_main.m" ${HOST_C} ${HOST_M} ${TRANSLATED}
    "${OPENCE_GENERATED}/host_imports.c"
    "${OPENCE_WASM_RUNTIME}/wasm-rt-impl.c" "${OPENCE_WASM_RUNTIME}/wasm-rt-mem-impl.c"
    "${PLATFORM}/posix_files.c" "${PLATFORM}/posix_net.c" "${PLATFORM}/posix_dtls.c" "${PLATFORM}/posix_upnp.c"
    ${MBED_C} ${UPNP_C})
target_compile_definitions(OpenCETV PRIVATE HALO_TVOS=1
    WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1 WASM_RT_TRAP_HANDLER=host_trap
    WASM_RT_MEMCHECK_BOUNDS_CHECK=1 WASM_RT_MEMCHECK_GUARD_PAGES=0
    _GNU_SOURCE MINIUPNP_STATICLIB MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR)
target_compile_options(OpenCETV PRIVATE -O2 -g -ffp-contract=off
    "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
set_source_files_properties(${TRANSLATED} PROPERTIES COMPILE_OPTIONS "-O3;-w")
set_source_files_properties(${MBED_C} ${UPNP_C} PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(OpenCETV PRIVATE "${APPLE_HOST}" "${ROOT}/port/macos/guest"
    "${PLATFORM}" "${OPENCE_W2C}" "${OPENCE_WASM_RUNTIME}" "${OPENCE_SIMDE}"
    "${THIRD_PARTY}/mbedtls/include" "${THIRD_PARTY}/mbedtls/library"
    "${THIRD_PARTY}/miniupnpc/include" "${THIRD_PARTY}/miniupnpc/src")
target_link_libraries(OpenCETV PRIVATE SDL3::SDL3-static iconv
    "-framework Foundation" "-framework UIKit" "-framework Metal" "-framework QuartzCore"
    "-framework GameController" "-framework CoreAudio" "-framework AudioToolbox"
    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework CoreGraphics"
    "-framework CoreHaptics" "-framework UniformTypeIdentifiers")
set_target_properties(OpenCETV PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${ROOT}/port/tvos/Info.plist")
