# Changelog

This record lists major changes to the Co (colang) language/compiler by release version.
Versioning policy: 0.* = development/test releases, 1.* = official releases, 2.* = bootstrap.

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
