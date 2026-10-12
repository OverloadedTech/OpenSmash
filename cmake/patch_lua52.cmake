# Applies the upstream fix from Lua 5.3.5 for "Expression list with four or more expressions in a 'for' loop can
# crash the interpreter" (a heap overflow in the parser, reachable from any script in an APK) to a Lua 5.2.4 tree.
# Usage: cmake -DLUA_DIR=<lua-5.2.4 dir> -P cmake/patch_lua52.cmake   (idempotent; FetchContent and vendor_deps.sh)
if(NOT LUA_DIR)
  message(FATAL_ERROR "patch_lua52.cmake: pass -DLUA_DIR=<lua source dir>")
endif()
set(f "${LUA_DIR}/src/lparser.c")
file(READ "${f}" src)
string(FIND "${src}" "OpenSmash: upstream fix from Lua 5.3.5" done)
if(NOT done EQUAL -1)
  return()
endif()
set(old1 [=[      luaK_nil(fs, reg, extra);
    }
  }
}
]=])
set(new1 [=[      luaK_nil(fs, reg, extra);
    }
  }
  /* OpenSmash: upstream fix from Lua 5.3.5 ('Expression list with four or more expressions in a for loop can crash
     the interpreter'): extra values are removed here, so forlist's adjust_assign(ls, 3, nexps, ...) drops them too */
  if (nexps > nvars)
    ls->fs->freereg -= nexps - nvars;  /* remove extra values */
}
]=])
set(old2 [=[    if (nexps != nvars) {
      adjust_assign(ls, nvars, nexps, &e);
      if (nexps > nvars)
        ls->fs->freereg -= nexps - nvars;  /* remove extra values */
    }
]=])
set(new2 [=[    if (nexps != nvars)
      adjust_assign(ls, nvars, nexps, &e);  /* OpenSmash: extra values removed inside (Lua 5.3.5 fix) */
]=])
foreach(i 1 2)
  string(FIND "${src}" "${old${i}}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "patch_lua52.cmake: ${f} is not the expected Lua 5.2.4 source (hunk ${i} not found)")
  endif()
  string(REPLACE "${old${i}}" "${new${i}}" src "${src}")
endforeach()
file(WRITE "${f}" "${src}")
message(STATUS "Patched ${f} (Lua 5.3.5 for-list fix)")
