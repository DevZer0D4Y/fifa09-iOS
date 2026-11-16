# fifa09 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fifa09 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FIFA09_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(fifa09 WIN32 ${FIFA09_SOURCES})
else()
    add_executable(fifa09 ${FIFA09_SOURCES})
endif()

rexglue_setup_target(fifa09 GPU_PLUGINS xenos)

# On-screen football gamepad (src/ios/touch_pad.h): on by default on iOS, --touch_controls=true
# elsewhere. Not on Windows: rexruntime.dll does not export its static SDL3.
if(NOT WIN32)
    find_package(SDL3 REQUIRED CONFIG COMPONENTS Headers)
    target_link_libraries(fifa09 PRIVATE SDL3::Headers)
    target_sources(fifa09 PRIVATE src/ios/touch_pad.cpp)
    target_compile_definitions(fifa09 PRIVATE FIFA09_TOUCH=1)
endif()

# iPhone / iPad (docs/IOS.md): SDK built with patches/rexglue-ios.patch, Vulkan through MoltenVK.
# The guest vblank comes from the screen (src/ios/display_link.mm), so 60 fps lands on every
# display refresh instead of drifting against it.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    target_sources(fifa09 PRIVATE src/ios/display_link.mm)
    set_source_files_properties(src/ios/display_link.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(fifa09 PRIVATE "-framework UIKit" "-framework QuartzCore" "-framework Foundation")
    # rex_wait_display_vblank is looked up with dlsym from the SDK's runtime library; it is
    # marked visibility("default") + used, so the executable exports it.
endif()

# FSR (AMD FidelityFX, SDK built with REXGLUE_ENABLE_FIDELITYFX=ON): rexruntime needs
# amd_fidelityfx_dx12*.dll at run time, but the SDK only links it privately, so the
# TARGET_RUNTIME_DLLS copy misses it. Put it next to fifa09.exe (package.ps1 ships *.dll).
if(WIN32 AND TARGET rex::amd_fidelityfx_dx12)
    add_custom_command(TARGET fifa09 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::amd_fidelityfx_dx12> $<TARGET_FILE_DIR:fifa09>
        VERBATIM)
endif()
