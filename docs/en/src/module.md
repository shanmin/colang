# Module

> This chapter describes the design direction for Co modules. Since **0.2.0**, the `public` keyword is partially implemented (lexer / parser / sema + dynamic-library export; it does **not** change linkage and does **not** participate in cross-module visibility yet); `import` and cross-module dependencies remain unimplemented. The "Currently implemented boundary" section at the end gives the real model you can rely on today.

## One file is one module

By design, **one `.co` source file is one module**. The module is the basic unit Co uses to organize a program and to draw interface boundaries: names defined inside a module belong to that module by default; to let other modules use them, mark them `public` explicitly.

A module is **named after its file by default** (the file name without the extension); when importing, `as` can give it a local alias (see the "import (Module Import)" chapter).

Modules exist to support organizing large projects and to avoid the duplicate-definition and synchronization costs that C/C++ header (`.h`/`.cpp`) separation brings.

## The external interface: `public`

- `public` is Co's **only visibility keyword** — there is no `private`, and no separate `export`. It serves two roles at once: marking the **module interface** (importable by other modules) and **dynamic-library export**.
- A definition without `public` is visible only inside its module; only a `public` definition is externally visible and importable.
- `public` may modify functions, `class`, `struct`, and class members (see the "Function Definition" and "class" chapters).

## Forms of a module (design direction)

Physically, a module may take different artifact forms:

| Form | Description | Status |
| --- | --- | --- |
| `.co` | Source-file module, the most direct form | Design core (the compiler does not yet let `import` consume it) |
| `.bc` | LLVM bitcode object file | Design direction · not implemented |
| `.dll` / `.so` | Native dynamic library | Long-term idea · not implemented |

> **Why a `.bc`/`.dll` module is not "ready to use"**: `import` needs **compile-time declaration information** (function prototypes, type definitions, which names are externally visible), whereas a `.bc`/`.dll` is essentially a **link-time symbol definition**. For a binary artifact to become an "importable module" it must **additionally carry its own interface/declaration metadata** — a carrier the specification has not defined and the compiler has not implemented. Linking several `.bc` files today merely combines them at link time; it cannot recover importable declarations from them.

## Not yet finalized

The following key points are **not yet finalized** in the specification, so this chapter gives only direction, not syntax details:

- the full rules for cross-module name visibility (whether bare names cross modules, the granularity of `public` export);
- how `.bc`/`.dll` forms carry and validate interface declarations;
- how module names and paths are resolved, and how dependency cycles are handled;
- when and how many times a module's top-level code runs during cross-module loading (the single-module entry and top-level execution model is covered in the "Program Entry and Execution" chapter);
- whether file-scope `static` stays meaningful once modules land, and how it coexists with "omit `public` = module-internal" (in the specification `static` is only a class-member modifier).

These will be filled in once the module system is implemented in the current-generation compiler and its semantics are finalized. The import mechanism's syntax is covered in the "import (Module Import)" chapter.

## Currently implemented boundary

Since **0.2.0**:

- `public` may modify top-level functions and global variables; when the artifact is a dynamic library the symbol is written into the export table (`DLLExportStorageClass`); `static public` in either order is legal. At this stage `public` **does not change linkage** and is **not** an open cross-module visibility marker (that requires the `import` layer). Visibility among multiple files inside the same artifact still follows C's linkage rules: `static` confines a name to its own translation unit, everything else is externally visible. **Note**: in Co's target model, "module-internal" is expressed by **omitting `public`**, not by `static`; in the specification `static` is only a class-member modifier.
- Dynamic-library artifacts: there is no `-shared` option, but the link step passes the user's output path through to `clang` verbatim; the driver auto-switches between DLL and exe based on the `-o` extension. Naming the output `*.dll` produces an export-table-bearing dynamic library (verified on Windows).
- Each `.co` is still compiled as an independent **translation unit**; the textual declarations provided by `#include` are visible within that unit. Cross-file reuse still goes through the compile + link route (see "Multi-file programs" in the "Command Line" chapter).
- `import` is still not implemented (see the "import (Module Import)" chapter for the current status); when and how many times a module's top-level code runs during cross-module loading is still to be finalized together with `import` (single-module entry model is covered in the "Program Entry and Execution" chapter).
