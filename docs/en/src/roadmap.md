# Development Roadmap

colang (the Co language compiler) follows a staged route: **first fully implement a C compiler, then tailor and extend it into the Co language**.

## Version Policy

Versions follow `MAJOR.MINOR.PATCH`:

- **0.\*** — Development and testing. Features and syntax may change; no cross-version compatibility guarantee.
- **1.\*** — Official release. Public contracts are frozen; backward compatibility is guaranteed.
- **2.\*** — Bootstrapping. The compiler itself rewritten in Co.

Within 0.\*, **MINOR** bumps denote milestones (a key Co-layer capability). **PATCH** bumps are small Co-layer prerequisites or C-base fixes between two milestones.

## 0.\* Milestones

| Version | Theme                             | Status   |
| ------- | --------------------------------- | -------- |
| 0.1.0   | C compiler                        | Released |
| 0.2.0   | Co entry model + `public`         | Released |
| 0.3.0   | Linking                           | Released |
| 0.4.0   | `class`                           | Planned  |

Details for 1.\* / 2.\* will be finalized by the maintainer before 0.\* wraps up.

## Released

### 0.1.0 — C compiler

- Full C17 coverage plus key C23 features (`_BitInt`, `nullptr`, `auto`, `bool` following C++ semantics, attribute syntax, etc.).
- Hand-made `colib` standard headers to avoid cross-ABI dependencies.
- Passes the Fujitsu `c-testsuite` conformance test suite.
- Layered compiler structure: lexer → preprocessor → parser → sema → LLVM codegen, producing a single `.bc` that is handed to an external linker.

### 0.2.0 — Co entry model + `public`

- Top-level statements execute in source order; a user `main` is not required (the compiler synthesizes the entry).
- Coexistence of a user-written `main` and the synthesized entry; top-level `return V;` sets the process exit code.
- The `public` keyword marks exported symbols; `-o *.dll` / `-o *.so` selects the output kind by file extension.
- Three-way entry rule: bare statements present → normal execution; only `main` → forward without warning; neither → warn and emit an empty `ret 0`.

### 0.3.0 — Linking

First delivery covers `import` (`.co` source merge) and the **linking core** (checked items below); unchecked items roll to later milestones.

- [x] `import` (`.co` source merge): syntax `import * | name [as alias], … from ("file" | module_name);`; consumes `.co` sources only, `public` being the **sole** cross-module visibility gate; top-level statements of imported modules run once at first load, unified before the main entry in topological order; path dedup + DFS cycle detection reporting the full chain; identifier form `from a` equals `from "a.co"` (same directory only).
- [x] The compiler links on its own: `-c` emits a COFF `.obj`, and the default action links the object into an executable — **no dependency on an external `clang`**.
- [x] `-o <name>.dll` produces a Windows dynamic library: with an export table, and a `.lib` import library generated alongside the base name.
- [x] `[[lib("user32")]]` declares library attribution, with no command-line flag needed.
- [x] Static CRT linked by default: the produced exe ships without requiring the VC runtime to be installed.
- [ ] Multi-module `.obj` merging, `.dll` / `.lib` consumption, static library output.
- [ ] `import` extended to consume `.obj` / `.dll` / `.lib` as importable modules (bundling compile-time declaration metadata).
- [ ] Module-alias-limited access: `m.f()`, `m.x`.
- [ ] Refinement of `DllMain` synthesis and top-level body triggering for DLL outputs.
- [ ] User-visible `argv` (shipped with array-with-length infrastructure).
- [ ] `winapi.co`: Co-modular wrapping of the Win32 API (declarations and wrapping unified inside a `.co` module, replacing `windows.h`).

## Planned

### 0.4.0 — class

- Reference type `class` with `new` instantiation and `this` member access.
- Four lifecycle protocols: `__new__` / `__retain__` / `__release__` / `__delete__`.
- Automatic reference counting and heap-object reclamation; out-of-scope auto-free.
- `public` extended to class members.
- Static class members accessed via the class name with double colon (`C::m()`).

## Terminology

- Internal development documents use "milestone".
- Public-facing manuals (this one) use "release version".
- Feature wording and error messages reflect actual compiler behavior.
