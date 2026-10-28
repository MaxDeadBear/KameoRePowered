# Kameo: RePowered - ReXGlue recompilation of Kameo: Elements of Power.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(kameorepowered LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KAMEO_X86_64_V3 "Build for x86-64-v3 (AVX2, Haswell and newer)" ON)

include(generated/rexglue.cmake)

if(NOT REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    message(FATAL_ERROR "No recompiled code. Run codegen first:\n  rexglue codegen kameorepowered_manifest.toml")
endif()

foreach(submodule plume XenosRecomp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${submodule}/CMakeLists.txt")
        message(FATAL_ERROR "thirdparty/${submodule} is missing:\n  git submodule update --init --recursive")
    endif()
endforeach()
add_subdirectory(thirdparty/plume plume EXCLUDE_FROM_ALL)

set(KAMEO_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/res/shader_cache/shader_cache.cpp")
file(READ "${KAMEO_SHADER_CACHE}" _kameo_shader_cache_head LIMIT 64)
if(_kameo_shader_cache_head MATCHES "^version https://git-lfs")
    message(FATAL_ERROR "res/shader_cache/shader_cache.cpp is a Git LFS pointer:\n  git lfs pull")
endif()

file(GLOB_RECURSE KAMEO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER KAMEO_SOURCES EXCLUDE REGEX "/src/launcher/")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX src FILES ${KAMEO_SOURCES})
list(APPEND KAMEO_SOURCES
    "${KAMEO_SHADER_CACHE}"
    # XenosRecomp smol-v encodes the cache's SPIR-V, so the Vulkan backend decodes it before use.
    thirdparty/XenosRecomp/thirdparty/smol-v/source/smolv.cpp)

if(WIN32)
    add_executable(kameorepowered WIN32 ${KAMEO_SOURCES} res/windows/kameo.rc)
else()
    add_executable(kameorepowered ${KAMEO_SOURCES})
endif()

target_include_directories(kameorepowered PRIVATE
    src
    res
    thirdparty
    thirdparty/XenosRecomp/thirdparty/xxHash
    thirdparty/XenosRecomp/thirdparty/smol-v/source)

# plume is a static library, so its D3D12 imports are satisfied here; Vulkan is loaded through volk.
target_link_libraries(kameorepowered PRIVATE plume)
if(WIN32)
    # dbghelp writes the crash reporter's minidump.
    target_link_libraries(kameorepowered PRIVATE d3d12 dbghelp mfplat mfreadwrite mfuuid ole32 xaudio2)
elseif(UNIX AND NOT APPLE)
    # plume only builds Xlib surfaces on Linux, and the guest memory-free wrappers find the runtime's exports through dlsym.
    target_link_libraries(kameorepowered PRIVATE X11 ${CMAKE_DL_LIBS})
endif()

# Release keeps a PDB too, so the module+offset frames in a crash report resolve to functions. It stays with whoever built it: CI drops it from the archive and publishes no symbols. RelWithDebInfo has one from CMake's own defaults.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(kameorepowered PRIVATE $<$<CONFIG:Release>:-g -Xclang -gcodeview>)
    target_link_options(kameorepowered PRIVATE $<$<CONFIG:Release>:-g>)
endif()

# The xenos plugin stays staged as the reference renderer behind --kameo_native_renderer=false.
rexglue_setup_target(kameorepowered GPU_PLUGINS xenos)

# The SDK only stages its shared libraries next to the executable on Windows and macOS.
if(UNIX AND NOT APPLE)
    foreach(_kameo_runtime_lib rex::runtime rex::TracyClient)
        if(TARGET ${_kameo_runtime_lib})
            add_custom_command(TARGET kameorepowered POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${_kameo_runtime_lib}> $<TARGET_FILE_DIR:kameorepowered> VERBATIM)
        endif()
    endforeach()
endif()

# Non-Release SDK builds bump a shared atomic on every indirect guest call; the recompiled code opts out.
if(TARGET kameorepowered_recomp)
    target_compile_options(kameorepowered_recomp PRIVATE -UREXGLUE_ENABLE_PERF_COUNTERS)
endif()

# fp-contract stays off so separate PPC multiplies and adds are never fused into FMAs that round differently.
if(KAMEO_X86_64_V3 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(_kameo_target kameorepowered kameorepowered_recomp)
        if(TARGET ${_kameo_target})
            target_compile_options(${_kameo_target} PRIVATE -march=x86-64-v3 -ffp-contract=off)
        endif()
    endforeach()
endif()

# The launcher edits settings, installs DLC, fetches and switches builds and starts the game, borrowing the game's runtime DLL for ImGui and STFS.
if(WIN32)
    file(GLOB_RECURSE KAMEO_LAUNCHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/*.cpp")
    add_executable(KameoLauncher WIN32
        ${KAMEO_LAUNCHER_SOURCES}
        thirdparty/imgui_backends/imgui_impl_win32.cpp
        thirdparty/imgui_backends/imgui_impl_dx11.cpp
        res/windows/kameo.rc)
    target_include_directories(KameoLauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src thirdparty)
    target_link_libraries(KameoLauncher PRIVATE rex::runtime d3d11 dxgi dwmapi d3dcompiler winhttp)
    target_link_options(KameoLauncher PRIVATE "LINKER:/DELAYLOAD:rexruntime.dll")
    target_link_libraries(KameoLauncher PRIVATE delayimp)
    # The launcher runs on its own copy of the runtime DLL in launcher\ (see src/launcher/runtime_loader.cpp), so switching builds can replace the game's.
    add_custom_command(TARGET KameoLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:KameoLauncher>/launcher
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:KameoLauncher>/launcher/
        VERBATIM)
    add_dependencies(kameorepowered KameoLauncher)
endif()
