cmake_minimum_required(VERSION 3.21)
project(OP2Lua VERSION 0.6.0 LANGUAGES CXX C)
# NOTE: keep this VERSION in sync with OP2LUA_VERSION in src/OP2LuaCore.h.

# ---------------------------------------------------------------------------
# OP2Lua builds TWO DLLs:
#
#   OP2LuaCore.dll  - the shared Lua runtime (interpreter + sol2 bindings + mission/placement
#                     logic). Ships once, lives in the game folder.
#   LuaMission.dll  - a tiny per-mission stub: exports the standard OP2 mission interface
#                     (DescBlock/InitProc/On*/...) and forwards into OP2LuaCore.dll. The converter
#                     clones this and byte-patches its DescBlock per mission. Code is identical
#                     for every mission.
#
# OP2 is 32-bit, so build for x86 (use the x64_x86 cross tools). Ninja recommended.
#   cmake -B build -G Ninja
#   cmake --build build
#
# Vendored under third_party/: Tethys (header-only API), Lua 5.4.7, sol2. That's the whole list.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "OP2Lua must be built for 32-bit (x86). Use the x64_x86 cross tools / an x86 kit.")
endif()

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# --- Version resource (Properties > Details: File version, Copyright, ...) --------------------
if(MSVC)
  enable_language(RC)
  set(OP2LUA_VER_MAJOR ${PROJECT_VERSION_MAJOR})
  set(OP2LUA_VER_MINOR ${PROJECT_VERSION_MINOR})
  set(OP2LUA_VER_PATCH ${PROJECT_VERSION_PATCH})
  set(OP2LUA_VER_STR   "${PROJECT_VERSION}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/OP2LuaCore.version.rc" @ONLY)
  set(OP2LUACORE_RC "${CMAKE_CURRENT_BINARY_DIR}/OP2LuaCore.version.rc")
  # The stub's resource is fixed; the new-mission tools byte-patch its FileDescription per mission.
  set(OP2LUAMISSION_RC "${CMAKE_CURRENT_SOURCE_DIR}/stub/LuaMission.rc")
endif()

# Statically link the CRT for both targets (no VC runtime DLL dependency).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Lua 5.4 sources (compiled into the core) ------------------------------
file(GLOB LUA_SOURCES "${THIRD_PARTY}/lua/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac)\\.c$")
if(NOT LUA_SOURCES)
  message(WARNING "No Lua sources in third_party/lua/ - see third_party/README.md.")
endif()

# ===========================================================================
# OP2LuaCore.dll - the shared runtime
# ===========================================================================
add_library(OP2LuaCore SHARED
  src/Mission.cpp
  src/Placement.cpp
  src/LuaHost.cpp
  src/Bindings.cpp
  src/Names.cpp
  src/ErrorHandling.cpp
  src/Log.cpp
  ${LUA_SOURCES}
  ${OP2LUACORE_RC}                 # Windows version/copyright resource (MSVC only)
)
target_include_directories(OP2LuaCore PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${THIRD_PARTY}                  # for "Tethys/..."
  ${THIRD_PARTY}/lua
  ${THIRD_PARTY}/sol2/include
)
target_compile_definitions(OP2LuaCore PRIVATE
  OP2LUACORE_EXPORTS              # export the OP2LuaCore_* C API
  NOMINMAX                        # sol2/STL need it; Tethys's unqualified max() is shimmed in-source
  _CRT_SECURE_NO_WARNINGS
  MAKE_LIB                        # build Lua as a library
)
target_link_libraries(OP2LuaCore PRIVATE dbghelp)  # crash reporter
set_target_properties(OP2LuaCore PROPERTIES OUTPUT_NAME "OP2LuaCore" PREFIX "")

# ===========================================================================
# LuaMission.dll - the per-mission stub template
# ===========================================================================
add_library(LuaMission SHARED stub/LuaMission.cpp ${OP2LUAMISSION_RC})
target_include_directories(LuaMission PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src   # for OP2LuaCore.h
  ${THIRD_PARTY}                    # for "Tethys/..."
)
target_compile_definitions(LuaMission PRIVATE _CRT_SECURE_NO_WARNINGS)
# NOTE: no NOMINMAX here - the stub includes <windows.h> before the Tethys headers, so the
# windows.h min/max macros satisfy Tethys's unqualified max() usage. The stub uses no STL min/max.
target_link_libraries(LuaMission PRIVATE OP2LuaCore)   # links the auto-generated import lib
set_target_properties(LuaMission PROPERTIES OUTPUT_NAME "LuaMission" PREFIX "")

# Optional: copy artifacts into a game install for quick iteration.
set(OP2_GAME_DIR "" CACHE PATH "Outpost2 install dir; if set, DLLs are copied there post-build")
if(OP2_GAME_DIR)
  add_custom_command(TARGET LuaMission POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OP2LuaCore> "${OP2_GAME_DIR}/OP2LuaCore.dll")
endif()
