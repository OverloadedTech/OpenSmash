cmake_minimum_required(VERSION 3.25)
project(opensmash VERSION 0.1.0 LANGUAGES C CXX)
# Release builds pass -DOPENSMASH_VERSION_STRING=<tag>; local builds report the project version.
set(OPENSMASH_VERSION_STRING "${PROJECT_VERSION}-dev" CACHE STRING "Version reported by --version")
# macOS bundle version: numeric part of the release version (CFBundleShortVersionString allows at most three numbers)
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?" OPENSMASH_BUNDLE_VERSION "${OPENSMASH_VERSION_STRING}")
if(NOT OPENSMASH_BUNDLE_VERSION)
  set(OPENSMASH_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()
# Windows: link the C/C++ runtime statically (incl. bundled SDL3/Lua) so OpenSmash.exe needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SH_BUILD_GAME "Build the SDL3 game executable" ON)
option(SH_BUILD_TESTS "Build tests" ON)
# Distributable builds link the bundled SDL3 (third_party/) statically. A system SDL3 is only used when this is ON
# (the macOS and Windows presets turn it off so a Homebrew/vcpkg SDL3 cannot leak into a package).
option(SH_SYSTEM_SDL3 "Use an installed SDL3 if one is found" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- third party -------------------------------------------------------------
# Pinned versions. If third_party/<name> exists (tools/vendor_deps.sh; included in release source archives) it is
# used as-is and nothing is downloaded; otherwise the sources are fetched at configure time.
foreach(dep lua52 miniz stb SDL3)
  string(TOUPPER ${dep} DEP)
  if(NOT FETCHCONTENT_SOURCE_DIR_${DEP} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep}/.)
    set(FETCHCONTENT_SOURCE_DIR_${DEP} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep})
  endif()
endforeach()
# Lua 5.2 is what the original engine embeds; script semantics must match.
FetchContent_Declare(lua52
  URL https://www.lua.org/ftp/lua-5.2.4.tar.gz
  URL_HASH SHA256=b9e2e4aad6789b3b63a056d442f7b39f0ecfca3ae0f1fc0ae4e9614401b69f4b
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -DLUA_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_lua52.cmake)
FetchContent_Declare(miniz
  URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(lua52 miniz stb)
# the parser must carry the upstream for-list fix (cmake/patch_lua52.cmake); an unpatched tree is refused
file(READ ${lua52_SOURCE_DIR}/src/lparser.c LUA_PARSER_SRC)
string(FIND "${LUA_PARSER_SRC}" "OpenSmash: upstream fix from Lua 5.3.5" LUA_PATCHED)
if(LUA_PATCHED EQUAL -1)
  message(FATAL_ERROR "${lua52_SOURCE_DIR} lacks the Lua 5.3.5 parser fix: run cmake -DLUA_DIR=${lua52_SOURCE_DIR} -P cmake/patch_lua52.cmake")
endif()

file(GLOB LUA_SRC ${lua52_SOURCE_DIR}/src/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(lua52 STATIC ${LUA_SRC})
target_include_directories(lua52 PUBLIC ${lua52_SOURCE_DIR}/src)
if(UNIX)
  target_compile_definitions(lua52 PRIVATE LUA_USE_POSIX)
endif()
target_compile_options(lua52 PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
# Lua 5.2's number hashing relies on wrapping signed overflow (ltable.c hashnum); make it well-defined
target_compile_options(lua52 PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-fwrapv>)

add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

# ---- engine library (no SDL / GL dependency: usable headless) ---------------
add_library(shcore STATIC
  src/core/vfs.cpp
  src/core/xml.cpp
  src/physics/convex.cpp
  src/physics/poly.cpp
  src/physics/world.cpp
  src/physics/solver.cpp
  src/physics/fracture.cpp
  src/audio/ogg.cpp
  src/audio/ogg_impl.c
  src/assets/image.cpp
  src/script/lua_host.cpp
  src/game/level_script.cpp
  src/game/content.cpp
  src/game/level.cpp
  src/game/particles.cpp
  src/ui/fonts.cpp
  src/ui/ui_script.cpp)
target_include_directories(shcore PUBLIC src)
target_link_libraries(shcore PUBLIC lua52 stb_headers)
target_sources(shcore PRIVATE ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(shcore PUBLIC ${miniz_SOURCE_DIR})
if(MSVC)
  target_compile_options(shcore PRIVATE /W3)
else()
  target_compile_options(shcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(opensmash_headless src/app/headless_main.cpp)
target_link_libraries(opensmash_headless PRIVATE shcore)
if(MSVC)
  target_sources(opensmash_headless PRIVATE src/app/opensmash.manifest)
endif()

if(SH_BUILD_GAME)
  if(SH_SYSTEM_SDL3)
    find_package(SDL3 CONFIG QUIET)
  endif()
  if(NOT SDL3_FOUND)
    message(STATUS "Using the bundled SDL3 (release-3.2.22)")
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.22 GIT_SHALLOW TRUE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
  endif()
  add_library(shrender STATIC
    src/render/gl.cpp
    src/render/shader.cpp
    src/render/gfx.cpp
    src/render/game_renderer.cpp
    src/render/ui_renderer.cpp
    src/audio/audio_system.cpp
    src/app/game_app.cpp)
  target_link_libraries(shrender PUBLIC shcore SDL3::SDL3)
  add_executable(opensmash src/app/main.cpp)
  target_compile_definitions(opensmash PRIVATE OPENSMASH_VERSION="${OPENSMASH_VERSION_STRING}")
  target_link_libraries(opensmash PRIVATE shrender SDL3::SDL3)
  if(WIN32)
    set_target_properties(opensmash PROPERTIES WIN32_EXECUTABLE ON)   # double-click: no console window
    target_sources(opensmash PRIVATE src/app/win_console.cpp)        # --help/--version visible from cmd/PowerShell
    if(MSVC)
      target_sources(opensmash PRIVATE src/app/opensmash.manifest)   # UTF-8 process code page (non-ASCII paths)
    else()
      target_link_options(opensmash PRIVATE -Wl,--subsystem,windows)   # zig/lld ignores -mwindows
      # note: zig/mingw cross builds do not embed the manifest; release builds use MSVC
    endif()
  endif()
  if(APPLE)
    # Apple Silicon .app bundle (the user supplies the APK at first launch via the file dialog)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      set(OPENSMASH_MACOS_MIN "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
      set(OPENSMASH_MACOS_MIN "12.0")
    endif()
    set_target_properties(opensmash PROPERTIES
      MACOSX_BUNDLE ON
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
      MACOSX_BUNDLE_BUNDLE_NAME "OpenSmash"
      MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.opensmash"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${OPENSMASH_BUNDLE_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${OPENSMASH_BUNDLE_VERSION}"
      OUTPUT_NAME "OpenSmash")
    target_link_libraries(opensmash PRIVATE "-framework OpenGL")
  endif()
  install(TARGETS opensmash opensmash_headless BUNDLE DESTINATION . RUNTIME DESTINATION bin)
endif()

if(SH_BUILD_TESTS)
  enable_testing()
  function(sh_test name exe src)
    add_executable(${exe} ${src})
    target_link_libraries(${exe} PRIVATE shcore)
    add_test(NAME ${name} COMMAND ${exe})
  endfunction()
  sh_test(physics sh_physics_test tests/physics_test.cpp)
  sh_test(world sh_world_test tests/world_test.cpp)
  sh_test(poly sh_poly_test tests/poly_test.cpp)
  sh_test(lua sh_lua_test tests/lua_test.cpp)
  # these need the game data (SH_ASSETS) and exit with 77, reported as skipped, without it
  sh_test(parse_all_assets sh_asset_test tests/asset_test.cpp)
  sh_test(level_scripts sh_script_test tests/script_test.cpp)
  sh_test(ui_scripts sh_ui_test tests/ui_test.cpp)
  add_test(NAME headless_smoke COMMAND opensmash_headless --level 0 --seconds 10 --bot)
  # Every campaign room (each checkpoint played until the next one) plus zen/endless; ~30 s.
  add_test(NAME headless_all_rooms COMMAND opensmash_headless --level all --full)
  set_tests_properties(parse_all_assets level_scripts ui_scripts headless_smoke headless_all_rooms
    PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(headless_all_rooms PROPERTIES TIMEOUT 900 LABELS long)
endif()
