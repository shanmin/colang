# import (Module Import)

> As of **0.3.0**, `import` is implemented for consuming `.co` source-file modules. Using `.obj` / `.dll` / `.so` as importable modules is deferred to later milestones.

## Purpose

`import` brings names in from other `.co` source files, to organize large projects and delineate interface boundaries — avoiding the duplicate-definition and synchronization costs that C/C++ header separation brings.

## Syntax

Two **mutually exclusive** forms:

```
import name [as alias], … from ("file" | module_name) [as ModAlias];
import * from ("file" | module_name) [as ModAlias];
```

| Element | Meaning |
| --- | --- |
| `name` | A specific symbol to import (must be marked `public` in the source module) |
| `name as alias` | A local alias for the imported name |
| `…, …` | Additional `name [as alias]` items separated by commas |
| `*` | Import every `public` name from the source module |
| `"file"` | String-literal form; supports relative paths / cross-directory / special characters |
| `module_name` | Simplified identifier (no quotes, no extension, no separators); resolved only in the same directory |
| `ModAlias` | A local alias for the source module, used for **qualified access** `ModAlias.f()` / `ModAlias.x` (implemented since **0.3.2**); when omitted, the module's filename stem is the default qualifier |

## Filename and Path Resolution

- **Two forms** — pick either `"file"` or `module_name`:
  - String literal: allows cross-directory (relative or absolute) paths; the extension may be omitted (`.co` is auto-appended when missing).
  - Simplified identifier: equivalent to `from "<name>.co"`, resolved **in the importer's directory**.
- **Base directory**: relative paths are resolved against **the directory containing the importing file**, not the current working directory.

## Independent Module Closures

Since **0.3.2**, every `.co` module is an **independent compilation closure**: each module builds its own type and symbol tables and is analyzed on its own; the main source sees a module only through its **export surface** (its `public` names). This yields two user-visible guarantees:

- **Same-name, different-layout types never interfere**: two modules may each define a `struct Config` with a different layout, and each `sizeof` uses its own layout — there is no "last importer wins" hijack.
- **Ambient cut-off**: **ambient symbols** brought in by a module's `#include` (e.g. `<stdio.h>`'s `printf`) **do not enter the module's export surface**. A host that also `#include <stdio.h>` has its `printf` unconflicted with the module's — no more "import non-public symbol" error.

## Visibility Rules

- **`public` is the sole cross-module visibility gate**: definitions without `public` are module-internal and cannot be reached by any `import`.
- Named-import of a non-`public` symbol: compile-time error `cannot import non-public symbol`.
- Non-`public` symbols in a module reached via `import *`: silently skipped — only the `public` surface is imported.
- Qualified access `ModAlias.x` hitting a missing or non-`public` name in the module: error `no public symbol 'x' in module 'ModAlias'`.
- Referring (from the main source) to a module symbol that exists but is **neither named-imported nor reached via a qualifier**: compile-time error `identifier not imported`.
- A module referring to **its own** non-`public` symbols: allowed, unrestricted.

## Module-Alias Qualified Access `m.f()` / `m.x`

After importing a module you can reach its `public` members directly with `m.name`, without naming each one in an import:

- **Default qualifier** = the module filename without extension (importing `counter.co` → `counter.bump(3)` / `counter.total`).
  - If the filename is not a valid identifier (contains `-`, `.`, …), you **must** give an explicit alias with `as ModAlias`.
- **Alias override**: `import * from "counter.co" as CNT;` → `CNT.bump(3)` / `CNT.total`.
- **What you can reach**: `m.f(...)` (call), `m.f` (function reference), `m.x` (global read), `m.x = v` (global write).
- **Write parity with locals**: `m.x = v` behaves like assigning a local variable; the only exception is writing a `const` global (`m.constGlobal = v`), rejected with `read-only variable is not assignable`.
- Qualified access only reaches `public` members; non-`public` always errors `no public symbol`.

```co
import * from "counter.co" as CNT;   // counter.co: public int total; public int bump(int n){...}
CNT.total = 5;                        // qualified write to a global
int a = CNT.bump(3);                  // qualified call → a == 8
return CNT.total == 8 && a == 8 ? 0 : 1;
```

## Name-Clash Adjudication

When multiple modules export the **same name** to the main source, the following rules apply (`m` = qualifier/alias; *bare name* = an unqualified reference):

1. **Explicit named import vs a local of the same name** → error (two explicit sources for one bare name).
2. **`import *` wildcard vs a local of the same name** → **the bare name belongs to the local**; the wildcard entry is shadowed (a shadow warning is emitted). The wildcard carries only the `public` surface; non-`public` is silently excluded.
3. **Two wildcard modules share a name** → the **bare name is ambiguous and disabled** (a reference reports `identifier not imported`-style), but `m1.name` / `m2.name` are **each accessible via their qualifier and bind to their own definition**.
4. **Default qualifier = filename stem**, overridable by `as ModAlias`; a stem that is not a valid identifier requires an explicit alias.
5. **Two explicit named imports of the same name from different modules** → error.
6. **Minimal export surface**: a module's `public` acts as a visibility gate when merged into the host; for exe outputs it is not individually placed in a DLL export table.
7. **Functions and globals follow the same rules**: `m.f` and `m.x` share the resolution / shadowing / ambiguity trio.
8. **Qualified write parity**: there is no "cross-module read-only" gate; `m.x = v` matches a local write (only `const` global writes are intercepted, see above).

> **Cross-module same-name coexistence**: rule ③ lets two modules each define `get` without overwriting one another — at link time they coexist under synthesized symbol names (`__<module-stem>__<name>`), and each module's internal call plus `m1.get()` / `m2.get()` binds back to its own definition.

## Execution Model of Top-Level Statements

If an imported module contains top-level statements (statements outside any function or type definition), their execution follows:

- **Exactly once at first load**: no matter how many times a module is imported, its top-level code runs only once.
- **Centralized topological order**: all imported modules' top-level statements are emitted **before the main entry**, in DFS-postorder of the dependency closure (dependencies run before dependents). The physical position of `import` lines in the main source has no effect on this order.
- **`main` of a non-main module is not a load hook**: if a module defines `main`, it is treated as an ordinary function symbol and does not execute automatically.

## Deduplication and Cycle Detection

- **Deduplication**: the canonical absolute path is the unique key (case-insensitive on Windows); a module imported from multiple sites is loaded and merged only once.
- **Cycle detection**: the loader performs DFS over the dependency graph; on detecting a cycle, it reports the full chain (e.g. `cycle detected: a.co -> b.co -> a.co`) and aborts compilation.

## Example

**Main source `main.co`**:

```
import add, wrapper from "mod.co";
if (add(3, 4) != 7) return 1;
if (wrapper(2) != 7) return 2;
return 0;
```

**Imported module `mod.co`**:

```
static int helper(int x) { return x * 3; }
public int add(int a, int b) { return a + b; }
public int wrapper(int x) { return helper(x) + 1; }
printf("mod initialized\n");
```

Compilation and execution: `mod initialized` prints first (top-level statements emitted in centralized topological order), then the main source's top-level statements execute in source order; the exit code is set by a top-level `return`.

## Current Implementation Boundary (0.3.2)

Implemented:
- Full `.co` consumption pipeline (lexing / parsing / loading / **per-module closure analysis** / merging / visibility enforcement / linkage strip / entry-model integration);
- Both named and `*` forms; `import … as ModAlias` **enables qualified access**;
- **Module-alias qualified access** `m.f()` / `m.f` / `m.x` / `m.x = v` (with local write parity and `const` global write interception);
- **Independent module closures**: per-module type/symbol isolation, same-name different-layout `struct` types never hijack each other;
- **Ambient cut-off**: environment symbols brought in by a module's `#include` do not enter its export surface;
- **Name-clash adjudication ①~⑧**: shadowing / ambiguity / cross-module same-name coexistence (synthesized symbol names `__<stem>__<name>` at link time);
- Centralized topological execution with first-load semantics; deduplication; cycle detection reporting the full chain;
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen;
- Cross-module `public struct` / `union` / `enum` type references (usable as value-typed variables, member access, and address-of for passing);
- Cross-module **bare tags**: a `public` `struct`/`union`/`enum` from an imported module may be referenced with the keyword omitted (`Point p;`);
- Cross-module **compile-time constants**: a `public const int` from an imported module (e.g. the `WM_*` series in `winapi.co`) is usable in the consumer as a plain read-only value, an array dimension, or a `switch` `case` label;
- **DLL / WinMain top-level-init carriers**: producing `-o *.dll` synthesizes a `DllMain` that runs each module's top-level initialization under `DLL_PROCESS_ATTACH` (a user-authored `DllMain` is renamed and forwarded); under a GUI-exe `WinMain` entry, top-level statements and runtime-initialized globals are carried by a synthesized `WinMain` wrapper (see the "Module" chapter).

Reserved names: `__init__*__` is a reserved prefix for the module-init dispatch protocol and **cannot be defined by user code** (compile-time error `reserved protocol name '__init__*' cannot be defined by user code`).

Not covered in this batch — deferred to later milestones:
- `.obj` / `.dll` / `.so` as importable modules (they must carry compile-time declaration metadata);
- User-authored `__init__` customization.
