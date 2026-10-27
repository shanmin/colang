# Command Line

## Invocation

```
colang [options] <source.co>
```

One invocation handles **one** source file. When no action option is given, the compiler builds and links an executable.

## Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | Show usage and the option list |
| `-v`, `--version` | Show the compiler version, the LLVM version used, and the target platform |
| `-E` | Preprocess only; send the expanded token listing to standard output |
| `-c` | Compile only; produce an object file (COFF `.obj` for a Windows target, ELF `.o` for a Linux target) without linking |
| `-o <file>` | Write the output to `<file>` |
| `-I <dir>` | Add a header search directory; may also be written `-I<dir>` and repeated |
| `--target <triple>` | Set the target triple (e.g. `x86_64-unknown-linux-gnu`); defaults to the host platform when omitted |
| `--resource <file>` | Embed a PE resource into the executable; may be repeated. Accepts a `.rc` (compiled by the built-in resource compiler) or a precompiled `.res` (embedded as-is). Windows targets only |

## Output files and default names

| Action | Output | Default name when `-o` is omitted |
| --- | --- | --- |
| Default (compile + link) | Executable | Windows: `<source basename>.exe`; other platforms: `<source basename>` |
| `-c` | `.obj` object file | `<source basename>.obj`, left in the current directory |
| `-E` | Standard output | Always goes to standard output; `-o` has no effect |

When linking, the intermediate `.obj` is written to the system temporary directory and removed automatically after the link.

## What `-E` actually prints

`-E` prints a **line-by-line token listing**, shaped like:

```
hello.co:3:1  keyword     int
hello.co:5:12  string      "hello\n"
```

It is meant for inspecting macro expansion, conditional pruning and file inclusion; it is **not** C source that can be fed back into a compiler. Also, `-E` stops at preprocessing and performs no syntax checking: even if the source contains syntax errors, `-E` still prints tokens and returns 0.

## Header search

In order of decreasing priority:

1. Directories given with `-I` on the command line;
2. The Co-provided standard header directory (`stdio.h`, `stdint.h`, `winapi.co`, ...), located automatically: the environment variable `COLANG_STDLIB` is checked first (point it straight at the header directory); if unset, the compiler walks upward (at most 8 levels) from the directory containing the `colang` executable and locates that `include/` directory by the presence of `include/stddef.h`.

The release package is a flat single-level layout — `colang.exe` sits next to the standard header directory `include/`, so it works out of the box after extraction: neither `#include <stdio.h>` nor `import "winapi.co"` needs `-I`. If you move the compiler elsewhere, keep `colang.exe` and `include/` side by side, or point `COLANG_STDLIB` at the header directory. When the standard headers cannot be located, `#include <stdio.h>` fails with `#include file not found`.

## Target platform and data model

colang drives the whole pipeline from a single target: predefined macros, type widths, code generation and linking all come from the same target triple. When `--target` is omitted the target defaults to the **host platform** (Windows/LLP64 on Windows, Linux/LP64 on Linux).

| Target | Data model | `long` | `void*` | `wchar_t` | Key predefined macros |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | LLP64 | 4 | 8 | 2 | `_WIN32` `_WIN64` `_MSC_VER` `__LLP64__` |
| Linux x86_64 | LP64 | 8 | 8 | 4 | `__linux__` `__gnu_linux__` `__ELF__` `__x86_64__` `__LP64__` |

The data-model macros (`__LLP64__`/`__LP64__`) reflect the compiler's **real type widths** and are strictly consistent with `sizeof(long)`; standard-header typedefs such as `size_t`/`ptrdiff_t` and `wchar_t` are dispatched accordingly (on Linux `size_t` is `unsigned long` and `wchar_t` is 4 bytes, matching glibc). The width of `long`, together with every **integer promotion and usual arithmetic conversion** it takes part in, is likewise carried through the whole chain from the target (preprocessing, type inference, semantic analysis and code generation all read the same width), so on LP64 an expression involving `long` is evaluated as 64-bit and on LLP64 as 32-bit.

> Cross-target compilation (for example `--target x86_64-unknown-linux-gnu` on Windows) can produce ELF object files and IR; but the linking step uses the linker backend of the **platform the compiler was built on**, so linking artifacts across platforms is not currently supported.

Beyond the target-specific macros above, colang **always defines** one target-independent identification macro `__COLANG__` (value `1`), present on every platform and under any `--target`, so sources can test "whether this is being compiled by colang" (in the same family as `__clang__` and `__GNUC__`).

## Linker: built in

colang ships its own linker, so turning sources into an executable needs **no external `clang` and no linker command**.

- **Windows**: Linking requires the Windows SDK and the VC runtime libraries to be installed on the machine (they provide `libcmt` and other platform libraries). colang locates them automatically, so an ordinary command prompt is enough — a Developer command prompt is **not** required. By default the output **statically links the C runtime**, so an exe you build on your machine runs on Windows machines that do not have the VC redistributable installed.
- **Linux**: The in-process linker (`lld`) statically links glibc and libgcc to produce a native ELF executable. colang automatically locates `crt1.o`/`crti.o`/`crtn.o`, `libc.a` and `libgcc`; you need the static library packages installed (Debian/Ubuntu `libc6-dev`). The search locations can be overridden with the `COLANG_CRT_DIR`, `COLANG_LINUX_SYSROOT` and `COLANG_GCC_LIB_DIR` environment variables. Statically linked executables are currently supported; dynamic PIE / `.so` are still under development.

> **Compatibility note**: what you distribute is your **compiled program (exe)**, not colang itself. No LLVM/clang tooling needs to accompany the program.

## PE resource embedding

`--resource` embeds Windows resources (icons, version info, a manifest, ...) into the produced exe and may be repeated; for example `colang --resource app.rc app.co` (give the `.rc` through `--resource`, not as the positional source file). Each item takes one file:

- **`.res`**: a precompiled resource file, embedded as-is.
- **`.rc`**: a resource script, compiled and embedded by colang's **built-in resource compiler** — **no external `rc.exe` needed**.

A `.rc` script can `#include "resource.h"` for your own ID macros (system headers such as `<windows.h>` are not required and are skipped). The following resources are currently supported:

- icons via `ICON` — automatically split and grouped, so the `.ico` file is given only once;
- bitmaps via `BITMAP`;
- arbitrary binary data via `RCDATA`;
- an application manifest via `MANIFEST`;
- version info via `VERSIONINFO` — fixed attributes (`FILEVERSION`, `PRODUCTVERSION`, `FILEFLAGS`, `FILETYPE`, ...), plus `VALUE` text under `STRINGFILEINFO` and `TRANSLATION` under `VARFILEINFO`;
- string tables via `STRINGTABLE` (and its `STRING` entries) — encoded as `RT_STRING` following rc.exe's rules; multiple `STRINGTABLE` blocks in the same language may be declared across the script and are merged into the same group;
- accelerators via `ACCELERATORS` — encoded as `RT_ACCELERATOR` with rc.exe's per-entry 8-byte layout and its end-of-block `fVirt` 0x80 marker (reproduced for byte-for-byte parity); keys accept a quoted single character or a `VK_*` virtual-key name (the latter requiring `VIRTKEY`), the options `VIRTKEY`/`NOINVERT`/`SHIFT`/`CONTROL`/`ALT` accumulate as bit flags, and the command must be a numeric id after macro expansion;
- extended dialogs via `DIALOGEX` — encoded as `RT_DIALOG` following rc.exe's `DLGTEMPLATEEX`/`DLGITEMTEMPLATEEX` layout, **byte-for-byte identical** to rc.exe; the dialog header accepts `x,y,cx,cy[,helpID]` and the optional clauses `STYLE`/`EXSTYLE` (`|`-separated style expressions, which may use common `WS_*`/`DS_*`/`WS_EX_*` style macros or numeric values), `CAPTION`, `MENU`, `CLASS`, and `FONT pt,"face"[,weight[,italic[,charset]]]` (which automatically sets `DS_SETFONT`); controls include `PUSHBUTTON`/`DEFPUSHBUTTON`/`CHECKBOX`/`RADIOBUTTON`/`AUTOCHECKBOX`/`AUTORADIOBUTTON`/`GROUPBOX`/`LTEXT`/`RTEXT`/`CTEXT`/`ICON`/`EDITTEXT`/`LISTBOX`/`COMBOBOX`/`SCROLLBAR` (each mapping to a predefined class atom and rc.exe's default style) plus the general `CONTROL`; a control may be followed by optional style/exstyle/helpID override values;
- custom types and resource names: written as a numeric ID, a quoted string, or a bare identifier (a bare identifier is upper-cased and stored without quotes as a string name/type, matching rc.exe); plus per-resource `LANGUAGE` statements.

Qualifiers such as `MOVEABLE` and `DISKFILEONLY` are accepted and ignored (they do not affect the result); `LANGUAGE` statements take effect.

String literals in a `.rc` always **follow rc.exe's rules**: to embed a double quote inside a string, write `""` (doubled) rather than `\"` (rc.exe does not treat a backslash as escaping a quote). Recognized escapes are `\t`, `\n`, `\r`, `\a` (0x08), `\\`, octal `\0`–`\7` (at most 3 digits), and hex `\x` (at most 2 digits); any other `\X` keeps the backslash literally.

The `VERSIONINFO` (`VS_VERSION_INFO`) produced by the built-in resource compiler is **byte-for-byte identical** to rc.exe's; the fixed attribute `FILEOS` uses its given value when present and defaults to 0 when omitted (matching rc.exe).

**Current boundaries**:

- Resource embedding applies only to the **link output**; with `-c` (compile-only) `--resource` is ignored with a note.
- Only **Windows targets** are supported; cross-compiling to a non-Windows target reports an error.
- Script-style resources — the classic **`DIALOG` (non-EX form) / menus `MENU`** — are not yet supported and produce a clear diagnostic; `DIALOGEX` styles/controls use a built-in table, so custom control classes, rare style bits, and long control creation data (creation data) not listed in the table are not yet handled.

## Multi-file programs

A single invocation compiles one source file. colang composes several `.co` source files through the `import` mechanism (see the "import (Module Import)" chapter).

The single `.obj` produced by `-c` is useful for inspecting the compile result. Linking several object files directly into an executable is still under development; for now, organize multi-file programs with `import`.

## Diagnostics and exit codes

Diagnostics are printed to standard error in the form `file:line:column: error: message` (advisory ones as `warning`). There are only two exit codes: `0` on success, `1` on failure.

## Source files

Co source files use the `.co` extension by default (`.cop` for web-page based sources). A UTF-8 BOM at the start of a file is stripped automatically when it is read.

## Current boundaries

- `-h` also lists several options meant only for debugging the compiler itself (token, syntax tree and IR dumps). Their behavior and output format are not guaranteed to stay stable across versions, so this document makes no promises about them.
