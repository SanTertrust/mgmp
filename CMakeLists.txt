# mgmp -- Mewgenics multiplayer mod.
#
# Replaces the old build.ps1. Same outputs, same /MT-static-CRT requirement
# (the DLL is injected into the game process and must not depend on a VC
# redist being present there -- see "Write procedure" in CLAUDE.md), just
# driven by CMake instead of one hand-rolled script.
#
#   cmake -B build -A x64
#   cmake --build build --config Release
#
# -> build\Release\mgmp.dll, build\Release\mgmp_loader.exe (multi-config
# generators such as Visual Studio) or build\mgmp.dll directly (Ninja / NMake,
# single-config). tools\net_test.ps1 handles both layouts.
cmake_minimum_required(VERSION 3.21)
project(mgmp LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "mgmp targets Mewgenics.exe, a Windows x64 PE -- see CLAUDE.md")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere, debug or release -- the payload runs inside the
# game's process, not its own, so it cannot assume a matching redist is
# loaded there.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MGMP_BUILD_TESTS "build the pure-logic unit tests in tests/" ON)

# third_party/minhook ships its own CMakeLists.txt (BUILD_SHARED_LIBS is OFF
# by default there, which is what we want: a static lib folded into mgmp.dll,
# same as build.ps1 compiling its .c files directly into the DLL link).
add_subdirectory(third_party/minhook)

add_library(mgmp SHARED
    res/mgmp_dll.rc
    src/core/dllmain.cpp
    src/core/mgmp_hooks.cpp
    src/core/mgmp_log.cpp
    src/core/mgmp_mem.cpp
    src/core/mgmp_rtti.cpp
    src/core/mgmp_config.cpp
    src/core/mgmp_crash.cpp
    src/core/mgmp_gpak.cpp
    src/core/mgmp_sigscan.cpp
    src/core/mgmp_resolve.cpp
    src/determinism/mgmp_turnaction.cpp
    src/determinism/mgmp_ability.cpp
    src/determinism/mgmp_record.cpp
    src/determinism/mgmp_rng.cpp
    src/determinism/mgmp_replay.cpp
    src/net/mgmp_net.cpp
    src/session/mgmp_lockstep.cpp
    src/session/mgmp_session.cpp
    src/session/mgmp_follow.cpp
    src/session/mgmp_savefile.cpp
    src/session/mgmp_leave.cpp
    src/session/mgmp_catsync.cpp
    src/session/mgmp_invsync.cpp
    src/session/mgmp_runhist.cpp
    src/session/mgmp_aim.cpp
    src/session/mgmp_nodehash.cpp
    src/session/mgmp_choice.cpp
    src/session/mgmp_combatlock.cpp
    src/session/mgmp_cursor.cpp
    src/session/mgmp_overlay.cpp
    src/ui/mgmp_ui.cpp
    # Dear ImGui, compiled straight into mgmp.dll -- same rule as minhook and
    # stb_image: the payload runs inside the game's process and cannot pull in
    # a second DLL that would have to be shipped and found beside it.
    #
    # The backends are win32 + opengl3, NOT sdl3. Every SDL symbol here is a
    # thunk through the SDL_DYNAPI table whose static target is the stub that
    # gets replaced at init, so the SDL3 backend's forty-odd direct SDL_* calls
    # would each need a slot RVA decoded by hand -- see the header note in
    # src/ui/mgmp_ui.h. imgui_impl_opengl3 carries its own GL loader
    # (imgui_impl_opengl3_loader.h, LoadLibraryA("opengl32.dll") +
    # wglGetProcAddress), so it needs no GL import library either, and both
    # backends declare gdi32/dwmapi themselves via #pragma comment(lib).
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
)
set_target_properties(mgmp PROPERTIES OUTPUT_NAME "mgmp" PREFIX "")
target_compile_definitions(mgmp PRIVATE
    WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX
)
target_compile_options(mgmp PRIVATE /W3)
# Flat quoted includes (#include "mgmp_hooks.h") resolve across folders via
# these four dirs, so moving a file between layers never touches an #include.
target_include_directories(mgmp PRIVATE
    src/core src/determinism src/net src/session src/ui
    # stb_image.h, header-only; the implementation is compiled into
    # mgmp_overlay.cpp, which is the only translation unit that decodes a PNG.
    third_party
    # imgui.h and "backends/imgui_impl_*.h" -- the backends are included with
    # their directory prefix so this one entry covers both.
    third_party/imgui
)
target_link_libraries(mgmp PRIVATE minhook kernel32 user32 shell32)

add_executable(mgmp_loader loader/mgmp_loader.cpp res/mgmp_loader.rc)
target_compile_definitions(mgmp_loader PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(mgmp_loader PRIVATE /W3)
# third_party for json.hpp: the loader parses mgmp.json for the game path,
# which is the one setting it needs and the DLL cannot supply.
target_include_directories(mgmp_loader PRIVATE src/core third_party)
target_link_libraries(mgmp_loader PRIVATE kernel32 user32)

# Ship a default config next to mgmp_loader.exe, but never clobber one the user
# has since edited (game, net.role, net.control) -- same rule build.ps1 followed.
add_custom_command(TARGET mgmp_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            "-DSRC=${CMAKE_SOURCE_DIR}/mgmp.json.template"
            "-DDST=$<TARGET_FILE_DIR:mgmp_loader>/mgmp.json"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
)

if(MGMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
