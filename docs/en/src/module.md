# Module

> As of **0.3.0**, the Co module system has landed alongside `import` (`.co` source merge): one `.co` file is one module, `public` is the **single gate** serving both dynamic-library export and cross-module visibility, and top-level statements of imported modules run exactly once at first load in a centralized topological order. Since **0.3.2**, modules are promoted to **independent compilation closures** (per-module isolation), **module-alias qualified access** `m.f()` / `m.x` is enabled, and **DLL / WinMain top-level-init carriers** have landed. Using `.obj` / `.dll` / `.so` as importable modules is still deferred to later milestones.

## One file is one module

**One `.co` source file is one module.** The module is the basic unit Co uses to organize a program and draw interface boundaries: names defined inside a module belong to that module by default; to let other modules use them, mark them `public` explicitly.

A module is **named after its file by default** (the file name without the extension); on import, `as` can give it a local alias used for qualified access `alias.f()` / `alias.x` (enabled since 0.3.2). When the file name is not a valid identifier, the default qualifier is unusable and an explicit `as` alias is required. During loading, the **canonical absolute path** (case-insensitive on Windows) serves as the module's unique key; a module imported from multiple sites is loaded and merged only once.

Modules exist to support organizing large projects and to avoid the duplicate-definition and synchronization costs that C/C++ header (`.h`/`.cpp`) separation brings.

## The external interface: `public`

- `public` is Co's **only visibility keyword** — there is no `private`, and no separate `export`. It serves two roles at once: marking the **module interface** (importable by other modules) and **dynamic-library export** (written to the DLL export table).
- A definition without `public` is visible only inside its module; only a `public` definition is externally visible and importable.
- `public` may modify top-level functions, global variables, and `struct` / `union` / `enum` types (cross-module type visibility has been available since 0.3.0/0.3.1), as well as **file-scope `typedef`** (`public typedef int MyInt;`, part of the export surface since 0.4.1.40; a non-`public` `typedef` is not exported); `public` on `class` and its members has been available since 0.4.0 (cross-module `import` consumption of a `public class` is still not opened).
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen — references within the same module resolve, but no externally-visible LLVM value is emitted.

## Same-name tags: warning + qualified type names

When two or more imported modules export a **same-named `public` tag** (`struct` / `union` / `enum` / `class`), a type reference becomes **ambiguous** — same-named types from different modules usually differ in layout, and binding the wrong one is a silent miscompile. Since **0.4.1** the rule has three stages:

1. **Unconditional warning on the definition side**: `tag 'Leaf' is exported by multiple imported modules (leaf_a.Leaf, leaf_b.Leaf); unqualified references are an error, use a qualified name`. It does not care whether the layouts match, nor whether the name is actually used — same name is enough (compilation proceeds).
2. **Bare names rejected on the use side**: a host bare-name reference to that tag (declaration / signature / member type / cast / `sizeof` / `_Alignof` / `typeof` / `new` / `Tag::static`) → `reference to 'Leaf' is ambiguous ...; use a qualified name`. **Not an error when the host defines a same-named type itself** (the local definition wins, same rule as functions/globals).
3. **Qualified type names as the escape hatch**: write `a.Leaf` / `b.Leaf` (`a` is the `as` alias, or the filename stem without an alias) to bind each module's own definition:

```co
b.Leaf l;                 // qualified names work in type-name positions
l.y = 7;
sizeof(a.Leaf);           // 4 (a's definition)
sizeof(b.Leaf);           // 16 (b's definition)
```

Internally the qualified name flows as a **module-identity-bearing tag key** (e.g. `a.Leaf`), so every existing tag-keyed path — member tables, completeness checks, size/alignment, layout — hits the right definition unchanged. It is therefore not merely a diagnostic but the binding mechanism that actually removes the miscompile.

For the full syntax (including qualified enum constants `p.RED`, qualified class statics `c.Counter::total`, and qualified `typedef` `a.MyInt` — only `public typedef` is exported), see "Same-Name Tags" in the import chapter.

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

## Top-level initialization carrier

Top-level code needs an entry point to actually run. Different artifact forms use different carriers (since **0.3.2**):

| Artifact | Carrier | Behavior |
| --- | --- | --- |
| Console exe | synthesized `main` | calls each module's initializer in topological order, then runs the main source's top-level statements |
| GUI exe (`WinMain` entry) | synthesized `WinMain` wrapper | the user's `WinMain` is renamed `__co_WinMain__`; the synthesized `WinMain` runs top-level init/statements first, then forwards (top-level code and `WinMain` are no longer mutually exclusive) |
| DLL (`-o *.dll`) | synthesized `DllMain` | runs top-level initialization under `DLL_PROCESS_ATTACH` and returns `TRUE` for other reasons; a user-authored `DllMain` is renamed `__co_DllMain__` and forwarded |

So a `.co` dynamic library still initializes when it is loaded, even if it has only a runtime-initialized global and no top-level statements — a dependent host need not call any explicit "init" function.

## File-scope `static` alongside "omit `public`"

Since 0.3.0:
- **`public`** controls cross-module visibility — omitting it means "module-internal", adding it makes a symbol importable;
- **`static`** keeps its C meaning — internal linkage within its own translation unit, orthogonal to `public` (`static public` / `public static` in either order are legal; the symbol is still internal to the TU despite the export mark);
- In the target specification, `static` is only a class static-member modifier (landed since 0.4.0 `class`).

## Not yet finalized

Deferred to later milestones:

- The declaration-metadata carrier that lets `.obj` / `.dll` / `.so` become **importable** modules (note: *producing* a DLL and *consuming* a DLL-as-module are two different things — the former landed with the 0.3.2 carriers);
- User-authored `__init__` customization.

## Currently implemented boundary (0.4.1)

- **Module boundary**: one `.co` file = one module; module key = canonical absolute path; same-name files in different directories are distinguished by their paths.
- **Independent compilation closures** (since 0.3.2): each module builds its own type/symbol tables and is analyzed on its own before merging; same-name different-layout `struct` types never hijack each other; `#include`-brought environment symbols do not enter the export surface (ambient cut-off).
- **`public`**: usable on top-level functions and global variables; when the artifact is a dynamic library the symbol is written into the export table (`DLLExportStorageClass`). In this batch `public` **does not change linkage** (non-`public` symbols remain `ExternalLinkage` by default, but non-`public` imported symbols are stripped to `InternalLinkage` by codegen).
- **`static public`** in either order is legal; semantics match P211.
- **Dynamic-library artifacts**: there is no `-shared` option, but the compiler switches between DLL and exe link modes automatically from the `-o` output extension. Naming the output `*.dll` produces an export-table-bearing dynamic library (an import library `*.lib` is generated alongside, verified on Windows).
- **Multi-file programs**: compose several `.co` sources with `import` (available since 0.3.0). The single `.obj` from `-c` is useful for inspecting the compile result; linking several object files directly into an executable is still under development (see the Command Line chapter).
- **Top-level execution model**: see the "import (Module Import)" chapter; this batch covers only `.co` consumption.
- **Cross-module type references**: a `public struct` / `union` / `enum` imported from a module is usable as a value type (member access, address-of for passing, and `sizeof` all work); the consumer may also use the tag **with the keyword omitted** (`Point p;`, since 0.3.1). **Main source and imported modules behave identically** (since 0.4.1): instantiating a directly imported type by value inside a submodule works too, where previously only pointers and file-scope fields were supported.
- **Same-name tag adjudication** (since 0.4.1): two or more modules exporting a same-named `public` tag → an **unconditional warning** on the definition side; a host **bare-name reference is an error** (switch to the qualified type name `mod.Tag`); **qualified type names** work in declarations, the `struct` keyword form, casts, `sizeof`/`_Alignof`/`typeof`, and `new`, with `struct`/`union`/`enum`/`class` handled alike. The old behaviour (later import wins; the other layout's fields folded onto slot 0) is retired.
- **Cross-module compile-time constants**: a `public const int` from an imported module is usable in the consumer as a read-only value, an array dimension, or a `switch` `case` label (since 0.3.1; the `WM_*` series in `winapi.co` is exactly this use).
