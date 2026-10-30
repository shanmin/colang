# import (Module Import)

> As of **0.3.0**, `import` is implemented for consuming `.co` source-file modules. Using `.obj` / `.dll` / `.so` as importable modules is deferred to later milestones.
>
> **Two hard limits — statement location and target**: ① the `import` statement itself **may only be written in `.co`/`.CO` source files**; in `.c` or any other source file it is an error (see "Who may write `import`"); ② the import **target** may only be a `.co`/`.CO` module — `.c`/`.h` are permanently rejected and `.bc` is planned but not implemented (see "Import Target Suffix Whitelist").

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

## Who may write `import`: `.co` source files only

`import` is Co-surface syntax and may appear only in `.co`/`.CO` source files:

- **`import` in a `.c` file**: compile-time error `'import' is only allowed in .co source files` — it is neither silently accepted nor downgraded to "recognized but not loaded".
- **The check follows the file the statement actually appears in**: if a header `#include`d by a `.co` contains `import`, that is an error too, and the diagnostic points at the line and column **inside that header** — a `.h` is always a C-surface file, never relaxed because a `.co` includes it.
- **A pure C translation unit (`.c`) has only `#include` for reaching declarations across files**, and a `.c` cannot consume any `.co` module (including `winapi.co`).

## Import Target Suffix Whitelist

The import target may only be `.co`/`.CO`:

| Target spelling | Status | Diagnostic / behavior |
| --- | --- | --- |
| `"mod.co"` / `"mod.CO"` | ✅ usable | — |
| `"mod"` (no suffix) | ✅ `.co` appended automatically | — |
| `"mod.bc"` | ⏳ planned, not implemented | `.bc modules are planned but not implemented yet` |
| `"mod.dll"` / `".lib"` / `".so"` | ⏳ slot reserved for link-time consumption | `binary library import (.dll/.lib/.so) is not supported yet` |
| `"mod.c"` / `"mod.h"` | ❌ permanently rejected | `must be a .co/.bc module; C source files and headers cannot be imported (use #include)` |
| other suffixes (e.g. `.txt`, `.obj`) | ❌ rejected | `must be a .co/.bc module (unsupported extension '…')` |

The two target classes differ in nature and must not be conflated: `.co` is **source merging** (it supplies types, prototypes and definitions), whereas a future `.bc` / `.dll` / `.lib` / `.so` is **link-time consumption** (symbols only, no compile-time declarations, hence unable to stand in for a declaration source). The whitelist applies equally to **the main source and every imported module** — submodules get no exemption.

## Independent Module Closures

Since **0.3.2**, every `.co` module is an **independent compilation closure**: each module builds its own type and symbol tables and is analyzed on its own; the main source sees a module only through its **export surface** (its `public` names). This yields two user-visible guarantees:

- **Same-name, different-layout types never interfere**: two modules may each define a `struct Config` with a different layout, and each `sizeof` uses its own layout — there is no "last importer wins" hijack.
- **Ambient cut-off**: **ambient symbols** brought in by a module's `#include` (e.g. `<stdio.h>`'s `printf`) **do not enter the module's export surface**. A host that also `#include <stdio.h>` has its `printf` unconflicted with the module's — no more "import non-public symbol" error.

## Visibility Rules

- **`public` is the sole cross-module visibility gate**: definitions without `public` are module-internal and cannot be reached by any `import`.
- Named-import of a non-`public` symbol: compile-time error `cannot import non-public symbol`.
- Non-`public` symbols in a module reached via `import *`: silently skipped — only the `public` surface is imported.
- Qualified access `ModAlias.x` hitting a missing or non-`public` name in the module: error `no public symbol 'x' in module 'ModAlias'`; if that name is a **type** in the module (a non-`public` `struct`/`enum`/`class`), the diagnostic uses type wording instead: `no public type 'X' in module 'ModAlias'`.
- Referring (from the main source) to a module symbol that exists but is **neither named-imported nor reached via a qualifier**: compile-time error `identifier not imported`.
- A module referring to **its own** non-`public` symbols: allowed, unrestricted.

## Main Source and Submodules Alike: Where Imported Types/Constants Are Usable

Since **0.4.1**, imported `public` `struct`/`union`/`enum` types and `public const` values are usable in **exactly the same ways in the main source and in any imported module** — the module's depth in the import graph does not matter:

- **By value**: declaring local variables, function parameters/return types, struct fields, taking addresses, member access, `sizeof`;
- **Bare tags**: omitting the `struct`/`union`/`enum` keyword and writing `Leaf l;` directly;
- **Compile-time constants**: as array dimensions and `switch` `case` labels.

That is, if `mid.co` does `import * from leaf;`, then inside `mid.co` you may instantiate `Leaf l;` by value and access its members just like in the main source. **This did not hold before 0.4.1** — tag/constant seeds from imported modules were injected only into the main source, so by-value instantiation inside a submodule degraded to an incomplete type (`IR verification: Cannot allocate unsized type`, and a member access crashed the process); only pointer and file-scope field positions worked. The restriction is removed in 0.4.1 (each submodule is loaded with seeds drawn from **its own direct dependencies**, through the same pipeline as the main source).

```co
// leaf.co
public struct Leaf { int a; int b; };

// mid.co — a submodule that uses the imported type by value
import * from leaf;

public int leaf_sum(void)
{
    Leaf l;            // direct instantiation (failed before 0.4.1)
    l.a = 1;
    l.b = 2;
    return l.a + l.b;
}
```

> **Same-name tags no longer hijack silently (since 0.4.1)**: when one file imports two modules that both export `public` a **same-named tag**, the compiler emits a **warning**; if the host then refers to that tag by its bare name it is an **error**, requiring a qualified name (`a.Leaf` / `b.Leaf`). See "Same-Name Tags: Warning + Qualified Type Names" below.

## Same-Name Tags: Warning + Qualified Type Names

Type names share the cross-module same-name problem with functions/globals, but a type also carries a **layout** — binding the wrong one is a silent miscompile. Since **0.4.1** this is handled in three stages: warn on the definition side, reject bare names on the use side, and provide qualified names as the escape hatch.

**① Definition side (unconditional warning)**: as soon as **two or more** imported modules export a same-named `public` tag (`struct` / `union` / `enum` / `class` alike), compiling the host emits one warning listing every candidate:

```text
leaf_a.co:1:22: warning: tag 'Leaf' is exported by multiple imported modules (leaf_a.Leaf, leaf_b.Leaf);
              unqualified references are an error, use a qualified name
```

The warning **does not care whether the layouts match, nor whether the host actually uses the bare name** — same name is enough. It is informational and does not affect compilation or linking. Candidates are listed as **names you can actually write**: the `as ModAlias` name when given, otherwise the filename (so example ① below, imported without an alias, reports `leaf_a.Leaf`, while example ② with `a`/`b` reports `a.Leaf`).

**② Use side (bare name is an error)**: if the host (main source) refers to that tag by its **bare name**, it is an error naming the candidate qualifiers:

```text
main.co:3:23: error: reference to 'Leaf' is ambiguous (defined in multiple imported modules:
               a.Leaf, b.Leaf); use a qualified name
```

It covers all type reference sites: local/global declarations, function signatures, member types, casts `(Leaf)x`, `sizeof(Leaf)` / `_Alignof(Leaf)`, `typeof(Leaf)`, `new Leaf()`, `Leaf::static`. Two cases are **not** errors:

- **the host defines a same-named type itself** → the host wins (same rule as local shadowing for functions/globals);
- **only one module exports that tag** → the bare name keeps working, no qualifier needed.

**③ Qualified type names `mod.Tag`**: `mod` is the `as ModAlias` name, or the filename without extension when no alias is given. Accepted in every type-name position:

```co
import * from leaf_a as a;      // public struct Leaf { int a; };
import * from leaf_b as b;      // public struct Leaf { char *s; int y; };

b.Leaf l;                       // declaration: uses leaf_b's 16-byte layout
l.s = nullptr;
l.y = 7;
if (sizeof(a.Leaf) != 4) return 1;   // sizeof: each binds its own layout
if (sizeof(b.Leaf) != 16) return 2;
a.Leaf* p = (a.Leaf*)0;              // pointer / cast
```

The keyword form `struct a.Leaf l;` works too, and `union` / `enum` / `class` are structurally identical (`a.Slot` / `a.Color` / `new a.Box()`). Bare references **inside a module** (including an imported module itself) are unaffected — a bare name inside a module always binds that module's own definition.

**④ Qualified enum constants `mod.RED`**: after functions/globals, `mod.X` also consults the imported module's `public enum` constant table; a hit folds to an integer constant of the same value (same semantics as the bare `RED`, zero runtime cost):

```co
// palette.co
public enum Color { RED, GREEN = 5, BLUE };

// main.co
import * from palette as p;

if (p.RED != 0) return 1;        // 0
if (p.BLUE != 6) return 2;       // GREEN = 5 -> BLUE = 6
```

> Since **0.4.1.47**, **compile-time constant positions** recognise the qualified form as well: `case p.GREEN:`, `int arr[p.BLUE];`, `enum { E = p.GREEN };` and `int g = p.GREEN;` (including file-scope and `static` initializers) all fold to the same integer constant.
>
> Still not available: **bare** enumeration constants do not cross modules (writing `GREEN` in `main.co` after the `import` still reports `use of undeclared identifier`) — for a cross-module integer constant use `public const` (see "Cross-module compile-time constants").

**④-add File-scope initializers must be constant expressions (rejected since 0.4.1.48)**: using a global that is **neither `public const` nor an enumeration constant** as a file-scope initializer — whether a plain `int` in the same file or a cross-module `public int` — no longer "compiles with a value of 0"; it is now a hard compile-time error whose wording matches clang `-std=c23` verbatim, pointing at the referenced variable:

```co
// main.co
import * from cfg as p;      // public int M = 5;  public const int K = 7;  public enum Size { DIM, WIDE };

int g = p.M;                 // since 0.4.1.48: error: initializer element is not a compile-time constant
int h = M;                   // the bare name (brought in by `import *`) is rejected too; so is a plain `int M` written in the same file
int ok = p.K;                // const global: folds to the real value (since 0.4.1.46)
int e = p.DIM;               // qualified enum constant: folds (since 0.4.1.47)
int *q = &p.M;               // address-of: an address constant, unaffected
```

Compound forms are rejected the same way (`M + 1`, `-M`, `(int)M`, `double d = M`), and the qualified compound forms are rejected as well (`-p.M`, `(int)p.M`, `double d = p.M`, `p.M + 1` each produce one diagnostic). Four points must be known:

- **Plain C files (`.c`) are covered too** — C §6.7.9p4 already requires a static-storage-duration initializer to be a constant expression; this only turns the silent fold-to-0 into a rejection;
- **No error when a runtime init carrier exists**: if the `.co` has top-level statements (or a `*.dll` / `WinMain` carrier is produced), an unfoldable initializer is stored at runtime by the synthesized carrier, so the same line of code is judged differently in the two shapes;
- **Address-constant forms are unaffected**: `&p.M`, array/function name decay (`int *gp = arr;`, `int (*fp)(int) = add6;`) and `sizeof` do not constitute a *load*, and are still accepted; **address-constant arithmetic** folds to a **real address** since **0.4.1.49** — `arr + 1`, `1 + arr`, `arr - 1`, `&x + 1`, `&arr[0] + 2`, `&arr + 1`, chained `arr + 1 + 1` and the string-literal base `"abc" + 1` all step by the **element size** (`arr + 1` advances by `sizeof(element)`; `&arr + 1` advances by the whole array size) — this family used to compile yet fold to `null`, crashing on the first runtime dereference;
- **Pointer targets are covered too (since 0.4.1.49)**: in `int *o = &M; int *p = o;` the `o` is a *loaded* non-`const` global, clang `-std=c23` rejects it, and colang now rejects it instead of silently folding to a null pointer (the compound form `o + 1`, whose base is a load, is rejected as well). **Since 0.4.1.50 the rejection surface extends to three further families**: non-constant initializers in **aggregate member positions** (`int a[2] = {M, 1};`, `{[0]=M}`, `{{M}, 1}`, `{.in.x = M}`), **dereference and subscript** forms (`*(&M)`, `*o`, `*arr`, `*(arr + 1)`, `arr[0]`, `*(&s.a)`), and **member access** (`oo.y`, `oo.in.x`, `op->in.x`) — each reported on the same line as clang with verbatim wording. **Since 0.4.1.52 the family of explicit pointer-to-integer conversions is rejected as well**: `(long)(arr + 1)`, `(long)arr`, `(long)&M`, `(int)&M`, `(long)(&arr[1])`, `(int)(long)arr`, `(long)add9`, `(long)&K` and `{(long)arr, 1}` in an aggregate member position now error on the same line (in C a pointer-to-integer conversion is never a constant expression, so clang rejects every one of them), while a **same-object pointer difference without a cast** (`arr - arr`) and `(long)(arr - arr)` (whose operand is already an integer) stay accepted. Known gaps still open: a loaded `extern` declaration used as an initializer, and a block-scope `static int s = M;`. **Since 0.4.1.53 array decay through a member folds to a real address**: `oo.buf`, `oo.buf + 2`, `oa[1].buf`, the union member `uu.c` and `{oo.buf, oa[1].buf}` in an aggregate member position no longer fold to `null`; the same batch cures three forms that had been **rejected in error** (colang was stricter than clang, which reports nothing for them) — `int *q = oo.m[1];`, `int *q = oo.m[0] + 2;` and `int *q = g2[1] + 1;` (the subscript form of a member array and the row-decay form of a two-dimensional array), which clang now accepts identically and which fold to real addresses; the step is the width of the decayed element (`oo.buf + 2` steps 1, `oo.m[0] + 2` steps 4). A chain that still contains one *load* (the arrow `op->buf`, the dereference `(*op).buf`, or a subscript on a pointer) is **not** a constant address — **since 0.4.1.57** such forms are rejected line for line with clang, whether the address is taken (`&op->buf[1]`, `&(*op).buf[1]`, `&(op->m[1][2])`) or the value merely decays (`(*op).buf`); before that version colang accepted them and folded a wrong address, treating the pointer variable's own storage as the object base. Taking the address of the pointer variable **itself** (`&op`) is still a valid address constant and remains accepted. **Since 0.4.1.55 the two remaining folding gaps are closed**: a same-object pointer difference now folds to the real element count, computed as *byte distance from the same root global ÷ effective pointee width* — `long d = &arr[1] - &arr[0];` yields 1, `long d = &arr[0] - &arr[2];` yields -2, `&s.b - &s.a` yields the member gap — and the same batch cures `int d8 = g2[1] - g2[0];`, which had been **rejected in error** before that version; the step after a cast is now taken from the **cast pointee** rather than the element width of the base array — `(char *)arr + 1` steps 1 byte, `(short *)arr + 3` steps 2, `(void *)arr + 1` steps 1 (GNU semantics, matching clang). **Since 0.4.1.58 the cross-object under-report is closed**: a pointer difference whose two operands resolve to **distinct root globals** (`other - arr`, `&oo.in.x - &arr[0]`, `&other[1] - &arr[1]`, `&M - &N`, `ex - arr`, `arr + 1 - other`, and `{ other - arr, 0 }` in an aggregate member position), which colang previously accepted while silently yielding 0, is now rejected on the same line as clang (the rejection shares the folding criterion: both sides resolve to a root global, the roots differ, and the step widths are equal and positive); only a difference **within the same object** is a valid form and still folds to a real value. A pointer difference between **incompatible pointee types** is now rejected as well (**since 0.4.1.62**): `sa - arr`, `(short *)arr - arr`, `(long *)arr - (int *)arr`, `(long *)&s.a - (int *)&s.a`, `int (*)[3] - int (*)[4]`, `enum E1 * - enum E2 *` — previously accepted and silently yielding 0 — now produce the sema diagnostic `'short *' and 'int *' are not pointers to compatible types` (C 6.5.9p4) on the same line **and the same column** as clang, anchored at the `-` operator, both for file-scope initializers and inside function bodies. Compatible forms are still accepted and **never falsely rejected**: `int *` ↔ `const int *` (qualification differences), `enum *` ↔ `int *`, the same type in different objects, and array names after decay. Two more items landed in the same batch: a difference between two `void *` operands used to **crash the compiler** (`0xC0000005`) and is now fixed, yielding a value with GNU's **byte**-sized step (`vp - vp2` gives -12; note that `void *` ↔ object pointer remains an incompatible pair and is rejected as clang does); and pointer spellings in diagnostics now match clang (`int (*)[4]` instead of `int[4] *`, `int **` instead of `int * *`, `unsigned char *` instead of `char *`). **Since 0.4.1.63 the `&function` fallback is closed**: `&function` now resolves to the **real function prototype** (`&f` gives `int (*)(void)`), and the diagnostics align with clang accordingly — the difference form `long d = &f - arr;` moved from "same line and column, wrong text" to the same line, column **and text**; a difference between **differently prototyped function names** (`&f - &g`, i.e. `int (*)(void)` ↔ `int (*)(int)`), which used to go completely unreported because both sides degraded to `void *` and were then judged compatible, is now reported; and the assignment form (`int (*p)(void) = &g;`), previously skipped silently because one side was `void *`, now errors with wording verbatim identical to clang (clang anchors at the declarator name and colang at the initializer — a column difference within the established tolerance). The same batch cures a **compiler crash**: the legal C form `(*&f)()` (calling through a dereferenced function pointer), which used to crash colang at compile time, now compiles and runs exactly as clang does. **Since 0.4.1.67 the bare-function-name decay form is closed**: a function name that fails variable lookup now takes the **real function prototype** (the non-decayed function type), and the decay is left to the existing lvalue conversion — the assignment forms (`int (*q)(void) = f;`, `int *ip = f;`) and the difference forms (`f - g`, `f - arr`) now match clang **line for line and verbatim** (the former prints the non-decayed function type, the latter the decayed function pointers; a column difference stays within the established tolerance); `sizeof(function)` follows the GNU extension and equals clang's **1** (the file-scope initializer folding order was fixed in the same batch); C23 `auto pf = f;` / `auto pa = arr;` derive a function pointer / element pointer via the lvalue conversion, and the same batch cures the **regression that falsely rejected `auto p = &f;`** (legal C is no longer an error). **Since 0.4.1.73 implicit pointer ↔ integer conversions are no longer silent**: initializers (`long z = arr;`, `long z = &sobj;`, `long z = arr + 1;`, `long z = str;`, `long z = 1 + arr;`), assignment forms (`z = arr;`), argument forms and block-scope declarations are all covered and match clang **line for line and verbatim** — initializer `incompatible pointer to integer conversion initializing 'long' with an expression of type 'int[4]'` (the right side still prints the non-decayed array type, anchored at the declarator name), assignment `assigning to 'long' from 'int[4]'` (anchored at `=`), argument `passing 'int[4]' to parameter of type 'long'` (source first); the reverse direction reports `incompatible integer to pointer conversion …` (e.g. `int *q = 1;`). Still accepted and never reported: **bool targets** (`bool b = arr;`, where clang only warns with `-Wpointer-bool-conversion`) and **null pointer constants** (`int *p = 0;`, `int *p = 1 - 1;`, `(void *)0` — integer constant expressions equal to 0), plus integer sources whose value cannot be computed (`int *p = x;` remains under-reported). Four **known gaps** remain: `char *` ↔ `signed char *` is not reported (the two are indistinguishable in the type representation); a **pointer-to-floating** conversion prints a different wording (`float f1 = arr;` yields `initializing 'float' with an expression of incompatible type 'int[4]'`, still accepted by colang); an aggregate initializer **element position** (`int (*tbl[2])(void) = { f, g };`) and a `?:` conditional used as an array dimension (`int a[1 == 1 ? 3 : 5];`) are still not reported; and note that the **value side** of `bool b = pointer;` is implemented as a truncation (a non-zero test is unreliable). **Since 0.4.1.75** a **subscript base** is only an array or a pointer to an object: `f[1]` (function designator), `int (*fp)(void); fp[1]`, `int (*tbl[2])(void); tbl[0][0]`, file-scope initializers (`int v = f[1];`) and aggregate element positions (`{ f[1], 0 }`) now match clang **line for line** — `subscript of pointer to function type 'int (void)'` (spelled from the prototype, anchored at the base); a **function pointer** base was previously **silently accepted** (it read function-body bytes at run time; observed crash `0xC0000005`) and is rejected as well. **Since 0.4.1.76** arithmetic and subscripting on a **`void *`** follow GNU semantics with a **1-byte** step: the statement form `void *vp; vp[1];` no longer crashes colang at compile time (it used to give `0xC0000005`; clang only warns with `-Wunused-value`), `vp + 1` / `1 + vp` / `vp - 1` / `vp++` / `vp += 1` are no longer falsely rejected with `GEP into unsized type` (a 1-byte step, the same rule as `(void *)arr + 1` and `void * - void *`), and `sizeof(void)` / `sizeof(vp[1])` are 1; reading a `void` element **loads no memory** (a zero placeholder is produced), so a null `vp` is safe. **Since 0.4.1.77** a **scalar base** matches clang line for line and verbatim: `int x; x[1]`, `double d; d[1]`, `struct S s; s[1]`, `union U u; u[0]`, `enum E e; e[0]`, `1[0]`, `a[0][0]` (anchored at the second `[`), `x[1][2]` (the first `[`), `s.a[0]`, `(s.a)[0]`, `g()[0]` and `vf()[0]` all report `subscripted value is not an array, pointer, or vector`, anchored at the `[` of the **offending level** (it used to print our own wording anchored at the leftmost start of the whole base, 1–4 columns earlier). **Since 0.4.1.79** a subscript on a pointer to an **incomplete record** is rejected by sema on the same line and with the same wording as clang — `subscript of pointer to incomplete type 'struct I'`, anchored at the `[` of that level (a forward-declared `enum` stays accepted, as clang accepts it; the initializer, argument, address-of, arithmetic and member-access shells all report this single diagnostic — previously the form never reached sema, was only caught in codegen as `GEP into unsized type`, and the enclosing shell reported an unrelated diagnostic first). **Since 0.4.1.84**, using a **`void` value in binary operations, simple/compound assignments, initializers, arguments or `++/--`** (e.g. `vp[1] != 0`, `vp[1] = vp`, `int x = vp[1];`, `int y = f();` where f returns void) is rejected by sema, with this batch's primary errors matching clang in **line, wording and column**, instead of silent acceptance, compiler crashes or verifier fallbacks. Binary/assignment/increment/decrement errors are anchored at the operator; void-source initializers at the **declarator identifier**, including function pointers, multiple declarators and multiline initializers; other compatibility families retain their existing locations. Explicit discards, void calls used as statements and void left operands of comma expressions stay accepted. In a void function, `return vp[1];` / `return (void)0;` / `return f();` are accepted under **clang's default extension policy**, preserving the returned expression's side effects (not a claim of strict ISO C validity). Returning a non-void value from a void function still errors, now including the function name. **Unary operations such as `-`/`!`, condition positions and returning void to a non-void result type** remain separate known gaps. **Since 0.4.1.80** applying `sizeof` / `_Alignof` to an **incomplete record** (`struct I *ip; sizeof(*ip)`, `sizeof *ip`, `sizeof(struct I)`, `_Alignof(struct I)`, plus typedef and union forms) also enters the sema rejection surface, matching clang **line, wording and column**: the type form is anchored at the `sizeof` keyword, the expression form at the **first token after the keyword** (the `(` of a parenthesized form, the first operand token of the unparenthesized one); the wording is `invalid application of 'sizeof' to an incomplete type 'struct I'` (`_Alignof` prints `alignof` even when spelled `_Alignof`; typedef forms carry the `(aka 'struct I')` alias). A forward-declared `enum` (`sizeof(*ep)`) is still accepted, and operands already caught by an earlier diagnostic (`ip->a`, `ip[0]`) do not get this extra line; before this the family **silently evaluated to 0** (mis-accepted plus a silently wrong value). Two known under-reports remain: a record defined **after** the `sizeof` (completeness means "some definition exists in the TU") and the dimension position `int arr[sizeof(*ip)];`. Since **0.4.1.82** pointer arithmetic on an incomplete record (`ip + 1`, `1 + ip`, `ip - 1`, `ip++`, `--ip`, `ip += 1`, `ip -= 1`, including the file-scope initializer `struct I *gq = gp + 1;`) is on the sema rejection surface as well, matching clang **line, wording and column**: the wording is `arithmetic on a pointer to an incomplete type 'struct I'`, binary forms are anchored at the **operator** column while postfix/prefix `++`/`--` and `+=`/`-=` are anchored at the **operator start**, unions print `'union U'` and typedefs carry `(aka 'struct I')`; after the error the result type is treated as unknown, so `int x = ip + 1;` / `char *cp = ip + 1;` report this single line (previously the int-conversion / incompatible-pointer line fired first), and when both sides are pointers of incompatible types only `'X *' and 'Y *' are not pointers to compatible types` is reported. Two known gaps remain: `1 - ip` belongs to the separate `invalid operands to binary expression` family (still backed by the codegen `GEP into unsized type` fallback), and file-scope globals whose record is defined **after** the arithmetic (same TU-wide rule).

For a constant use `public const` / `public enum`; to read a runtime value, assign inside a function body.

**⑤ Qualified class statics `mod.Counter::total`**: `mod.Counter` on the left of `::` is first merged into a qualified class name, then resolved in class scope (static data members / static methods); reads, writes and calls have the same rights as with the bare class name:

```co
import * from counter as c;      // public class Counter { public static int total; public static int twice(int x); };

if (c.Counter::twice(21) != 42) return 1;
c.Counter::total = 10;                // write: the static is one symbol across modules (linkInModule merge)
if (c.Counter::total != 10) return 2;
```

Name checking: `c.Counter::nope` -> `no member named 'nope' in 'c.Counter'`; `c.Counter::n` (a non-static member) -> `invalid use of non-static data member 'n'`; a misspelled module / non-`public` class -> `no public type 'X' in module 'c'`.

**⑥ Qualified `typedef`: only `public typedef` is exported** (since 0.4.1.40): a **file-scope** `typedef` marked `public` joins the export surface, so the consumer may write `a.MyInt`. The bare name works too (same rights as tags); when several modules export the same name, the three-stage scheme above applies — definition-side warning + bare-name error + qualified name available:

```co
// types.co
public typedef int MyInt;
public struct Point { int x; int y; };
public typedef struct Point MyPoint;   // struct alias: the consumer gets types' layout
typedef int Hidden;                    // not public -> not exported

// main.co
import * from types as t;

t.MyInt a = 3;      // qualified name
MyInt b = 4;        // bare name (single source) works too
t.MyPoint p;        // struct alias -> types' two-member layout (even if the host has its own struct Point)
p.x = 1;
p.y = 2;
```

- **Only `public typedef` is exported**: writing `t.Hidden` for a non-`public` one reports `no public type 'Hidden' in module 't'`.
- **Fully on par with qualified type names**: declarations, parameters, return types, pointers/arrays, `sizeof`, casts `(t.MyInt)x` and initializer expressions are all accepted.
- **The underlying type must not be an imported type**: `public typedef a.Point Bad;` (aliasing a type exported by another module) reports `public typedef 'Bad' cannot alias imported type 'a.Point'` — rejected deliberately in this first version, so one module's dependency is not half-carried into another module's export surface.
- **A host-side same-named `typedef` wins** (the bare name binds to the host), while `t.X` still refers to the module's copy — same as tags.

> **Breaking change (with a migration example)**: before 0.4.1 the same scenarios were a **silent miscompile** (the later import won; fields of the other layout were folded onto slot 0, so reads and writes crossed over). Code that relied on that "happens to compile" behaviour now gets an **error** and must switch to qualified names (`a.Leaf` / `b.Leaf`) or to distinct tag names.

The two same-named definitions (different layouts):

```co
// leaf_a.co
public struct Leaf { int a; };              // 4 bytes

// leaf_b.co
public struct Leaf { char *s; int y; };     // 16 bytes
```

**Before the migration (0.4.0 and earlier: compiles, but silently miscompiles)**

```co
import * from "leaf_a.co" as a;
import * from "leaf_b.co" as b;

Leaf g;                            // cc=0, zero diagnostics: the bare name binds just one of them (the later import wins)
if (sizeof(Leaf) != 4) return 1;   // this yields 16 — nothing at compile time, it surfaces only at run time
```

**After the migration (0.4.1: stopped at compile time)**

```text
leaf_a.co:1:22: warning: tag 'Leaf' is exported by multiple imported modules (a.Leaf, b.Leaf);
              unqualified references are an error, use a qualified name
main.co:4:1: error: reference to 'Leaf' is ambiguous (defined in multiple imported modules:
              a.Leaf, b.Leaf); use a qualified name
```

**Two ways to fix it**

```co
// Fix ① (recommended): qualify with the candidates the diagnostic listed, each binds its own layout
a.Leaf g;          // 4 bytes: back to the original intent
b.Leaf h;          // 16 bytes
if (sizeof(a.Leaf) != 4) return 1;
if (sizeof(b.Leaf) != 16) return 2;
```

Fix ②: if the two same-named types were never meant to coexist, rename the tags (e.g. `LeafA` / `LeafB`); the bare name keeps working and no qualifier is needed.

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
9. **Type tags (`struct`/`union`/`enum`/`class`) follow the same scheme**: two or more modules exporting a same-named public tag → an **unconditional warning** on the definition side; a host bare-name reference → `reference to 'X' is ambiguous ...; use a qualified name`; switching to the qualified type name `m.Tag` binds each module's own layout (see "Same-Name Tags" above). If the host defines a same-named type, the bare name belongs to the host (like rule ②).

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

## Current Implementation Boundary (0.4.1)

Implemented:
- Full `.co` consumption pipeline (lexing / parsing / loading / **per-module closure analysis** / merging / visibility enforcement / linkage strip / entry-model integration);
- Both named and `*` forms; `import … as ModAlias` **enables qualified access**;
- **Module-alias qualified access** `m.f()` / `m.f` / `m.x` / `m.x = v` (with local write parity and `const` global write interception);
- **Independent module closures**: per-module type/symbol isolation, same-name different-layout `struct` types never hijack each other;
- **Ambient cut-off**: environment symbols brought in by a module's `#include` do not enter its export surface;
- **Name-clash adjudication ①~⑧**: shadowing / ambiguity / cross-module same-name coexistence (synthesized symbol names `__<stem>__<name>` at link time);
- Centralized topological execution with first-load semantics; deduplication; cycle detection reporting the full chain;
- Non-`public` imported symbols are forced to `InternalLinkage` during codegen;
- Cross-module `public struct` / `union` / `enum` type references (usable as value-typed variables, member access, and address-of for passing) — **identical in the main source and in submodules** (since 0.4.1, by-value instantiation inside a submodule no longer degrades to an incomplete type);
- Cross-module **bare tags**: a `public` `struct`/`union`/`enum` from an imported module may be referenced with the keyword omitted (`Point p;`);
- **Qualified type names `mod.Tag`** (0.4.1): accepted in every type-name position (declarations / `struct` keyword form / casts / `sizeof` / `_Alignof` / `typeof` / `new`), with `struct`/`union`/`enum`/`class` handled alike; same-name tags follow the three-stage scheme (definition-side warning + bare-name error + qualified names available); qualified **enum constants** (`p.RED`), qualified **class statics** (`c.Counter::total`) and qualified **`typedef`** (`a.MyInt`, only `public typedef` exported) ship in the same batch; since **0.4.1.47** qualified enum constants are also usable in **compile-time constant positions** (`case` labels, array lengths, `enum` initializers, global initializers), while **bare** enumeration constants still do not cross modules.
- Cross-module **compile-time constants**: a `public const int` from an imported module (e.g. the `WM_*` series in `winapi.co`) is usable in the consumer as a plain read-only value, an array dimension, or a `switch` `case` label; globals that are **not `public const` / not enum constants** (including a cross-module `public int`) used as a file-scope initializer are **rejected since 0.4.1.48** (`initializer element is not a compile-time constant`) instead of silently yielding 0; address-constant forms are unaffected; since **0.4.1.50** the rejection also covers **aggregate member positions** (`{M, 1}`/`{[0]=M}`/`{{M}, 1}`/`{.in.x = M}`), **dereference and subscript** (`*(&M)`/`*o`/`*arr`/`arr[0]`/`*(&s.a)`) and **member access** (`oo.y`/`oo.in.x`/`op->in.x`), matching clang line for line; since **0.4.1.52** it also covers **explicit pointer-to-integer conversions** (`(long)arr`/`(long)&M`/`(long)add9`; a same-object pointer difference without a cast, `arr - arr`, is unaffected), while loaded `extern` declarations and block-scope `static` initializers remain known gaps; **since 0.4.1.53 array decay through a member (`char *pc = oo.buf;`, `oo.m[1]`, `g2[1] + 1`) folds to a real address**, the subscript and row-decay forms having been rejected in error before that version and now accepted like clang; **since 0.4.1.55 a same-object pointer difference (`&arr[1] - &arr[0]` folds to 1) and the step width after a cast (`(char *)arr + 1` steps 1, `(void *)arr + 1` steps 1) fold to real values**; **since 0.4.1.57 an address-taken or decaying chain through a pointer (`&op->buf[1]`, `&(*op).buf[1]`, `(*op).buf`) is rejected on the same line as clang instead of silently folding a wrong address** (taking the address of the pointer variable itself, `&op`, stays accepted); **since 0.4.1.58 a cross-object pointer difference (`other - arr`, `&M - &N`, `ex - arr`, `arr + 1 - other`, an aggregate member position `{ other - arr, 0 }` — operands whose addresses resolve to two distinct objects) used as a file-scope initializer is rejected on the same line as clang** (the rejection shares its criterion with the folding side, so a same-object difference still folds to a real value); **since 0.4.1.62 a pointer difference between incompatible pointee types (`sa - arr`, `(long *)arr - (int *)arr`, `int (*)[3] - int (*)[4]`, `enum E1 * - enum E2 *`) is rejected by sema on the same line, column and text as clang** (C 6.5.9p4, `'X *' and 'Y *' are not pointers to compatible types`, anchored at the `-` operator; compatible forms such as qualification differences and `enum *` ↔ `int *` are still accepted); in the same batch a difference between two `void *` operands no longer crashes the compiler and yields a value with GNU's byte step, and pointer spellings in diagnostics now match clang (`int (*)[4]`/`int **`/`unsigned char *`); **since 0.4.1.63 `&function` resolves to its real function prototype (`&f` gives `int (*)(void)`), so the incompatible-pointee wording and the assignment form now match clang line for line, differences between differently prototyped function names are reported instead of silently skipped, and the legal form `(*&f)()` no longer crashes the compiler**; the bare-function-name decay form is closed since 0.4.1.67, and **since 0.4.1.73 implicit pointer ↔ integer conversions (initializer/assignment/argument wording) match clang line for line** (bool targets and null pointer constants stay legal); two known gaps remain — `char *` ↔ `signed char *`, and a **pointer-to-floating** conversion (`float f1 = arr;`, a different wording); the **subscript base** family matches clang since 0.4.1.75 for function designators and function pointers, and **since 0.4.1.76** arithmetic and subscripting on a `void *` use a 1-byte step with the statement form `vp[1];` no longer crashing the compiler (`sizeof(void)` / `sizeof(vp[1])` are 1); **since 0.4.1.77** a scalar-base subscript matches clang line for line too (anchored at the `[` of the offending level), the incomplete-record form is closed since 0.4.1.79 (same line and wording as clang, anchored at the `[` of that level, forward-declared `enum` still accepted); applying `sizeof` / `_Alignof` to an incomplete record is closed since 0.4.1.80 as well (the type form anchored at the keyword, the expression form at the first token after it); pointer arithmetic on an **incomplete record** is closed since **0.4.1.82** as well (binary forms anchored at the operator column, `++/--`/`+=/-=` at the operator start, `int x = ip + 1;` reporting only the arithmetic line); **since 0.4.1.84 void values in binary operations, assignments, initializers, arguments and `++/--` are rejected with the same line, wording and column as clang** (operators are anchored at their own column, void-source initializers at the declarator identifier, including complex declarators and multiline initializers). Explicit discards stay accepted; returning a void expression from a void function follows clang's default extension policy and preserves side effects. Remaining known gaps include unary/condition/non-void-result return uses of void values, `1 - ip` in the separate invalid-operands family, and file-scope forms whose record is defined after the arithmetic;
- **DLL / WinMain top-level-init carriers**: producing `-o *.dll` synthesizes a `DllMain` that runs each module's top-level initialization under `DLL_PROCESS_ATTACH` (a user-authored `DllMain` is renamed and forwarded); under a GUI-exe `WinMain` entry, top-level statements and runtime-initialized globals are carried by a synthesized `WinMain` wrapper (see the "Module" chapter).

Reserved names: `__init__*__` is a reserved prefix for the module-init dispatch protocol and **cannot be defined by user code** (compile-time error `reserved protocol name '__init__*' cannot be defined by user code`).

Not covered in this batch — deferred to later milestones:
- `.bc` / `.dll` / `.lib` / `.so` / `.obj` as importable modules (all of them must carry compile-time declaration metadata); all of them currently get an **explicit diagnostic** (see "Import Target Suffix Whitelist") rather than degrading into "module not found";
- User-authored `__init__` customization.
