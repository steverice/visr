# Build the iPhone/iPad (or Apple TV, or Apple Vision Pro) host and SDL around the statically compiled guest image.
cmake_minimum_required(VERSION 3.24)
project(HaloCE LANGUAGES C CXX OBJC ASM)
# theater mode's immersive space is SwiftUI (host/Theater.swift)
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    enable_language(Swift)
endif()
set(HALO_BUNDLE_IDENTIFIER "org.haloce.ios" CACHE STRING "App bundle identifier")
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/build/third_party/SDL3" sdl3 EXCLUDE_FROM_ALL)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # SDL's CMake forces SDL_OPENGLES off for tvOS, but its UIKit GL ES code is
    # shared with iOS and tvOS still ships OpenGLES.framework (SDL's own Xcode
    # config, SDL_build_config_ios.h, enables it). The sources are always
    # compiled; only these macros gate them.
    target_compile_definitions(SDL3-static PRIVATE SDL_VIDEO_OPENGL_ES=1 SDL_VIDEO_OPENGL_ES2=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    # SDL's CMake ties its virtual-joystick driver to HIDAPI, which is off on
    # visionOS, but the driver doesn't use HIDAPI. host/host_tv.m's player-one
    # pad is a virtual joystick, so compile it in as tvOS gets it.
    target_sources(SDL3-static PRIVATE "${ROOT}/build/third_party/SDL3/src/joystick/virtual/SDL_virtualjoystick.c")
    target_compile_definitions(SDL3-static PRIVATE SDL_JOYSTICK_VIRTUAL=1)
endif()
# iPhone and visionOS: each build a new CFBundleVersion (tools/ios_build.py), so
# installing over the app replaces it and keeps its Documents (the imported game)
set(HALO_BUILD_NUMBER 6 CACHE STRING "CFBundleVersion")
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # Controllers only, no Files app: the XISO arrives from a browser
    # (host/host_tv_import.m).
    set(HALO_TVOS ON)
    # 4K fill rate is the bottleneck in heavy fights on an Apple TV 4K (3rd gen);
    # there's no Files app to edit config.toml, so the build picks it (0 = native).
    set(HALO_RENDER_HEIGHT 1080 CACHE STRING "Internal render height (display.render_height)")
    # Brand assets (layered icon, top shelf) made from the iOS AppIcon artwork.
    set(HALO_PLATFORM_SOURCES Assets-tvOS.xcassets host/host_tv.m host/host_tv_import.m)
    set(HALO_INFO_PLIST Info-tvOS.plist)
    set(HALO_DEVICE_FAMILY 3)
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    # A window, played with a game controller (host/host_tv.m's input), the
    # game imported through Files or the Documents folder (host/host_import.m).
    # visionOS has no OpenGL ES: Metal is the only renderer.
    set(HALO_VISIONOS ON)
    # native (0): the window's drawable, about 2560x1440 at its default size,
    # following the window when it's resized (d3d8_device.c's halo_screen_commit)
    set(HALO_RENDER_HEIGHT 0 CACHE STRING "Internal render height (display.render_height)")
    option(HALO_EXTENDED_VIRTUAL_ADDRESSING "Sign with the extended virtual addressing entitlement" OFF)
    set(HALO_PLATFORM_SOURCES Assets-visionOS.xcassets host/host_tv.m host/host_import.m host/host_orientation.m
        host/host_theater.m host/host_config.c host/Theater.swift host/host_texture_settings.m host/texture_refine.m)
    set(HALO_INFO_PLIST Info-visionOS.plist)
    set(HALO_DEVICE_FAMILY 7)
else()
    set(HALO_PLATFORM_SOURCES Assets.xcassets host/host_import.m host/host_orientation.m host/host_touch.m
        host/host_texture_settings.m host/host_config.c host/texture_refine.m)
    set(HALO_INFO_PLIST Info.plist)
    set(HALO_DEVICE_FAMILY "1,2")
endif()
# the texture upscale policy (port/assets/texture-policy.json) as a C table for port/runtime/texture_policy.c
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    COMMAND Python3::Interpreter "${ROOT}/tools/embed_texture_policy.py"
            "${ROOT}/port/assets/texture-policy.json" "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    DEPENDS "${ROOT}/tools/embed_texture_policy.py" "${ROOT}/port/assets/texture-policy.json"
    COMMENT "Embedding the texture upscale policy")
add_executable(HaloCE MACOSX_BUNDLE
    ${HALO_PLATFORM_SOURCES} "${ROOT}/port/runtime/xiso.c"
    "${ROOT}/port/runtime/texture_policy.c" "${ROOT}/port/runtime/texture_cache.c"
    "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    host/host_main.m host/host_memory.c host/host_loader.c host/host_syscall.c host/guest_call.S
    host/host_thread.c host/host_sdl.c host/host_gpu.c host/host_gpu_dispatch.c host/gpu_metal.m host/host_frame_counter.m host/host_lifecycle.m
    "${ROOT}/port/linux/src/posix_files.c" host/posix_net.c
    "${ROOT}/build/ios/host/bridges.c" "${ROOT}/build/ios/embedded/guest_image.S")
target_include_directories(HaloCE PRIVATE host "${ROOT}/port/runtime" "${ROOT}/port/runtime/include"
    "${ROOT}/port/runtime/guest/runtime" "${ROOT}/port/linux/src" "${ROOT}/build/ios/host"
    "${ROOT}/build/ios/embedded")
target_compile_definitions(HaloCE PRIVATE HALO_IOS=1 st_mtim=st_mtimespec st_atim=st_atimespec st_ctim=st_ctimespec)
target_compile_options(HaloCE PRIVATE "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-deprecated-declarations>")
if(NOT HALO_VISIONOS)
    # the GL backend (step 4): OpenGL ES through gl.h's pointer table, resolved by name
    target_sources(HaloCE PRIVATE "${ROOT}/port/linux/src/gpu_gl.c" "${ROOT}/port/linux/src/gl_functions.c")
    target_include_directories(HaloCE PRIVATE "${ROOT}/build/ios/gl_include")
    set_source_files_properties("${ROOT}/port/linux/src/gpu_gl.c" "${ROOT}/port/linux/src/gl_functions.c"
        PROPERTIES COMPILE_DEFINITIONS GPU_GL_HOST=1)
    target_link_libraries(HaloCE PRIVATE "-framework OpenGLES")
endif()
target_compile_options(HaloCE PRIVATE "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
target_link_libraries(HaloCE PRIVATE SDL3::SDL3-static "-framework Foundation" "-framework UIKit"
    "-framework Metal" "-framework QuartzCore" z)
# display.upscaler = "metalfx" (gpu_metal.m); the simulators have no MetalFX
if(NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    target_link_libraries(HaloCE PRIVATE "-weak_framework MetalFX")
endif()
if(HALO_TVOS)
    target_link_libraries(HaloCE PRIVATE "-framework Network" "-framework CoreImage")
    set_source_files_properties(Assets-tvOS.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(HALO_VISIONOS)
    target_link_libraries(HaloCE PRIVATE "-framework UniformTypeIdentifiers" "-framework CompositorServices"
        "-framework ARKit" "-framework SwiftUI")
    set_source_files_properties(Assets-visionOS.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # the app's page in the Settings app (host/host_texture_settings.m)
    target_sources(HaloCE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Theater.swift calls host_theater.m's C functions
    set_target_properties(HaloCE PROPERTIES
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/host/host_theater.h"
        XCODE_ATTRIBUTE_SWIFT_VERSION 5.0)
    # the guest pool can grow to about 1.9 GB; extended virtual addressing only
    # if a device refuses the 4 GB arena (tools/ios_build.py)
    if(HALO_EXTENDED_VIRTUAL_ADDRESSING)
        set(HALO_ENTITLEMENTS HaloCE-visionOS-extended.entitlements)
    else()
        set(HALO_ENTITLEMENTS HaloCE-visionOS.entitlements)
    endif()
    set_target_properties(HaloCE PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_ENTITLEMENTS}")
else()
    target_link_libraries(HaloCE PRIVATE "-framework UniformTypeIdentifiers")
    set_source_files_properties(Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # the app's page in the Settings app (host/host_texture_settings.m)
    target_sources(HaloCE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
set_target_properties(HaloCE PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
file(GLOB HALO_LICENSES "${ROOT}/build/ios/licenses/*.txt")
target_sources(HaloCE PRIVATE ${HALO_LICENSES})
set_source_files_properties(${HALO_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
set_target_properties(HaloCE PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_INFO_PLIST}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HALO_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${HALO_DEVICE_FAMILY}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_HALO_RENDER_HEIGHT "${HALO_RENDER_HEIGHT}"
    XCODE_ATTRIBUTE_HALO_BUILD_NUMBER "${HALO_BUILD_NUMBER}")
if(HALO_DEVELOPMENT_TEAM)
    set_target_properties(HaloCE PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${HALO_DEVELOPMENT_TEAM}")
endif()
