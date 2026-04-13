cmake_minimum_required(VERSION 3.22)
project(modloader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── Lua 5.4 static library ──────────────────────────────────────────────
file(GLOB LUA_SOURCES "${CMAKE_SOURCE_DIR}/third_party/lua54/src/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "lua\\.c$|luac\\.c$")
add_library(lua54_static STATIC ${LUA_SOURCES})
target_include_directories(lua54_static PUBLIC "${CMAKE_SOURCE_DIR}/third_party/lua54/src")
target_compile_definitions(lua54_static PRIVATE LUA_USE_LINUX)

# ── Dobby hooking library ───────────────────────────────────────────────
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/dobby" dobby_build)

# ── modloader shared library ────────────────────────────────────────────
set(MOD_SOURCES
    src/main.cpp
    src/core/init.cpp
    src/core/config.cpp
    src/core/symbols.cpp
    src/core/paths.cpp
    src/core/pattern_scanner.cpp
    src/core/object_monitor.cpp
    src/core/game_profile.cpp
    src/hook/process_event_hook.cpp
    src/hook/pe_trace.cpp
    src/hook/native_hooks.cpp
    src/reflection/reflection_walker.cpp
    src/reflection/lua_dump_generator.cpp
    src/reflection/class_rebuilder.cpp
    src/lua/lua_engine.cpp
    src/lua/lua_bindings.cpp
    src/lua/lua_uobject.cpp
    src/lua/lua_types.cpp
    src/lua/lua_enums.cpp
    src/lua/lua_delayed_actions.cpp
    src/lua/lua_ue4ss_types.cpp
    src/lua/lua_ue4ss_globals.cpp
    src/lua/lua_tarray.cpp
    src/lua/lua_ustruct.cpp
    src/lua/lua_enum_ext.cpp
    src/mods/mod_loader.cpp
    src/pak/pak_mounter.cpp
    src/pak/aes_extractor.cpp
    src/bridge/adb_bridge.cpp
    src/util/logger.cpp
    src/util/crash_handler.cpp
    src/util/notification.cpp
    src/util/safe_call.cpp
    src/core/auto_offsets.cpp
    src/lua/lua_console.cpp
    src/lua/lua_cast.cpp
)

add_library(modloader SHARED ${MOD_SOURCES})

target_include_directories(modloader PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/lua54/src"
    "${CMAKE_SOURCE_DIR}/third_party/sol2/include"
    "${CMAKE_SOURCE_DIR}/third_party/json/include"
    "${CMAKE_SOURCE_DIR}/third_party/dobby/include"
)

target_link_libraries(modloader PRIVATE
    lua54_static
    dobby
    log
    dl
    android
)

target_compile_options(modloader PRIVATE
    -fno-rtti
    -fexceptions
    -Wall
    -Wno-unused-function
    -O2
    -g                    # DWARF debug info — addr2line on crash logs
    -ffunction-sections   # Per-function sections for better GC
    -fdata-sections       # Per-data sections for better GC
)

target_link_options(modloader PRIVATE
    -Wl,--gc-sections
)

# ── Post-build: create stripped copy for deployment ─────────────────────
# The unstripped libmodloader.so (with DWARF) stays in build/ for addr2line.
# The stripped copy goes to build/deploy/ for pushing to the device.
find_program(STRIP_TOOL
    ${CMAKE_STRIP}
    "${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-strip"
    "${ANDROID_TOOLCHAIN_ROOT}/bin/aarch64-linux-android-strip"
)

add_custom_command(TARGET modloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/deploy"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:modloader>"
        "${CMAKE_BINARY_DIR}/deploy/libmodloader.so"
    COMMAND ${CMAKE_COMMAND} -E echo "[STRIP] Creating stripped copy for deployment..."
    COMMENT "Creating stripped deployment copy"
)

if(STRIP_TOOL)
    add_custom_command(TARGET modloader POST_BUILD
        COMMAND ${STRIP_TOOL} --strip-debug --strip-unneeded
            "${CMAKE_BINARY_DIR}/deploy/libmodloader.so"
        COMMENT "Stripping debug info from deploy copy"
    )
endif()
