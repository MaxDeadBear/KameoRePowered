# Kameo: RePowered - ReXGlue recompilation of Kameo: Elements of Power.
# The SDK package (find_package(rexglue)) provides rexglue_project and the rexglue_setup_* functions.

cmake_minimum_required(VERSION 3.25)
project(kameorepowered LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KAMEO_X86_64_V3 "Build for x86-64-v3 (AVX2, Haswell and newer)" ON)

# The release tag (v0.3.1-beta.4) goes into the Windows executables' version information; unsigned binaries without any
# are what virus scanners' machine-learning engines flag most often.
set(KAMEO_VERSION "dev" CACHE STRING "Version written into the executables, normally the release tag")
set(KAMEO_VERSION_COMMAS "0,0,0,0")
if(KAMEO_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[A-Za-z]+\\.?([0-9]+))?")
    # The pre-release number (beta.4) is the fourth part.
    set(_kameo_build 0)
    if(CMAKE_MATCH_5)
        set(_kameo_build ${CMAKE_MATCH_5})
    endif()
    set(KAMEO_VERSION_COMMAS "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${_kameo_build}")
endif()
# The title-update flavour, kameorepowered-tu: the game as patched by its title update (Expert mode, Rune Battle, Time
# Attack and the update's fixes), for players who have the update. Its recompiled code comes from the patched image, and
# our sources are compiled translated to the update's addresses (tools/tu/). It needs assets/TU/default.xexp.
option(KAMEO_TITLE_UPDATE "Build kameorepowered-tu, the title-update flavour, from assets/TU/default.xexp" OFF)

# kameorepowered-tu runs only the update it was recompiled from, so its version info names that patch by SHA-1 and the
# launcher refuses any other (src/launcher/title_update.cpp).
set(KAMEO_TU_PATCH_SHA1 "")
if(KAMEO_TITLE_UPDATE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/TU/default.xexp")
    file(SHA1 "${CMAKE_CURRENT_SOURCE_DIR}/assets/TU/default.xexp" KAMEO_TU_PATCH_SHA1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/TU/default.xexp")
endif()
configure_file(res/windows/kameo_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/res/kameo_version.h" @ONLY)

# The ReXGlue SDK: an installed package on CMAKE_PREFIX_PATH (the presets take it from the REXGLUE_SDK environment
# variable), or a source tree in REXSDK_DIR. Keep the version in step with sdk_version in kameorepowered_manifest.toml,
# SDK_VERSION in .github/workflows/_build.yml and tools/tu/prepare.py.
set(REXSDK_DIR "" CACHE PATH "Path to a rexglue-sdk source tree, to build the SDK with the game")
if(REXSDK_DIR)
    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
else()
    find_package(rexglue 0.11.0 REQUIRED CONFIG)
endif()

if(KAMEO_TITLE_UPDATE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/tu/prepare.py RESULT_VARIABLE _kameo_tu_prepared)
    if(_kameo_tu_prepared)
        message(FATAL_ERROR "tools/tu/prepare.py failed: the title-update build needs assets/TU/default.xexp")
    endif()
    # The title-update workspace has its own manifest, game files and codegen output. rexglue_project is a macro that
    # places all three under CMAKE_CURRENT_SOURCE_DIR, so it runs with that pointed at the workspace.
    set(KAMEO_TU_WS "${CMAKE_CURRENT_SOURCE_DIR}/generated/tu")
    set(_kameo_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CMAKE_CURRENT_SOURCE_DIR "${KAMEO_TU_WS}")
    rexglue_project(kameorepowered OUTPUT_DIR generated/default)
    set(CMAKE_CURRENT_SOURCE_DIR "${_kameo_source_dir}")
    set(KAMEO_SRC_DIR "${KAMEO_TU_WS}/src")
else()
    rexglue_project(kameorepowered OUTPUT_DIR generated/default)
    set(KAMEO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

if(NOT REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    message(FATAL_ERROR "No recompiled code. Run codegen first:\n  rexglue codegen kameorepowered_manifest.toml")
endif()

foreach(submodule plume XenosRecomp RexLiveless SentryWork RexFabric)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${submodule}/CMakeLists.txt")
        message(FATAL_ERROR "thirdparty/${submodule} is missing:\n  git submodule update --init --recursive")
    endif()
endforeach()
# Our changes to submodules whose repositories are not ours to push to: thirdparty/overlay/<submodule>/<path> is our
# version of that file (SentryWork ported to ReXGlue 0.11, RexFabric's node editor fitted to the window), copied over
# the submodule's own before it is built. When a submodule is moved to a newer commit, carry its changes to these
# files into the overlay, or drop the file from it once upstream has ours.
file(GLOB_RECURSE _kameo_overlay RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/overlay" "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/overlay/*")
foreach(_kameo_overlay_file IN LISTS _kameo_overlay)
    configure_file("thirdparty/overlay/${_kameo_overlay_file}" "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${_kameo_overlay_file}" COPYONLY)
endforeach()
add_subdirectory(thirdparty/plume plume EXCLUDE_FROM_ALL)

set(KAMEO_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/res/shader_cache/shader_cache.cpp")
file(READ "${KAMEO_SHADER_CACHE}" _kameo_shader_cache_head LIMIT 64)
if(_kameo_shader_cache_head MATCHES "^version https://git-lfs")
    message(FATAL_ERROR "res/shader_cache/shader_cache.cpp is a Git LFS pointer:\n  git lfs pull")
endif()

file(GLOB_RECURSE KAMEO_SOURCES CONFIGURE_DEPENDS "${KAMEO_SRC_DIR}/*.cpp")
list(FILTER KAMEO_SOURCES EXCLUDE REGEX "/src/(launcher|crash_dialog|shader_editor)/")
source_group(TREE "${KAMEO_SRC_DIR}" PREFIX src FILES ${KAMEO_SOURCES})
list(APPEND KAMEO_SOURCES
    "${KAMEO_SHADER_CACHE}"
    # XenosRecomp smol-v encodes the cache's SPIR-V, so the Vulkan backend decodes it before use.
    thirdparty/XenosRecomp/thirdparty/smol-v/source/smolv.cpp
    # zlib for reading the game's archives (core/game_archive.cpp), which shader mods take textures from.
    src/launcher/kmod_zlib.cpp)

if(WIN32)
    # The shader editor inside the game (F1): its own sources without the Direct3D 11 host, preview and window.
    foreach(_editor_file editor node_canvas shader_graph shader_build game_textures)
        list(APPEND KAMEO_SOURCES "${KAMEO_SRC_DIR}/shader_editor/${_editor_file}.cpp")
    endforeach()
    list(APPEND KAMEO_SOURCES src/launcher/zip_extract.cpp)
    add_executable(kameorepowered WIN32 ${KAMEO_SOURCES} res/windows/kameo.rc)
else()
    add_executable(kameorepowered ${KAMEO_SOURCES})
endif()

# Translations of our own text (src/core/localization.h), beside the programs that show it.
add_custom_target(kameo_lang ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/res/lang ${CMAKE_CURRENT_BINARY_DIR}/lang
    VERBATIM)
add_dependencies(kameorepowered kameo_lang)
# The storybook faces the pause book is set in (IM FELL, SIL Open Font License, OFL.txt beside them).
add_custom_target(kameo_fonts ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts ${CMAKE_CURRENT_BINARY_DIR}/fonts
    VERBATIM)
add_dependencies(kameorepowered kameo_fonts)
# Interface art of our own, made for these menus (the pause card's main page, res/ui).
add_custom_target(kameo_ui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/res/ui ${CMAKE_CURRENT_BINARY_DIR}/ui
    VERBATIM)
add_dependencies(kameorepowered kameo_ui)

target_include_directories(kameorepowered PRIVATE
    "${KAMEO_SRC_DIR}"
    res
    "${CMAKE_CURRENT_BINARY_DIR}/res"
    thirdparty
    thirdparty/tomlplusplus
    thirdparty/RexLiveless/third_party
    thirdparty/XenosRecomp/thirdparty/xxHash
    thirdparty/XenosRecomp/thirdparty/smol-v/source)

# plume is a static library, so its D3D12 imports are satisfied here; Vulkan is loaded through volk.
target_link_libraries(kameorepowered PRIVATE plume)
# The SDK's own SDL, the one that owns the window: input reads its per-device keyboard and mouse events (src/input/device_routing.cpp).
target_link_libraries(kameorepowered PRIVATE SDL3::SDL3)
if(WIN32)
    # dbghelp writes the crash reporter's minidump, and dwmapi takes the crashed game's window off the screen.
    target_link_libraries(kameorepowered PRIVATE d3d12 dxgi dbghelp dwmapi mfplat mfreadwrite mfuuid ole32 xaudio2)
elseif(UNIX AND NOT APPLE)
    # plume only builds Xlib surfaces on Linux, and the guest memory-free wrappers find the runtime's exports through dlsym.
    target_link_libraries(kameorepowered PRIVATE X11 ${CMAKE_DL_LIBS})
endif()

# Release keeps a PDB too, so the module+offset frames in a crash report resolve to functions. It stays with whoever built it: CI drops it from the archive and publishes no symbols. RelWithDebInfo has one from CMake's own defaults.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(kameorepowered PRIVATE $<$<CONFIG:Release>:-g -Xclang -gcodeview>)
    target_link_options(kameorepowered PRIVATE $<$<CONFIG:Release>:-g>)
endif()

# The SDK's Xenos drivers stay staged as the reference renderer behind --kameo.native_renderer=false.
rexglue_setup_target(kameorepowered)
if(KAMEO_TITLE_UPDATE)
    # The launcher starts kameorepowered-tu when the update is installed, and the sources pick its addresses by KAMEO_TU.
    set_target_properties(kameorepowered PROPERTIES OUTPUT_NAME kameorepowered-tu)
    target_compile_definitions(kameorepowered PRIVATE KAMEO_TU=1)
    # Every build re-translates src/ (rewriting only what changed), so edits there reach this flavour.
    add_custom_target(kameo_tu_translate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/tu/translate.py --sources
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Translating src/ to the title update's addresses"
        VERBATIM)
    add_dependencies(kameorepowered kameo_tu_translate)
endif()

# RexLiveless: the Xbox 360 Guide with sign-in, friends, achievements and online play through rexliveless.xyz. It
# replaces XAM and network imports, so its sources compile into the executable (src/app/app.cpp installs it).
option(KAMEO_GUIDE "Build the RexLiveless Guide into the game" ON)
if(KAMEO_GUIDE)
    include(thirdparty/RexLiveless/CMakeLists.txt)
    rexglue_guide_attach(kameorepowered)
    target_compile_definitions(kameorepowered PRIVATE KAMEO_GUIDE=1)
endif()
# RexFabric: Lua and native mods from the mods folder, hooks on every recompiled function, and the game's own modding
# API (src/mods/). Its hook slots come from codegen templates it composes from the SDK's, so it needs the SDK's source
# pieces: REXFABRIC_SDK_TEMPLATES is <rexglue-sdk>/resources/templates of the same SDK version, with the SDK's
# thirdparty/tomlplusplus and thirdparty/stb beside resources. The mods menu goes in the Guide when that is built.
option(KAMEO_MODS "Build RexFabric mod support into the game" ON)
if(KAMEO_MODS)
    include(thirdparty/RexFabric/CMakeLists.txt)
    if(KAMEO_GUIDE)
        rexfabric_attach(kameorepowered WITH_GUIDE WITH_NODE_EDITOR)
    else()
        rexfabric_attach(kameorepowered WITH_NODE_EDITOR)
    endif()
    target_compile_definitions(kameorepowered PRIVATE KAMEO_MODS=1)
endif()

# Sent with leaderboard scores, which the RexLiveless server refuses from a build with no name or an old one.
string(REGEX REPLACE "^v" "" _kameo_build_name "${KAMEO_VERSION}")
target_compile_definitions(kameorepowered PRIVATE KAMEO_BUILD_NAME="Kameo: RePowered ${_kameo_build_name}")

# SentryWork: crash reports to the project's Sentry, sent by the sentry-crash helper staged next to the executable
# (src/app/app.cpp starts it), which Windows builds name KameoBugReporter.exe. Only builds given a DSN report, which CI takes from the SENTRY_DSN repository variable for
# Windows, so forks and local builds send nothing. Linux gets no DSN: its minidumps carry the player's environment
# variables. The debug files go to Sentry with kameorepowered_upload_symbols, never into a release.
set(KAMEO_SENTRY_DSN "" CACHE STRING "Sentry DSN crash reports go to; empty builds without reporting")
set(KAMEO_SENTRY_ORG "" CACHE STRING "Sentry organization slug, for kameorepowered_upload_symbols")
set(KAMEO_SENTRY_PROJECT "" CACHE STRING "Sentry project slug, for kameorepowered_upload_symbols")
if(NOT KAMEO_SENTRY_DSN)
    set(SENTRYWORK_DISABLE ON)
endif()
set(_kameo_sentry_args RELEASE "kameorepowered@${KAMEO_VERSION}")
if(KAMEO_SENTRY_DSN)
    list(APPEND _kameo_sentry_args DSN "${KAMEO_SENTRY_DSN}")
endif()
if(KAMEO_SENTRY_ORG AND KAMEO_SENTRY_PROJECT)
    list(APPEND _kameo_sentry_args ORG "${KAMEO_SENTRY_ORG}" PROJECT "${KAMEO_SENTRY_PROJECT}")
endif()
option(KAMEO_SENTRY "Build SentryWork crash reporting into the game" ON)
if(KAMEO_SENTRY)
    include(thirdparty/SentryWork/CMakeLists.txt)
    sentrywork_attach(kameorepowered ${_kameo_sentry_args})
    target_compile_definitions(kameorepowered PRIVATE KAMEO_SENTRY=1)
endif()
# On Windows the helper ships under the game's name with the game's version details, since antivirus scanners treat an
# unknown exe with no publisher details more harshly; src/core/sentry_setup.cpp points Sentry at it.
if(WIN32 AND TARGET sentry-crash)
    set_target_properties(sentry-crash PROPERTIES OUTPUT_NAME KameoBugReporter)
    target_sources(sentry-crash PRIVATE res/windows/kameo.rc)
    target_include_directories(sentry-crash PRIVATE $<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_BINARY_DIR}/res>)
    target_compile_definitions(sentry-crash PRIVATE $<$<COMPILE_LANGUAGE:RC>:KAMEO_RC_BUG_REPORTER>)
endif()
# SentryWork builds Sentry's options itself; this sends its sentry_init call through src/core/sentry_setup.cpp first.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/SentryWork/src/sentrywork.cpp" APPEND PROPERTY COMPILE_DEFINITIONS "sentry_init=kameo_sentry_init")

# The SDK only stages its shared libraries next to the executable on Windows and macOS.
if(UNIX AND NOT APPLE)
    foreach(_kameo_runtime_lib rex::TracyClient)
        if(TARGET ${_kameo_runtime_lib})
            add_custom_command(TARGET kameorepowered POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${_kameo_runtime_lib}> $<TARGET_FILE_DIR:kameorepowered> VERBATIM)
        endif()
    endforeach()
endif()

# Non-Release SDK builds bump a shared atomic on every indirect guest call; the recompiled code opts out.
if(TARGET kameorepowered_recompiled)
    target_compile_options(kameorepowered_recompiled PRIVATE -UREXGLUE_ENABLE_PERF_COUNTERS)
endif()

# fp-contract stays off so separate PPC multiplies and adds are never fused into FMAs that round differently.
if(KAMEO_X86_64_V3 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(_kameo_target kameorepowered kameorepowered_recompiled)
        if(TARGET ${_kameo_target})
            target_compile_options(${_kameo_target} PRIVATE -march=x86-64-v3 -ffp-contract=off)
        endif()
    endforeach()
endif()

option(KAMEO_TOOLS "Build the launcher, the crash dialog and the shader editor with the game" ON)
# The launcher edits settings, installs DLC, fetches and switches builds and starts the game, borrowing the SDK's rexbase library for ImGui and its rexfilesystem module for the package and disc image readers.
if(WIN32 AND KAMEO_TOOLS)
    file(GLOB_RECURSE KAMEO_LAUNCHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/*.cpp")
    add_executable(KameoLauncher WIN32
        ${KAMEO_LAUNCHER_SOURCES}
        src/core/localization.cpp
        thirdparty/imgui_backends/imgui_impl_win32.cpp
        thirdparty/imgui_backends/imgui_impl_dx11.cpp
        res/windows/kameo.rc)
    target_include_directories(KameoLauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src thirdparty thirdparty/tomlplusplus "${CMAKE_CURRENT_BINARY_DIR}/res")
    # The ImGui backends define their entry points themselves, so they must not take rexbase's dllimport.
    target_compile_definitions(KameoLauncher PRIVATE IMGUI_IMPL_API= $<$<COMPILE_LANGUAGE:RC>:KAMEO_RC_LAUNCHER>)
    # bcrypt hashes the title update's patch against default.xex before installing it, and version reads which patch
    # kameorepowered-tu.exe was built from.
    target_link_libraries(KameoLauncher PRIVATE rex::base d3d11 dxgi dwmapi d3dcompiler winhttp bcrypt version)
    # The launcher runs on its own copies of the SDK libraries in launcher\ (see src/launcher/runtime_loader.cpp), so
    # switching builds can replace the game's: rexbase (ImGui, settings, the module loader) is delay-loaded from there,
    # and it loads rexfilesystem (the STFS and disc image readers) from the folder it was itself loaded from. Those two are all
    # launcher\ needs: their other imports are system DLLs and the C++ runtime. The file names carry the build configuration's
    # postfix (d, rd), which rexbase's own name already shows.
    target_link_options(KameoLauncher PRIVATE "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:rex::base>")
    target_link_libraries(KameoLauncher PRIVATE delayimp)
    # rexbase hands out strings and paths allocated by its own operator new, so every program that links it must forward its own
    # operator new and delete to the same allocator.
    rexglue_link_host_allocator(KameoLauncher)
    add_custom_command(TARGET KameoLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:KameoLauncher>/launcher
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rex::base> $<TARGET_FILE_DIR:rex::base>/rexfilesystem$<$<CONFIG:Debug>:d>$<$<CONFIG:RelWithDebInfo>:rd>.dll $<TARGET_FILE_DIR:KameoLauncher>/launcher/
        VERBATIM)
    add_dependencies(kameorepowered KameoLauncher)

    # The window that asks a player whether to send a crash to Sentry (src/core/crash_reporter.cpp starts it), drawn with the launcher's window and theme. Unlike the launcher it runs on the SDK's rexbase library beside it, because it ships with each build.
    add_executable(KameoCrashDialog WIN32
        src/crash_dialog/main.cpp
        src/core/localization.cpp
        src/launcher/window_d3d11.cpp
        src/launcher/ui/theme.cpp
        thirdparty/imgui_backends/imgui_impl_win32.cpp
        thirdparty/imgui_backends/imgui_impl_dx11.cpp
        res/windows/kameo.rc)
    target_include_directories(KameoCrashDialog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src thirdparty "${CMAKE_CURRENT_BINARY_DIR}/res")
    target_compile_definitions(KameoCrashDialog PRIVATE IMGUI_IMPL_API= $<$<COMPILE_LANGUAGE:RC>:KAMEO_RC_CRASH_DIALOG>)
    target_link_libraries(KameoCrashDialog PRIVATE rex::base d3d11 dxgi dwmapi d3dcompiler)
    rexglue_link_host_allocator(KameoCrashDialog)
    add_dependencies(kameorepowered KameoCrashDialog)

    # The shader editor (docs/modding.md), with the compiler and the game's translated shaders it builds mods from beside it.
    file(GLOB KAMEO_SHADER_EDITOR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/shader_editor/*.cpp")
    add_executable(KameoShaderEditor WIN32
        ${KAMEO_SHADER_EDITOR_SOURCES}
        src/core/localization.cpp
        src/core/game_archive.cpp
        src/launcher/window_d3d11.cpp
        src/launcher/ui/theme.cpp
        src/launcher/zip_extract.cpp
        src/launcher/kmod_zlib.cpp
        thirdparty/imgui_backends/imgui_impl_win32.cpp
        thirdparty/imgui_backends/imgui_impl_dx11.cpp
        res/windows/kameo.rc)
    target_include_directories(KameoShaderEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src thirdparty thirdparty/RexLiveless/third_party "${CMAKE_CURRENT_BINARY_DIR}/res")
    target_compile_definitions(KameoShaderEditor PRIVATE IMGUI_IMPL_API= $<$<COMPILE_LANGUAGE:RC>:KAMEO_RC_SHADER_EDITOR>)
    target_link_libraries(KameoShaderEditor PRIVATE rex::base d3d11 dxgi dwmapi d3dcompiler shell32 ole32)
    target_compile_definitions(KameoShaderEditor PRIVATE NOMINMAX)
    rexglue_link_host_allocator(KameoShaderEditor)
    set(KAMEO_DXC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64)
    add_custom_command(TARGET KameoShaderEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:KameoShaderEditor>/editor_shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KAMEO_DXC_DIR}/dxc.exe ${KAMEO_DXC_DIR}/dxcompiler.dll ${KAMEO_DXC_DIR}/dxil.dll
            $<TARGET_FILE_DIR:KameoShaderEditor>/editor_shaders
        VERBATIM)
    # Copied as the project configures, so a rebuilt pack arrives even when the editor itself doesn't relink.
    configure_file(res/shader_mods/source.zip ${CMAKE_CURRENT_BINARY_DIR}/editor_shaders/source.zip COPYONLY)
    add_dependencies(kameorepowered KameoShaderEditor)
endif()
