# Cross-compile with zig cc (see tools/make-zig-wrappers.sh). Set ZIG_WRAPPERS to the wrapper directory.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ZIG_WRAPPERS "$ENV{ZIG_WRAPPERS}")
set(CMAKE_C_COMPILER "${ZIG_WRAPPERS}/zig-cc-aarch64-linux-gnu")
set(CMAKE_CXX_COMPILER "${ZIG_WRAPPERS}/zig-c++-aarch64-linux-gnu")
set(CMAKE_AR "${ZIG_WRAPPERS}/zig-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${ZIG_WRAPPERS}/zig-ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
