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
| `-c` | Compile only; produce a `.bc` object file without linking |
| `-o <file>` | Write the output to `<file>` |
| `-I <dir>` | Add a header search directory; may also be written `-I<dir>` and repeated |

## Output files and default names

| Action | Output | Default name when `-o` is omitted |
| --- | --- | --- |
| Default (compile + link) | Executable | Windows: `<source basename>.exe`; other platforms: `<source basename>` |
| `-c` | `.bc` object file | `<source basename>.bc`, left in the current directory |
| `-E` | Standard output | Always goes to standard output; `-o` has no effect |

When linking, the intermediate `.bc` is written to the system temporary directory and removed automatically after the link.

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
2. The Co-provided standard header directory (`stdio.h`, `stdint.h`, ...), located automatically: the environment variable `COLANG_STDLIB` is checked first (point it straight at the header directory); if unset, the compiler walks upward from the directory containing the `colang` executable and locates `stdlib/include` by the presence of `stdlib/include/stddef.h`.

The release package keeps that relative layout, so it works out of the box after extraction. If you move the compiler elsewhere, keep `bin/` and `stdlib/include/` in their relative positions, or point `COLANG_STDLIB` at the header directory. When the standard headers cannot be located, `#include <stdio.h>` fails with `#include file not found`.

## Linking prerequisite: an external clang

The compiler produces LLVM bitcode; turning it into an executable is done by an external `clang`. It is looked up in this order:

1. Walking upward from the directory containing the `colang` executable, looking for `clang` under `llvm*/build/Release/bin/` (then `Debug`);
2. Falling back to the `clang` found on `PATH`.

On a machine without `clang`, only the non-linking actions such as `-E` and `-c` work; the default compile + link fails with `linking failed`.

> **Compatibility note**: this dependency is part of the runtime environment of the release package. What you distribute to a machine without LLVM tooling is your **compiled program**, not colang itself.

## Multi-file programs

A single invocation accepts only one source file; any additional positional argument whose name ends in `.bc` is treated as a link input:

```
colang -c a.co                 # produces a.bc
colang -c b.co                 # produces b.bc
colang a.bc b.bc -o app.exe    # link both object files into an executable
```

Without `-o`, a pure link action defaults to `a.exe` (Windows) / `a.out` (other platforms). The module mechanism (`import`) is not open yet; see the "Module" and "import (Module Import)" chapters for the design direction.

## Diagnostics and exit codes

Diagnostics are printed to standard error in the form `file:line:column: error: message` (advisory ones as `warning`). There are only two exit codes: `0` on success, `1` on failure.

## Source files

Co source files use the `.co` extension by default (`.cop` for web-page based sources). A UTF-8 BOM at the start of a file is stripped automatically when it is read.

## Current boundaries

- `-h` also lists several options meant only for debugging the compiler itself (token, syntax tree and IR dumps). Their behavior and output format are not guaranteed to stay stable across versions, so this document makes no promises about them.
