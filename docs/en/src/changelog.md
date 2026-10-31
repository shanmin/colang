# Changelog

This record lists major changes to the Co (colang) language/compiler by release version.
Versioning policy: 0.* = development/test releases, 1.* = official releases, 2.* = bootstrap.

## 0.5.0
- **`long` / `unsigned long` in `.co` sources is now 8 bytes** (Windows target); `long` and `long long` are now the same type. `.c` / `.h` are unchanged and still follow the target data model (4 bytes on Windows / 8 on Linux). Three migration consequences: ① `long` and `long long` can no longer both appear in one `_Generic`; ② taking the address of a `.co` `long` variable and passing it to an API prototype declared with `int` / `unsigned int` (`winapi.co` follows the SDK spellings and never uses `long`) is now rejected as incompatible pointer types; ③ printing a 64-bit integer requires `%lld` — `%ld` reads only 4 bytes and yields a wrong value.
- `winapi.co` constants corrected and expanded (every one of the 509 constants in the section was re-verified numerically against Windows SDK 10.0.26100.0): `SM_CXSCREEN` 78→`0`, `SOMAXCONN` -1→`0x7FFFFFFF`, `FILE_NOTIFY_CHANGE_LAST_WRITE` `0x8`→`0x10`, `PATCOPY` `0x00F00062`→`0x00F00021` — **code using these four constants changes runtime behaviour**, since it previously passed wrong values. Also added `FILE_NOTIFY_CHANGE_SIZE`, `SM_CYSCREEN`, `SM_XVIRTUALSCREEN`, `SM_YVIRTUALSCREEN`, `SM_CXVIRTUALSCREEN` and `SM_CYVIRTUALSCREEN`, so locally re-declared stand-in constants can go back to the `winapi.co` ones.
- **Literal and type-specifier diagnostic conformance**: the spellings below were silently accepted, folded to wrong values, or **crashed the compiler with no diagnostic at all**; they are now all rejected, with message text and line/column positions matching clang case by case: ① truncated exact-width suffixes (`1w`, `0x1w` → `invalid suffix 'w' on integer constant`); ② `signed` / `unsigned` on floating-point types (`signed float`, `unsigned double` → `'float' cannot be signed or unsigned`); ③ mixed-case suffixes (`1lL`, `1wB`, `1.0lF`); ④ illegal digit strings (`0b2`, `08`, `1f`); ⑤ a missing exponent digit (`1e`, `0x1p`) and hexadecimal floating constants missing a significand or an exponent (`0x.`, `0x1.5`); ⑥ integer literals wider than `unsigned long long` (`18446744073709551616`, `0xFFFFFFFFFFFFFFFFFFFF`). Valid spellings are unaffected: `1uwb` / `1UWB` / `0b1010wb`, `1.0f` / `1.0L`, `08.5` / `09e1` and `9223372036854775808` (typed `unsigned long long`) are still accepted.
- `winapi.co` batch XIII expansion and two return-type corrections: corrected `SendMessageA`'s return type `int`→`long long` (SDK returns LRESULT, 8 bytes on x64; the old spelling truncated the high 32 bits of the return value) and `RegisterClassA`'s return type `short`→`unsigned short` (SDK returns ATOM=WORD; window-class atoms always land in 0xC000–0xFFFF and read as negative when taken as `short`). Added the `WNDCLASSW` struct, **18** prototypes (user32 `CreateMenu`/`GetMenu`/`SetMenu`/`GetSubMenu`/`EnableMenuItem`/`RegisterClassW`/`UnregisterClassW`/`RegisterHotKey`/`UnregisterHotKey`/`LoadStringW`; kernel32 `CopyFileW`/`RemoveDirectoryW`/`GetFileSizeEx`/`FormatMessageW`/`LocalAlloc`/`LocalFree`; gdi32 `GetTextMetricsW`; comdlg32 `GetSaveFileNameW`) and **37** constants each verified line by line against Windows SDK 10.0.26100.0 (`MOD_*`/`WM_HOTKEY`/`VK_F1..F4`/`MF_*`/`FORMAT_MESSAGE_*`/`LMEM_*`/`GENERIC_*`/`CREATE_*`/`FILE_*`/`ERROR_*`, etc.), bringing the module to **374 prototypes / 46 struct-union layouts / 546 constants**. A "known boundaries" section at the end of `winapi.co` documents the compiler-layer limits still outstanding for consumers.
- **Windows x64 by-value struct/union ABI fix**: the compiler used to lower every by-value struct/union parameter and return to `ptr`, which does not match the winx64 ABI (aggregates of ≤ 8 bytes must be packed into a single general-purpose register; only larger ones take an implicit byval pointer). colang's caller and callee sides shared the same lowering so internal calls stayed self-consistent — the mismatch only showed up when calling external Win32 APIs. Now aligned with clang: aggregates whose store size is 1/2/4/8 are lowered to the matching integer scalar (`i8/i16/i32/i64`) at the signature boundary; larger or non-power-of-two sizes stay as `ptr` (the correct Microsoft x64 "pass by reference" lowering). `WindowFromPoint(struct POINT)` behaviour is corrected; `SetFilePointerEx(union LARGE_INTEGER)` is added to `winapi.co`; the corresponding item in the "known boundaries" section is flipped to fixed. **Remaining boundaries**: the 16-byte pure-integer aggregate "two-register" special case, and external APIs with >8-byte aggregates, are still passed by pointer. Non–Windows-x64 targets are unchanged byte-for-byte.

## 0.4.1
- Diagnostics-conformance wave: compile-time errors/warnings and the diagnostic context were realigned with clang and ISO C/C++, converging several silent-wrong-value/misjudgement sites (file-scope `bool`/integer constant folding now follows "nonzero is true").
- Diagnostics: every error/warning now shows the offending line with +/-2 lines of surrounding source and a caret; `-fno-diag-context` disables it.
- CLI and source-surface tightening: new `-l`/`--lib` to specify external libraries; `[[lib]]` and `class` definitions are restricted to `.co` sources (rejected in `.c`/`.h`).
- Co-layer robustness: fixed the degradation/crash when using a cross-module `public struct` by value inside an `import`ed module.
- `winapi.co` continues to expand: from 0.4.0's 323 prototypes / 39 struct-union layouts / 396 constants up to roughly 356 prototypes / 45 struct-union layouts / 503 constants, adding the winsock (ws2_32) socket family, the crypt32 string-encoding family, and more.

## 0.4.0
- The `class` reference type (heap-only + automatic reference counting): instantiation via `new`, member access via `this`, instance methods and static members via `C::m()`; handles are released in reverse frame order when leaving scope, and `break`/`continue`/`goto` path terminations release them correctly.
- Member and parameter types opened up: primitives, `class`, `struct`/`union`/arrays all work — a `struct`/`union`/array member is embedded by value (not reference counted, allocated and freed with its host); a class handle as a parameter **borrows** (ownership is not transferred), as a member it is **owned by the host** (assignment takes ownership, the destructor releases cascadingly).
- The `delete` statement: forced destruction (bypasses the count — destructor + cascading release + free); the slot is null-ed to prevent a double free; it can also be used to break reference cycles manually.
- `public class`: in a single module and under `-o *.dll`, exports the lifecycle protocols, the instance/static methods and the members.
- Current boundaries: no inheritance and no virtual functions; a `public class` in an `import`ed module cannot yet be consumed across modules; reference cycles are not broken automatically (use `delete`).
- Major `winapi.co` expansion: 323 Win32 API prototypes, 39 struct/union layouts and 396 constants, each re-verified against Windows SDK 10.0.26100.0; beyond the window/message-loop/drawing base it now adds dialogs, menus, common controls, tray icons, DWM, PDH, input method (imm32), file-time/directory enumeration and more.
- Bare tags in the Co layer (C++ style): in `.co` sources a `struct`/`union`/`enum` variable definition, parameter or `sizeof` may drop the keyword (`Point p;`), and a `public` tag of an imported module may be used bare as well; not enabled for `.c`, which stays byte-for-byte ISO C.
- Version strings extended to four segments `MAJOR.MINOR.PATCH.Build`: the fourth is a build counter increased by a real MSVC rebuild (it resets to zero when the three-segment version changes); `-v` output and the PE version info of artifacts follow suit.
- New default settings header `include/colang.h`: expanded ahead of the main source at compile time (main source only, not for imported modules), for your own global macro definitions.

> The last three items above (bare tags, the four-segment version, the default settings header) were already-shipped changes that never got recorded — the first two landed within the 0.3.1 cycle, the third in the 0.3.2 cycle; they are back-filled here.

## 0.3.2
- Built-in resource compiler: parses `.rc` syntax, supports ICON / BITMAP / RCDATA / STRINGTABLE / VERSIONINFO / ACCELERATORS / DIALOGEX / MENU resource types; output embedded directly into the PE executable (byte-level alignment with rc.exe).
- Linux cross-compilation & native linking: `--target=x86_64-unknown-linux-gnu` emits ELF objects; the in-process lld ELF linker provides end-to-end executables; LP64 data model (long=8, void*=8, wchar_t=4) correctly dispatched.
- Per-module compilation model promoted: each module compiles as an independent closure, cross-module same-name symbols are automatically routed (no collision); qualified access `mod.func()` / `mod.var` supported.
- `wchar_t` fully target-aware: width and signedness follow target (Win unsigned short 2 bytes / Linux int 4 bytes); `wint_t`, `L'…'` overflow judgement, and TC-prefixed characters dispatch correctly.
- const diagnostics aligned with clang: read-only violations now carry `note: … declared const here` pointing at the declaration site; new `-Wdefault-const-init` warning (function-local const objects or those with const members without initializer).
- DLL production: synthesised `DllMain` entry carrier, top-level initialisation code driving, `public` functions written into the export table.
- Multiple fixes in integer literal type selection, bit-field masking, and dynamic CRT linking.

## 0.3.1
- The `winapi.co` module: `import … from "winapi.co"` replaces `windows.h`, an SDK-verified set of Win32 API prototypes / struct-union layouts / constants; the module grows across releases and totals 323 prototypes / 39 struct-union layouts / 396 constants as of 0.4.0. First introduced covering common GUI APIs for window creation, message loops, drawing and text metrics.
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
