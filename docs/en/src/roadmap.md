# Development Roadmap

colang (the Co language compiler) follows a staged route: **first fully implement a C compiler, then tailor and extend it into the Co language**.

## Version Policy

Versions follow `MAJOR.MINOR.PATCH`:

- **0.\*** — Development and testing. Features and syntax may change; no cross-version compatibility guarantee.
- **1.\*** — Official release. Public contracts are frozen; backward compatibility is guaranteed.
- **2.\*** — Bootstrapping. The compiler itself rewritten in Co.
- **3.\*** — Product finalization. Beyond bootstrapping, converged into a final, production-ready stable form.

Within 0.\*, **MINOR** bumps denote milestones (a key Co-layer capability). **PATCH** bumps are small Co-layer prerequisites or C-base fixes between two milestones.

## 0.\* Milestones

| Version | Theme                             | Status   |
| ------- | --------------------------------- | -------- |
| 0.1.0   | C compiler                        | Released |
| 0.2.0   | Co entry model + `public`         | Released |
| 0.3.0   | Linking                           | Released |
| 0.3.1   | Windows GUI & `winapi.co`         | Released |
| 0.3.2   | Linux native ELF toolchain        | In progress |
| 0.4.0   | `class`                           | Planned  |

Details for 1.\* / 2.\* / 3.\* will be finalized by the maintainer before 0.\* wraps up.

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

### 0.3.1 — Windows GUI & winapi.co

- Windows GUI subsystem and `WinMain` entry: `[[subsystem("windows")]]` or defining `WinMain` switches the artifact to a console-less GUI program; `WinMain` is mutually exclusive with top-level bare statements.
- `winapi.co` replaces `windows.h`: 215 prototypes / 13 structs / 266 constants (SDK-verified), library attribution via `[[lib(...)]]`; a practical subset of programmatic GUI APIs (window, message loop, memory-DC drawing, clipping, text metrics, system colors).
- `const` consistency: read-only enforcement (diagnostics matching clang); `public const int` values flow across modules into array dimensions and `switch` `case` labels, with non-constant labels now rejected instead of silently folded to 0.
- First batch of attribute semantics: `[[nodiscard]]` / `[[noreturn]]` in effect; `[[deprecated]]` same-position duplicates accepted first-wins.
- C-conformance batch: designated initializers across the chain (chained designators, promoted anonymous aggregates, `union` hosts, mask-merged bit-field initialization, DFS continuation), `sizeof`/`_Alignof` in constant contexts, universal character names (`\uXXXX`/`\UXXXXXXXX`) in identifiers and literals, prefixed character constant typing, `_Alignas` context constraints and cross-declaration consistency, `static_assert` in more scopes.
- Diagnostics quality: `typedef` alias spelling restoration, column alignment with clang, up-front diagnostics for by-value allocation of incomplete types.
- Flat single-level release package: `colang.exe + include/` (standard headers located next to the compiler, no `-I` needed).
- Linux (WSL) build brought up: the colang compiler itself builds and runs its regression suite natively on WSL2/Debian (linking at this stage still relies on an external toolchain).

## Planned

### 0.3.2 — Linux native ELF toolchain (in progress)

- A single target/data-model abstraction (`__LP64__`/`__LLP64__`) drives predefined macros, type widths, code generation and linking, eliminating the front-end/back-end data-model split.
- On Linux an in-process `lld::elf` **statically** links the local glibc/libgcc into a native ELF executable (no external linker); `crt1.o`/`libc.a`/`libm.a`/`libgcc` are located automatically.
- `--target <triple>` cross-emits ELF object files and IR.
- Native colang build on Linux (LLVM 23.1 built from source, X86+lld); the plct matrix is being aligned to the Windows baseline.
- Follow-ups (long tail): `.so` shared objects, dynamic PIE, x86-64 SysV ABI edge cases, ELF mapping of `winapi.co`, glibc audit of all stdlib headers.

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
