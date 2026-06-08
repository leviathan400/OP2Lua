# Third-party dependencies

Everything needed to build is **vendored** here - there are no external deps to fetch and no
runtime DLLs to supply.

| Folder | What | How it got here |
|---|---|---|
| `Tethys/` | Reverse-engineered Outpost 2 API (header-only) | Vendored (TethysAPI). Include path is `third_party/`, so headers resolve as `Tethys/...`. |
| `lua/` | **Lua 5.4.7** sources | Downloaded from lua.org and extracted. Includes a `lua.hpp` C++ wrapper for sol2. |
| `sol2/` | **sol2** C++↔Lua bindings (header-only) | Cloned from GitHub, trimmed to `include/` + license. Provides `<sol/sol.hpp>`. |

That's the whole dependency list - the two-DLL design hooks nothing, so there's no LLVM, CLIF,
CPython, Patcher, capstone, or op2ext to build or ship.

## Updating the vendored libraries

**Lua** - CMake compiles every `*.c` in `lua/` except the standalone `lua.c` / `luac.c` mains:
```
curl -L -O https://www.lua.org/ftp/lua-5.4.7.tar.gz
tar xf lua-5.4.7.tar.gz
# copy lua-5.4.x/src/*.{c,h} into third_party/lua/   (keep lua.hpp)
```

**sol2** - header-only; the build expects `third_party/sol2/include/sol/sol.hpp`:
```
git clone --depth 1 https://github.com/ThePhD/sol2 /tmp/sol2
# copy /tmp/sol2/include into third_party/sol2/include
```

## Build

```
cmake -B build -G Ninja
cmake --build build            # -> build/OP2LuaCore.dll + build/LuaMission.dll
```

Use `-DOP2_GAME_DIR=<Outpost2 install>` to auto-copy `OP2LuaCore.dll` into the game folder on each
build. Requires a **32-bit** MSVC toolchain (configure inside the `x64_x86` cross-tools). See the
root `README.md` for install steps.
