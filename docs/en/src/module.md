# Module

> As of **0.3.0**, the Co module system has landed alongside `import` (`.co` source merge): one `.co` file is one module, `public` is the **single gate** serving both dynamic-library export and cross-module visibility, and top-level statements of imported modules run exactly once at first load in a centralized topological order. Using `.obj` / `.dll` / `.so` as importable modules is deferred to later milestones; module-alias qualified access `m.f()` / `m.x` is likewise deferred.

## One file is one module

**One `.co` source file is one module.** The module is the basic unit Co uses to organize a program and draw interface boundaries: names defined inside a module belong to that module by default; to let other modules use them, mark them `public` explicitly.

A module is **named after its file by default** (the file name without the extension); on import, `as` can give it a local alias (the syntax slot is parsed but qualified access is not yet active — deferred). During loading, the **canonical absolute path** (case-insensitive on Windows) serves as the module's unique key; a module imported from multiple sites is loaded and merged only once.

Modules exist to support organizing large projects and to avoid the duplicate-definition and synchronization costs that C/C++ header (`.h`/`.cpp`) separation brings.

## The external interface: `public`

- `public` is Co's **only visibility keyword** — there is no `private`, and no separate `export`. It serves two roles at once: marking the **module interface** (importable by other modules) and **dynamic-library export** (written to the DLL export table).
- A definition without `public` is visible only inside its module; only a `public` definition is externally visible and importable.
- `public` may modify top-level functions and global variables; `public` on `class` / `struct` and their members is deferred to 0.4.0.
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen — references within the same module resolve, but no externally-visible LLVM value is emitted.

## Forms of a module

Physically, a module may take different artifact forms:

| Form | Description | Status |
| --- | --- | --- |
| `.co` | Source-file module, the most direct form | **Implemented since 0.3.0** |
| `.obj` | Compiled object file (`-c` output) | Deferred to later milestones |
| `.dll` / `.so` | Native dynamic library | Deferred to later milestones |

> **Why an `.obj`/`.dll` module is not "ready to use"**: `import` needs **compile-time declaration information** (function prototypes, type definitions, which names are externally visible), whereas an `.obj`/`.dll` is essentially a **link-time symbol definition**. For a binary artifact to become an "importable module" it must **additionally carry its own interface/declaration metadata** — a carrier that both the specification and the compiler will define in later milestones.

## Execution model of top-level statements

Top-level statements in an imported module follow "**exactly once at first load** + **centralized topological order**": all imported modules' top-level statements are emitted before the main entry, arranged in DFS-postorder of the dependency closure (dependencies run before dependents). The physical position of `import` lines in the main source has no effect on this order. **`main` of a non-main module is not a load hook** — if a module defines `main`, it is treated as an ordinary function symbol and does not execute automatically. See the "import (Module Import)" chapter.

## File-scope `static` alongside "omit `public`"

Since 0.3.0:
- **`public`** controls cross-module visibility — omitting it means "module-internal", adding it makes a symbol importable;
- **`static`** keeps its C meaning — internal linkage within its own translation unit, orthogonal to `public` (`static public` / `public static` in either order are legal; the symbol is still internal to the TU despite the export mark);
- In the target specification, `static` will only be a class static-member modifier (to be handled alongside 0.4.0 `class`).

## Not yet finalized

Deferred to later milestones:

- Full semantics of module-alias qualified access `m.f()` / `m.x` (the `ModAlias` syntax slot is parsed but not yet active);
- The declaration-metadata carrier that lets `.obj` / `.dll` / `.so` become importable modules;
- Cross-module `struct` / `enum` type references (this batch targets functions, globals, and top-level statements);
- `DllMain` synthesis and top-level body triggering under DLL outputs.

## Currently implemented boundary (0.3.0)

- **Module boundary**: one `.co` file = one module; module key = canonical absolute path; same-name files in different directories are distinguished by their paths.
- **`public`**: usable on top-level functions and global variables; when the artifact is a dynamic library the symbol is written into the export table (`DLLExportStorageClass`). In this batch `public` **does not change linkage** (non-`public` symbols remain `ExternalLinkage` by default, but non-`public` imported symbols are stripped to `InternalLinkage` by codegen).
- **`static public`** in either order is legal; semantics match P211.
- **Dynamic-library artifacts**: there is no `-shared` option, but the compiler switches between DLL and exe link modes automatically from the `-o` output extension. Naming the output `*.dll` produces an export-table-bearing dynamic library (an import library `*.lib` is generated alongside, verified on Windows).
- **Multi-file programs**: compose several `.co` sources with `import` (available since 0.3.0). The single `.obj` from `-c` is useful for inspecting the compile result; linking several object files directly into an executable is still under development (see the Command Line chapter).
- **Top-level execution model**: see the "import (Module Import)" chapter; this batch covers only `.co` consumption.
