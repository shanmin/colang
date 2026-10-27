# Changelog

This record lists major changes to the Co (colang) language/compiler by release version.
Versioning policy: 0.* = development/test releases, 1.* = official releases, 2.* = bootstrap.

## 0.3.2
- Built-in resource compiler: parses `.rc` syntax, supports ICON / BITMAP / RCDATA / STRINGTABLE / VERSIONINFO / ACCELERATORS / DIALOGEX / MENU resource types; output embedded directly into the PE executable (byte-level alignment with rc.exe).
- Linux cross-compilation & native linking: `--target=x86_64-unknown-linux-gnu` emits ELF objects; the in-process lld ELF linker provides end-to-end executables; LP64 data model (long=8, void*=8, wchar_t=4) correctly dispatched.
- Per-module compilation model promoted: each module compiles as an independent closure, cross-module same-name symbols are automatically routed (no collision); qualified access `mod.func()` / `mod.var` supported.
- `wchar_t` fully target-aware: width and signedness follow target (Win unsigned short 2 bytes / Linux int 4 bytes); `wint_t`, `L'…'` overflow judgement, and TC-prefixed characters dispatch correctly.
- const diagnostics aligned with clang: read-only violations now carry `note: … declared const here` pointing at the declaration site; new `-Wdefault-const-init` warning (function-local const objects or those with const members without initializer).
- DLL production: synthesised `DllMain` entry carrier, top-level initialisation code driving, `public` functions written into the export table.
- Multiple fixes in integer literal type selection, bit-field masking, and dynamic CRT linking.

## 0.3.1
- The `winapi.co` module: `import … from "winapi.co"` replaces `windows.h`, providing 215 Win32 API prototypes, 13 struct/union layouts and 266 constants, all SDK-verified; covers common GUI APIs for window creation, message loops, drawing and text metrics (dialogs, menus and common controls not yet covered).
- Windows GUI programs: the `[[subsystem("windows")]]` attribute (or defining `WinMain`) produces a console-less windowed program.
- Co-layer enhancements: `public const int` / `public struct` etc. from an imported module can be used across modules directly; designated initializers support chained designators and anonymous `union` members.
- C compatibility: universal character names (`\uXXXX`) in identifiers; `u8`/`u`/`U`/`L` prefixed character constants; `sizeof(string literal)`; `static_assert` in multiple scopes; the `[[nodiscard]]`/`[[noreturn]]`/`[[deprecated]]` attributes.
- Diagnostics: `const` read-only enforcement, up-front incomplete-type diagnostics and rejection of non-constant `switch` `case` labels, with wording aligned to mainstream compilers.
- Fixes: two families of runtime crashes caused by global initializers being mis-folded to null.
- Release package flattened to `colang.exe + include/` (no runtime redistributable, no extra configuration); native Linux (WSL) build brought up.

## 0.3.0
- Built-in linker: `-c` emits a COFF `.obj`, and the default action links the object into an executable — **no dependency on an external `clang`**.
- Static CRT linked by default: the produced exe ships without requiring the VC runtime to be installed.
- `-o <name>.dll` produces a Windows dynamic library: with an export table, and a `.lib` import library generated alongside the base name.
- `[[lib("user32")]]` declares library attribution, with no command-line flag needed.
- Linking requires a locally installed Windows SDK / VC runtime (auto-located).
- `import` (`.co` source merge) landed:
  - Syntax: `import * | name [as alias], … from ("file" | module_name) [as ModAlias];`;
  - Consumes `.co` sources only; `public` is the sole cross-module visibility gate;
  - Top-level statements run exactly once at first load, unified before the main entry in DFS-postorder topological order;
  - Normalized absolute path dedup; DFS cycle detection reporting the full chain;
  - Identifier form `from a` is accepted, equivalent to `from "a.co"` (same directory only).
- Referencing a non-imported symbol from the main source is rejected; importing a non-`public` symbol via named or `*` form is rejected.
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen (no externally-visible LLVM value is emitted).

See the "import (Module Import)" and "Module" chapters for semantics.

## 0.2.0
- Added bare top-level code, `public` functions, and `import` support;
- Added DLL/SO generation;
- Fixed several issues against the C language test suite.

## 0.1.0
A development/test release; the C language compiler is complete.
- The compiler supports C compilation; Co's object-oriented and other extensions (such as class) are at the specification/planning stage and are not yet implemented.

## 0.0.x
Development/test versions (historical). Features and syntax followed the then-current test versions and are not a release baseline.
