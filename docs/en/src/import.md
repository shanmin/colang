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
| `ModAlias` | A local alias for the source module (**syntax slot is parsed but not yet active** in this batch; qualified access `m.f()` is deferred to later milestones) |

## Filename and Path Resolution

- **Two forms** — pick either `"file"` or `module_name`:
  - String literal: allows cross-directory (relative or absolute) paths; the extension may be omitted (`.co` is auto-appended when missing).
  - Simplified identifier: equivalent to `from "<name>.co"`, resolved **in the importer's directory**.
- **Base directory**: relative paths are resolved against **the directory containing the importing file**, not the current working directory.

## Visibility Rules

- **`public` is the sole cross-module visibility gate**: definitions without `public` are module-internal and cannot be reached by any `import`.
- Named-import of a non-`public` symbol: compile-time error `cannot import non-public symbol`.
- Non-`public` symbols in a module reached via `import *`: silently skipped — only the `public` surface is imported.
- Referring (from the main source) to a symbol that exists but was not brought in via `import`: compile-time error `identifier not imported`.
- A module referring to **its own** non-`public` symbols: allowed, unrestricted.

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

## Current Implementation Boundary (0.3.1)

Implemented:
- Full `.co` consumption pipeline (lexing / parsing / loading / merging / visibility enforcement / linkage strip / entry-model integration);
- Both named and `*` forms; the `as ModAlias` syntax slot is parsed but its qualified-access semantics are not yet active;
- Centralized topological execution with first-load semantics; deduplication; cycle detection reporting the full chain;
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen;
- Cross-module `public struct` / `union` / `enum` type references (usable as value-typed variables, member access, and address-of for passing);
- Cross-module **bare tags**: a `public` `struct`/`union`/`enum` from an imported module may be referenced with the keyword omitted (`Point p;`; see the "struct / union / enum" chapter);
- Cross-module **compile-time constants**: a `public const int` from an imported module (e.g. the `WM_*` series in `winapi.co`) is usable in the consumer as a plain read-only value, an array dimension, or a `switch` `case` label; a non-constant label is rejected with `case label does not reduce to an integer constant`.

Not covered in this batch — deferred to later milestones:
- Module-alias qualified access `m.f()` / `m.x` (parsed but not yet wired into Sema/Codegen);
- `.obj` / `.dll` / `.so` as importable modules (they must carry compile-time declaration metadata);
- `DllMain` synthesis and top-level body triggering under DLL outputs.
