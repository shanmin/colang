# Data Types

This chapter covers Co's **built-in types** — integers, character types, booleans, floating-point types, pointers and arrays — together with `_BitInt`, `typedef` / `typeof` / `auto`, the typing of literals, qualifiers, and implicit-conversion rules. The baseline is ISO C (C23 recommended); on top of it Co carries one **breaking extension**: inside `.co` files `long` is always 64 bits (see "Co extension · `long` is 64-bit in `.co`"). For `struct` / `union` / `enum` see the "struct / union / enum" chapter, for `class` see the "class" chapter.

All widths below are **measured on Windows x64**; on Linux x64 pointers are 8 and `long` is 8 (native LP64 for the target), everything else is unchanged.

## Syntax

```
type ::=  integer-type            [ qualifier … ]
       |  floating-type           [ qualifier … ]
       |  void                   [ qualifier … ]
       |  type  *                [ qualifier … ]              // pointer
       |  type  [ constant-expression ]                        // array (length may be empty → flexible member / inferred)
       |  ( struct | union | enum ) [ TAG ] { … } | TAG        // derived types, see the respective chapter
       |  typeof ( type | expression )
       |  typeof_unqual ( type | expression )
       |  auto                                                  // inferred from the initializer (C23)
       |  _BitInt ( integer-constant )
```

Integer specifiers may be combined freely in any order; the omitted width defaults to `int`:

| Spelling | Equivalent type |
| --- | --- |
| `char` / `signed char` / `unsigned char` | distinct types (`char` and `signed char` share width 1) |
| `short` / `short int` / `signed short` / `unsigned short` | `short` / `unsigned short` |
| `int` / `signed` / `signed int` / `unsigned` | `int` / `unsigned int` |
| `long` / `long int` / `signed long` | `long` |
| `long long` / `long long int` / `unsigned long long` | `long long` / `unsigned long long` |
| `_Bool` / `bool` | the same type (`bool` is the spelling Co recommends) |

Qualifiers: `const`, `volatile`, `restrict`, `_Atomic`.

## Widths and alignment

| Type | `.c` / `.h` | `.co` |
| --- | --- | --- |
| `_Bool` / `bool` | 1 / 1 | 1 / 1 |
| `char`, `signed char`, `unsigned char` | 1 / 1 | 1 / 1 |
| `short`, `unsigned short` | 2 / 2 | 2 / 2 |
| `int`, `unsigned int` | 4 / 4 | 4 / 4 |
| **`long`, `unsigned long`** | **4 / 4** | **8 / 8** |
| `long long`, `unsigned long long` | 8 / 8 | 8 / 8 |
| `float` | 4 / 4 | 4 / 4 |
| `double`, `long double` | 8 / 8 | 8 / 8 |
| `wchar_t` | 2 / 2 | 2 / 2 |
| `char8_t` | 1 / 1 | 1 / 1 |
| `char16_t` | 2 / 2 | 2 / 2 |
| `char32_t` | 4 / 4 | 4 / 4 |
| Any pointer, `void*` | 8 / 8 | 8 / 8 |
| `enum` | 4 / 4 | 4 / 4 |
| `size_t`, `ptrdiff_t`, `intptr_t` | 8 / 8 | 8 / 8 |
| `_BitInt(N)` | `ceil(N/8)` / see below | same as left |

Cells are formatted "`sizeof` / `_Alignof`". The width of a `struct` / `union` follows from member layout and alignment; see the respective chapter.

## Semantics

(This section describes the externally-observable semantics and behaviour; internal implementation mechanisms are out of scope.)

### Integers

The widths of `char`, `short`, `int` and `long long` match ISO C; `long` is the only type whose width differs between `.co` and `.c` (see the dedicated section below). `signed` and `unsigned` used alone mean `int` / `unsigned int`.

- `char` is **signed** (`char c = -1; printf("%d", c)` prints `-1`), matching the MSVC / clang default on Windows. Write `unsigned char` explicitly when you need unsigned semantics.
- Unsigned arithmetic wraps modulo the type width: `unsigned v = 0u - 1u;` yields `4294967295`.
- Signed overflow remains undefined behaviour; the language makes no saturation or wrap-around promise.
- `<stdint.h>` gives `int8_t` … `int64_t` / `uint64_t` at the widths listed above; `<stddef.h>` gives `size_t` / `ptrdiff_t` / `nullptr_t`, all 8 bytes (pointer width).

### Character and boolean types

`wchar_t` is **2 bytes unsigned** (aligned with the host CRT's wide-character semantics, i.e. a Windows UTF-16 code unit). `char8_t` / `char16_t` / `char32_t` (C23 / C11) are 1 / 2 / 4 bytes and **always unsigned**.

`bool` and `_Bool` are the **same type**: **1 byte** wide, `false == 0`, `true == 1`, and every assignment normalises to `0` or `1` (`bool b = 5;` yields `1`). They are two spellings of one type and therefore **must not appear as two separate associations in a `_Generic`**.

### Floating-point types

`float` is 4 bytes, `double` is 8. `long double` is accepted syntactically, takes part in arithmetic and can be printed with `%Lf`, but its **width equals `double` (8 bytes)** — extended (x87-style) precision is not provided today.

- Floating-point types **do not accept `signed` / `unsigned`** (as ISO C requires): `signed float`, `unsigned double` and `unsigned long double` are all rejected with a diagnostic such as `'float' cannot be signed or unsigned`; diagnostics spell `long double` as `double`.
- Floating literals take only the four suffixes `f` / `F` / `l` / `L`, and **upper and lower case must not be mixed** (`1.0lF` is rejected). `1.0d` is not a valid suffix.

### Arbitrary-width integers `_BitInt(N)`

C23's `_BitInt(N)` is supported for `N` in 1..65535:

- `sizeof(_BitInt(N)) == (N + 7) / 8`.
- `_Alignof` is bucketed by width: `N ≤ 8` → 1, `N ≤ 16` → 2, `N ≤ 32` → 4, anything wider → 8 (measured: `_BitInt(40)` is size 5 / align 8, `_BitInt(128)` is 16 / 8).
- Signed by default; `unsigned _BitInt(N)` may be written. Arithmetic truncates to the declared bit width (`100 + 27` on `_BitInt(7)` yields `-1`).
- Mixed with `int` the result is `int` (the integer rank of `_BitInt(N)` is below `int`).
- For 128-bit integers write `_BitInt(128)`; `__int128` is **not** a spelling of this language.

### Pointers and `void`

All object pointers are 8 bytes wide. `void*` converts **implicitly** to and from any object pointer, no cast needed (`int v = 5; void *p = &v; int *q = p;` passes as written). `nullptr_t` (`<stddef.h>`) and `void*` are two spellings of the **same type**, so they cannot be listed as two separate associations in a `_Generic`; both `NULL` and `nullptr` work for null-pointer initialisation and comparison. Function pointers may be variables, parameters and return values (`int (*f)(int, int) = add; f(2, 3);`).

### Arrays

`sizeof` of `int a[10];` is element width × length; multi-dimensional arrays are stored contiguously in row-major order (`int m[3][4]` is 48). A string literal is a **character array including the trailing `\0`**: `sizeof("abc") == 4`, `sizeof("") == 1`.

- `char b[4] = "hi";` zero-fills the remaining elements; `char c[2] = "hi";` is allowed to truncate without `\0`.
- **Arrays degenerate to pointers as function parameters**: whether written `int f(int a[4])` or as a VLA parameter `int f(int n, int a[n])`, `sizeof(a)` inside the function body is 8 (pointer width), not the total element width.
- Variable-length arrays (VLAs) are accepted syntactically, and `sizeof(a)` is computed from the **current value** of the length expression at the point where `sizeof` is evaluated (consistent with ISO C): after `int n = 5; int a[n];`, changing `n` to 9 makes `sizeof(a)` yield 36. The array object does **not** grow with `n`; accessing past the original extent once `n` is larger remains undefined behaviour.
- Flexible array members (`char data[];`) are covered by the "struct / union / enum" chapter.

### Type aliases and inference

- `typedef` behaves as in ISO C and works for function pointers, arrays and structs; a `typedef` name participates in type-spelling reconstruction (diagnostics keep the alias form).
- `typeof(x)` (C23) yields the type of an expression or a type; arrays and structs both work: `typeof(a) b;` declares an array of the same dimension. The result of `typeof` is a **type name**, so it can be written directly inside `sizeof(typeof(e))` and `_Alignof(typeof(e))`. `typeof_unqual(x)` gives the same type with top-level qualifiers stripped.
- `auto` (C23) infers from the initializer: `auto x = 1;` is `int`, `auto p = &v;` is a pointer, `auto n = 3.5;` is `double`, and `auto f = 2.0f;` **keeps `float`** (no default promotion). The form `auto *p = &v;`, which puts `*` into the declarator after `auto`, is not supported today — write pointers as `auto p = &v;`.

### Types of literals

An integer literal takes the **first type in the C §6.4.5.2 candidate sequence that can hold the value**; the suffix determines the candidate set.

| Literal | `.c` / `.h` | `.co` |
| --- | --- | --- |
| `1` | `int` (4) | `int` (4) |
| `1u` | `unsigned int` (4) | `unsigned int` (4) |
| `1L` / `1l` | `long` (4) | `long long` (8) |
| `1UL` | `unsigned long` (4) | `unsigned long long` (8) |
| `1LL` / `1ULL` | `long long` / `unsigned long long` (8) | same as left |
| `3000000000`, `0xffffffff`, `0x100000000` | 8 / 4 / 8 | same as left |
| `1wb` / `1ub` | exact-width `_BitInt` literal (1) | same as left |

Key points:

- An unsuffixed decimal that exceeds `int` promotes to `long` (in `.c` 4 bytes cannot hold it, so it skips to `long long`); hence `sizeof(3000000000) == 8`.
- Octal and hexadecimal try the unsigned candidates first, hence `sizeof(0xffffffff) == 4` (`unsigned int`).
- In the `.co` dialect `long` and `long long` are both 64-bit, so after de-duplicating the candidate sequence `1L` and `1UL` land on 64 bits directly.
- Floating literals default to `double`: `1.0`, `.5` and `1.0L` are all 8 bytes; `1.0f` / `1.0F` are `float`.
- The type of a character constant `'a'` is **`int`** (`sizeof('a') == 4`); the prefixed forms are `L'a'` → `wchar_t` (2), `u'a'` → `char16_t` (2), `U'a'` → `char32_t` (4).
- For exact-width literals the suffix **must be written in full** as `wb` / `ub`: `sizeof(1wb) == 1`, `sizeof(255wb) == 2`; the truncated `1w` and `0x1w` are **rejected as invalid suffixes** (`invalid suffix 'w' on integer constant`) rather than passing as an ordinary `int`. Integer suffixes likewise require **consistent case pairing**: `1lL` and `1wB` are rejected, while `1ll`, `1llu`, `1uwb` and `1UWB` are accepted.
- The string prefixes `u8"…"`, `u"…"`, `U"…"` and `L"…"` have `char8_t`, `char16_t`, `char32_t` and `wchar_t` elements respectively, with width (number of code units + 1) × element width.

### Qualifiers

`const` has real read-only semantics (writing to a `const` object is an error). `volatile`, `restrict` and `_Atomic` participate in **type spelling and compatibility checking** but currently **do not change code generation** — `volatile` does not inhibit optimisation nor order volatile accesses; for atomic operations call the `<stdatomic.h>` functions such as `atomic_load` / `atomic_store`, which do emit genuine atomic instructions. `register` is accepted and is a no-op.

### Implicit conversions, promotions and arithmetic conversions

- **Integer promotion**: narrow integer types such as `char` / `short` are promoted to `int` before arithmetic (`int` when it can hold all values, otherwise `unsigned int`). Hence `(char)100 + (char)100` gives `200` and `short a = 30000; a + a` gives `60000`, neither overflowing.
- **Usual arithmetic conversions**: on the integer side the wider type wins, and at equal width the unsigned side dominates — `unsigned + int` is `unsigned int`, `int + long long` is `long long`; on the floating side mixing `float` with `double` yields `double`.
- **Explicit casts** `(T)e` truncate the value: `(char)300` gives `44`, `(unsigned char)-1` gives `255`; float-to-integer conversion **truncates toward zero**: `(float)3.99` gives `3`, `(double)-2.7` gives `-2`.
- The type of an **assignment** expression is the type of its left operand (`float f; sizeof(f = f + d)` is 4).
- Pointers and integers may be cast to each other; `_Generic` dispatches on the exact type, and `bool` / `_Bool`, or `long` / `long long` inside `.co`, cannot be listed together because they are the same type.

### `sizeof`, `_Alignof` and `_Alignas`

- `sizeof` applies to both **types** and **expressions**; applied to an arithmetic expression involving variables, the result is computed from the expression's **actual type**: with `double d; float f;`, `sizeof(d + d)`, `sizeof(-d)`, `sizeof(1 + d)` and `sizeof(d / f)` all yield `8`, `sizeof(f + f)` yields `4`, and `sizeof(c + c)` (with `char c;`, after integer promotion) yields `4`.
- Applied to a struct, `sizeof` returns the total width including padding, and a flexible array member contributes 0; applied to a string it includes the `\0`.
- `sizeof(void)` yields **1** and `sizeof(function)` yields **1** — an extension kept for compatibility with GNU sources; ISO C does not define it.
- A compound literal may be the operand: `sizeof (int){1}` yields `4`, `sizeof (double){0}` yields `8`, `sizeof (int[3]){0}` yields `12`. When the parentheses hold a **type name**, the width still comes from the type, so `sizeof (int) - 1` is `sizeof(int) - 1` (yields `3`), not a compound literal.
- `_Alignof(T)` with a **type name** matches ISO C; it also accepts the **expression** form (the GNU extension clang accepts as well): `_Alignof d` and `_Alignof(d + 1)` both yield the alignment of the operand's type. Without parentheses it binds as a **unary expression**, so `_Alignof d + 1` is `(_Alignof d) + 1` (yields `9`). The spellings `_Alignof` / `__alignof__` / `alignof` are equivalent.
- `_Alignas(N)` may be used on variables and struct members; the effective alignment is "the greater of natural alignment and `N`": `_Alignas(32) char buf[8];` places `buf` on a 32-byte boundary, and `struct { char c; _Alignas(16) int i; };` has `sizeof` 32. `__attribute__((aligned(N)))` contributes with equivalent effect (see the "Attributes" chapter).

### Co extension · `long` is 64-bit in `.co`

This is the **only width discontinuity** Co introduces relative to C, and the only breaking difference in this chapter:

- Inside **`.co` / `.CO`** files, `long` / `unsigned long` are always **8 bytes** (as wide as `long long`); in **`.h`, `.c`** and every other source file they stay **native to the target** (4 on Windows, 8 on Linux). Combined spellings widen too: `long int` and `signed long` are also 8 bytes in `.co`.
- The decision is made per **source file of each declaration itself**, not per "whole translation unit": when a `.co` file `#include`s a `.h`, the `long` in that header is still 4 bytes. Consequently **the layout of a struct in a shared header is byte-for-byte identical on the `.co` and `.c` sides** — a single header never yields two different `sizeof` values in the two kinds of source.
- `long` and `L` literals produced by **macro expansion** follow the dialect of the **use site**: with `#define BIG 0x100000000L` in a shared header, expanding it inside a `.co` file computes in 64 bits and does not falsely report overflow.
- Two writing disciplines derived from the 64-bit `long` in `.co`:
  - **Always write explicit widths when calling external (C) interfaces**: if the callee returns 32 bits (Win32 `DWORD` / `LONG`), write `int` / `unsigned int`; write `long` or `long long` only when it truly returns 64 bits. The language provides no "reject external declarations containing `long`" check — the compiler cannot see the real external signature and has no authority to judge correctness.
  - Inside `.co`, `long` and `long long` are the same type, so `_Generic(x, long: 1, long long: 2)` fails as a duplicate association; diagnostics may spell a `.co` `long` as `long long`.
- Where a value must stay consistent across `.co` and `.c`, prefer `int32_t` / `int64_t` outright.

> **Compatibility notice (regarding `long`)**: in a `.co` file, `printf("%ld", v)` passes 8 bytes while `%ld` reads only 4; the language performs no format-string checking, so use `%lld` or a `PRId64`-style spelling instead. Also, `#include <limits.h>` inside a `.co` file still yields the 4-byte `LONG_MAX` (`2147483647`) — test the range of a 64-bit `long` against `INT64_MAX`. These two points are language-level discontinuities that data-model macros cannot cover, and must be carried by writing discipline.

## Current boundaries (not yet supported / not finalized)

- **`_Complex` / `_Imaginary` and the `2.0i` imaginary literal are not implemented** (writing `_Complex` is diagnosed directly).
- **`__int128` is not a spelling of this language**; use `_BitInt(128)`.
- **`long double` has no extended precision** — equivalent to `double`.
- **The `volatile` / `restrict` / `_Atomic` specifiers do not reach code generation**; they only affect type spelling and compatibility.
- **`auto *p = &v;` is not supported** (the `*`-declarator form following `auto`); write `auto p = &v;`.
- **Bare tags as types (`Point p;`) are enabled only in `.co`**; in `.c` you still must write `struct Point p;` — see the "struct / union / enum" chapter.
- **GNU statement expressions `({ ... })` are not supported** (e.g. `x = ({ int t = 3; t + 4; })`; C has no such syntax — it is a vendor extension). Writing one produces `GNU statement expression '({ ... })' is not supported`, and the whole braced block is skipped, so one occurrence reports once. Rewrite it as ordinary statements or a helper function.
- **GNU range designators `[a ... b]` are not supported** (e.g. `int arr[4] = { [0 ... 2] = 7 };`, `t.a[1 ... 5] = 9`; C has no such syntax — it is a vendor extension). Writing one produces `GNU range designator '[a ... b]' is not supported`; the upper-bound expression is consumed and discarded, and the following `]` / `=` / initializer still parse normally, so one occurrence reports once. For the same fill effect, list the designators one by one (e.g. `[0]=7, [1]=7, [2]=7`) or assign in a loop.

## Reserved names

`bool`, `true`, `false`, `nullptr`, `constexpr`, `typeof`, `typeof_unqual`, `_BitInt`, `alignas`, `alignof`, `static_assert`, `thread_local`, `class`, `new`, `this`, `public`, `import`, `module` and the like are Co **keywords** and cannot be used as identifiers (`bool` / `true` / `false` are reserved even inside `.c`; `int bool = 1;` is diagnosed directly). C++'s `decltype` is **not** a Co keyword and is not implemented — it is simply an ordinary identifier; use `typeof(e)` to obtain the type of an expression. The remaining spellings (`int`, `long`, `float`, `_Bool`, `_Complex`, `_Imaginary`, `_Atomic`, `_Generic`, `_Noreturn`, `wchar_t`, `char8_t`, `char16_t`, `char32_t`, `restrict`, `volatile`) are reserved for the implementation under the C language-layer rules, and a program may not overload their meaning.
