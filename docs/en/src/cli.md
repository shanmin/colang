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
| `-c` | Compile only; produce a `.obj` (COFF) object file without linking |
| `-o <file>` | Write the output to `<file>` |
| `-I <dir>` | Add a header search directory; may also be written `-I<dir>` and repeated |

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

## Linker: built in

colang ships its own linker, so turning sources into an executable needs **no external `clang` and no linker command**.

- Linking requires the Windows SDK and the VC runtime libraries to be installed on the machine (they provide `libcmt` and other platform libraries). colang locates them automatically, so an ordinary command prompt is enough — a Developer command prompt is **not** required.
- By default the output **statically links the C runtime**, so an exe you build on your machine runs on Windows machines that do not have the VC redistributable installed.

> **Compatibility note**: what you distribute is your **compiled program (exe)**, not colang itself. No LLVM/clang tooling needs to accompany the program.

## Multi-file programs

A single invocation compiles one source file. colang composes several `.co` source files through the `import` mechanism (see the "import (Module Import)" chapter).

The single `.obj` produced by `-c` is useful for inspecting the compile result. Linking several object files directly into an executable is still under development; for now, organize multi-file programs with `import`.

## Diagnostics and exit codes

Diagnostics are printed to standard error in the form `file:line:column: error: message` (advisory ones as `warning`). There are only two exit codes: `0` on success, `1` on failure.

## Source files

Co source files use the `.co` extension by default (`.cop` for web-page based sources). A UTF-8 BOM at the start of a file is stripped automatically when it is read.

## Current boundaries

- `-h` also lists several options meant only for debugging the compiler itself (token, syntax tree and IR dumps). Their behavior and output format are not guaranteed to stay stable across versions, so this document makes no promises about them.
