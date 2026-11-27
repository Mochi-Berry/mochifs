# 02 — ABI Reference (mochifs)

- Native entry point: `mbjs_init_module()`
- ABI version: `MBC1`
- Header ABI:
- `include/mochifs.h`
- `include/mochifs_abi.h`

## Runtime exports (11 — from manifest.json)

- `appendText`
- `copyFile`
- `cwd`
- `exists`
- `mkdir`
- `readText`
- `readdir`
- `remove`
- `rename`
- `stat`
- `writeText`

Symbols above are exposed by `mochi_modules/src/mochifs.c` through
`MBJS_AddModuleExportList()` inside `mbjs_init_module()`; the values are
recorded by `MBJS_SetModuleExportList()` in the module init func.

## Build variables (no absolute host paths)

- `ENGINE_INC`: directory holding `mbjs.h` + `mbjs_runtime.h` (default
  `../libmochiberry`; override at build time).
- Windows link libs: (none additional).
