# import (Module Import)

> **A planned feature — not yet available in the current version.** This chapter only records the design direction for `import` in the language specification; it does not mean the installed compiler supports it. colang is currently in the "C compilation only" stage and `import` is not implemented; rely on the actual behavior only once later versions deliver it.

## Purpose

As envisioned by the specification, `import` brings names in from other modules (files), to support organizing large projects and interface visibility, and to avoid the duplicate-definition and synchronization costs that C/C++ header separation brings.

## Design direction (specification text)

`Co程序设计` gives `import` a single syntax line, with no accompanying semantic rules yet. Its "import set" has two **mutually exclusive** forms — **list several named items** (comma-separated, each optionally aliased) or **use `*` to import all**; the specification writes them on one line separated by `|`, and they are split into two lines here for readability, with the meaning unchanged:

```
import name [as aliasname], … from filename [as aliasname];
import * from filename [as aliasname];
```

Meaning of each part:

| Part | Meaning |
| --- | --- |
| `name` | the name to import |
| `name as aliasname` | an alias for the imported name |
| `…, …` | more `name [as aliasname]` items may follow after a comma |
| `*` | import every importable name from the source |
| `filename` | the name (optionally a path) of the source module |
| `filename as aliasname` | an alias for the source module |

## Why there are no examples here

The following key points about `import` are **not yet finalized** in the specification, and any example would amount to committing to unratified syntax, so this chapter deliberately leaves them blank:

- the visibility rules for imported names (whether the exporting side must explicitly mark a name as externally visible, and whether bare names may be used across modules);
- when and how many times top-level code runs during cross-module loading;
- how module names and paths are resolved, and how dependency cycles are handled;
- the interaction boundary with `public` / dynamic-library export.

These will be filled in once `import` is implemented in the current-generation compiler and its semantics are finalized together.

## Current status

- The installed compiler does not recognize `import`: it treats an `import … from …` line in a source file as an unknown token at file scope, reports `expected type specifier, got 'import'`, and stops — no module import takes place.
- For multi-file programs for now, use the compile-time approach (`-c` + link; see "Multi-file programs" in the "Command Line" chapter for the exact steps).
