# Cross-compile with zig cc (see tools/make-zig-wrappers.sh). Set ZIG_WRAPPERS to the wrapper directory.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(ZIG_WRAPPERS "$ENV{ZIG_WRAPPERS}")
set(CMAKE_C_COMPILER "${ZIG_WRAPPERS}/zig-cc-x86_64-windows-gnu")
set(CMAKE_CXX_COMPILER "${ZIG_WRAPPERS}/zig-c++-x86_64-windows-gnu")
set(CMAKE_AR "${ZIG_WRAPPERS}/zig-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${ZIG_WRAPPERS}/zig-ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
