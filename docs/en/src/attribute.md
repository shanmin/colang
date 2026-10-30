# Attributes (`[[...]]`)

## Syntax

Co adopts the C23 attribute spelling: a pair of brackets `[[` `]]` encloses one or more attributes, separated by commas, written before a declaration:

```
[[ ATTRIBUTE1, ATTRIBUTE2(...) ]]  declaration
```

Five attributes currently carry meaning in Co:

| Attribute | Applies to | Effect |
| --- | --- | --- |
| `[[lib("X")]]` | top-level function declarations, global variable declarations | declares that the symbol is supplied by external library `X`; the linker pulls `X` in automatically |
| `[[deprecated]]`, `[[deprecated("message")]]` | top-level functions, global variables | emits a warning at each reference to the symbol |
| `[[nodiscard]]`, `[[nodiscard("reason")]]` | functions with a return value (prototypes included) | warns when a call's return value is discarded in statement position; an explicit `(void)f()` does not warn |
| `[[noreturn]]` | `void` functions | verifies the function never returns: both a `return` in the body and falling off the end warn; call sites are treated as non-returning |
| `[[subsystem("windows")]]` | entry function (`main` / `WinMain`) | switches the linked artifact to the Windows GUI subsystem (no console) |

`[[` is the only spelling; the `<:` `::>` alternative token spelling from C is not supported by Co.

## `[[lib("X")]]` — declaring library ownership

Which library implements an external function (such as an OS API) had no way to be expressed: writing only a prototype without telling the linker the library name makes the link fail with an undefined symbol. `[[lib("X")]]` supplies that ownership:

```
[[lib("user32")]] int GetSystemMetrics(int nIndex);
[[lib("user32")]] unsigned int GetDoubleClickTime(void);

public int screen_width()
{
    return GetSystemMetrics(0);   // no -luser32 on the command line; the linker pulls user32 in
}
```

`X` is a **link name** (no extension, path, whitespace, quotes, or colon), completed by the linker for the target platform, spelled the same as on the `-l` command line. So `"user32"` is valid, while `"user32.lib"`, `""`, `"sub/user32"`, and `"C:x64/user32"` are all rejected.

Constraints:

- **`.co`/`.CO` source files only**: `[[lib]]` is a Co-defined attribute, so any other origin — a `.c`/`.h` file, including a header `#include`d by a `.co` — is rejected with `'lib' attribute is only allowed in .co source files`. The check looks at **the file the attribute actually appears in**; a `.h` is not relaxed just because a `.co` includes it. Putting library ownership in a shared header for several `.co` files to consume therefore does not work — it must be written inside the `.co`.
- **Only on declarations**: a function must be a pure prototype (no body), and a global variable must be a pure external declaration (no initializer). Claiming a symbol comes from an external library while also writing its definition in this module is a contradiction, which the compiler rejects.
- **Mutually exclusive with `public` and `static`**: `public` is export, `lib` is import — they cannot fall on the same symbol; `static` means internal linkage, which an external library cannot supply.
- **Same-name symbols must agree on their library**: re-declaring the same name with the same library is legal; giving the same name two different libraries is an error (no silent choice is made).
- Writing two `[[lib]]` attributes on one declaration is an error.

## `[[deprecated]]` — marking something deprecated

Marking a function or global variable with `[[deprecated]]` emits a warning at every later reference, nudging callers toward a different interface; a message can name the replacement:

```
[[deprecated]] int old_add(int x);
[[deprecated("use new_api instead")]] int new_api(int x);

int use(int x)
{
    return new_api(x);   // warning: 'new_api' is deprecated: use new_api instead
}
```

- When the attribute is on a forward declaration and the definition is written elsewhere, they merge by **the same entity** — the call site still warns.
- **Repeated annotations at the same position are accepted idempotently** (e.g. `[[deprecated, deprecated]]`, or two adjacent attribute lists); the **first message written wins** (first-wins) and later duplicates are ignored — the same rule clang follows.
- Warnings **do not change the program's exit code** and do not block compilation.
- `deprecated` on a declaration in local scope is not effective at present.

## `[[nodiscard]]` — the return value must not be discarded

Marking a function with a return value as `[[nodiscard]]` produces a warning whenever a call discards its result in statement position; a reason can spell out what the caller should do:

```
[[nodiscard]] int check(int x);
[[nodiscard("handle the result")]] int compute(int x);

int run(int x)
{
    check(x);                 // warning: ignoring return value of function declared with 'nodiscard' attribute
    compute(x);               // same warning, trailing the reason: : handle the result
    (void)check(x);           // explicit discard counts as intentional — no warning
    return check(x) + 1;      // using the value — no warning
}
```

- The warning text matches clang (`-std=c23`); with a reason, the trailing `: reason` is kept verbatim.
- **A prototype alone (a declaration without a body) already takes effect**: prototypes and definitions register as the same entity.
- Marking a function **without a return value** (`void`) is an error: `attribute 'nodiscard' cannot be applied to functions without return value`.
- Repeated annotations at the same position (`[[nodiscard, nodiscard]]`) are accepted idempotently, same as clang.

## `[[noreturn]]` — declaring that a function never returns

Marking a `void` function `[[noreturn]]` makes the compiler verify it truly never returns: a `return` appearing in the body warns (each occurrence), falling off the end of the body warns once more, and call sites are treated as non-returning — dead code after the call no longer triggers spurious "missing `return`" complaints:

```
[[noreturn]] void bail(void);

int work(int c)
{
    if (c)
        bail();               // call site treated as non-returning
    return 0;                 // no spurious "not all paths return a value"
}
```

- The warning text matches clang: `function 'h' declared 'noreturn' should not return`.
- `[[_Noreturn]]` is the deprecated C23 spelling; Co accepts it as synonymous with `[[noreturn]]` (without emitting an extra deprecation warning for the old spelling).
- Again, **only the annotation matters, not the writing order**: marking a prototype registers it; repeated annotations are accepted idempotently.

## `[[subsystem("windows")]]` — switching the link subsystem

By default Co produces a **console**-subsystem program (it opens a console window at runtime). Marking the entry function with `[[subsystem("windows")]]` switches the linked artifact to the **Windows GUI subsystem** (no console window):

```
[[subsystem("windows")]]
int WinMain(void* hInstance, void* hPrevInstance, char* lpCmdLine, int nCmdShow)
{
    // create the window, run the message loop...
    return 0;
}
```

There are only two possible values:

- `"windows"`: Windows GUI subsystem, no console.
- `"console"`: console subsystem (an explicit spelling, equivalent to not marking it).

Constraints and key points:

- **Only takes effect on the entry function (`main` or `WinMain`)**; marking it on any other function emits a warning (without blocking compilation).
- **Writing two `[[subsystem]]` attributes on one declaration is an error**; a value that is neither `"windows"` nor `"console"` is an error.
- **Defining `WinMain` already implies the windows subsystem**; you need not also write `[[subsystem("windows")]]` (writing it does not conflict).
- See the "Program Entry and Execution" chapter for more on entry points and `WinMain`.

## Feature probing and unrecognized attributes

The preprocessor directive `__has_c_attribute(name)` returns `1` or `0` and can drive conditional compilation:

```
#if __has_c_attribute(lib)
[[lib("user32")]] int GetSystemMetrics(int nIndex);
#endif
```

- Known attributes that still carry no effect (`maybe_unused`, `fallthrough`, and others) are **accepted silently** — no error, no warning — so that code migrated from C/C++ is not disturbed.
- An unrecognized bare name emits one `unknown attribute 'x' ignored` warning and is skipped, without blocking compilation.
- A namespaced attribute name (such as `[[colang::lib]]`) is **rejected with an error**: Co does not pretend to support vendor attributes it cannot honor.

## Relationship to C / C++

| Aspect | Co | Compared to |
| --- | --- | --- |
| Attribute spelling `[[...]]` | adopted from C23 | same as C23 |
| `[[deprecated]]` | supported (diagnostic layer, not in the artifact) | matches C23 semantics |
| `[[nodiscard]]` / `[[noreturn]]` | first batch of semantics in effect (warnings and verification, clang-shaped) | largely matches C23 (type-level marking etc. see below) |
| `[[maybe_unused]]` / `[[fallthrough]]` | accepted but no effect | differs from C23 (C23 has real semantics) |
| `[[lib("X")]]` | newly defined by Co; no such attribute in the C standard; and `.co`/`.CO` source files only | Co-specific |
| `[[subsystem("windows")]]` | newly defined by Co; no such attribute in the C standard | Co-specific |
| `<:` `::>` alternative spelling | not supported | differs from C23 |

> **Compatibility note**:
> 1. `[[lib("X")]]` is a Co-defined attribute. When a real clang/clangd reads `.co` files, it warns `-Wunknown-attributes` and ignores it — **this is necessarily so, not a defect**; external tools will not validate library ownership, only the Co compiler consumes it.
> 2. When migrating from C/C++, `#pragma comment(lib, "X")` and `__declspec(dllimport)` are **neither implemented nor simulated** by this compiler; library ownership must always be rewritten explicitly as `[[lib("X")]]`. And `[[lib]]` **can only be written in `.co` files** — a pure `.c` translation unit names its external libraries on the command line with `--lib <name>` instead (see the "Command Line" chapter), with no need to move declarations into a `.co`.
> 3. `[[deprecated]]` is a front-end diagnostic only; like clang, Co **does not encode it into the artifact's IR**, so it does not propagate to downstream modules that consume the artifact.
> 4. `[[subsystem("windows")]]` is likewise a Co-defined attribute: a real clang/clangd warns and ignores it as an unknown attribute (**necessarily so, not a defect**); only the Co compiler consumes the subsystem switch.

## Vendor-compatibility extension: `__attribute__` (GCC/Clang)

> **Qualification (added solely for compatibility)**: the syntax in this section is **not ISO C** (no such spelling exists in C89–C23; the standard attributes are the `[[...]]` of the previous section) and is **not a Co dialect feature**. It exists **purely to be compatible with existing source code coming from GCC/Clang** — so such code need not be rewritten site by site just to compile — and is **not part of Co's language design**. For newly written code, prefer the C23 `[[...]]` attributes and `_Alignas(N)`; do not rely on `__attribute__`.

Co accepts the GNU `__attribute__((...))` spelling and handles it in **tiers**:

- **`aligned(N)` — given real semantics**: it raises the alignment of an object / member / `struct`·`union` type / `typedef` (observable via `sizeof`, `_Alignof`, `offsetof`), matching the local clang `-std=c23` verbatim. It is a **one-way promotion** only: alignment becomes `max(natural alignment, N)` and is **never lowered**. `N` must be a power of two, otherwise it is an error (same as clang).
- **Every other attribute — syntax accepted, semantics ignored, a warning emitted**, in **two levels** by risk:
  - **Strong warning (may affect memory layout / calling convention / linkage)**: `packed`, the calling-convention family (`stdcall` / `cdecl` / `fastcall` / `thiscall` / `vectorcall`), `section`, `weak`, `alias`, `dllimport` / `dllexport`, `constructor` / `destructor`, and the like. Text shaped like `gnu attribute 'packed' ignored: may affect layout/ABI (non-standard, GCC/Clang compatibility)`.
  - **Light warning (optimization / diagnostics / informational only; ignoring changes no observable behavior)**: `noinline`, `always_inline`, `format`, `unused`, `used`, `pure`, `const`, `visibility`, `deprecated`, and the like, as well as **unrecognized attribute names**. Text shaped like `gnu attribute 'noinline' ignored (non-standard, GCC/Clang compatibility)`.
- **None of these warnings block compilation or change the exit code.**

The positions where `__attribute__` may appear (matching GCC): before the declaration specifiers; between the `struct`/`union` keyword and its tag name and between the tag name and `{`; after the closing `}` (before a `typedef`/variable name) and after a `typedef` name; between the return type and the function name; before the type of a cast's abstract declarator; before the `*` inside a function-type parenthesis.

```
struct __attribute__((aligned(32))) Block { char c; };   // _Alignof(Block) == 32
int  __attribute__((aligned(16))) giant;                  // raises alignment
void __attribute__((stdcall)) api(void);                  // accepted + strong warning (stdcall ignored)
int  __attribute__((noinline)) leaf(void) { return 42; }  // accepted + light warning
```

> **Compatibility note (important)**:
> 1. Real GCC/Clang **fully implement** these attributes; Co **honors only `aligned`** and ignores all the rest. If source code relies on `packed` to compact a layout or on a particular calling convention, it **still compiles under Co but behaves differently from GCC/Clang** (the struct is not packed to 1 byte, the calling convention is unchanged) — this is exactly why Co emits a **strong warning** for every ignored "layout/ABI" attribute instead of accepting it silently.
> 2. When a real layout effect is needed, `aligned` can be rewritten as the C23 `_Alignas(N)`; `packed` and friends have no standard counterpart and thus no equivalent spelling yet.
> 3. `__declspec(...)` (the MSVC extension) is **out of scope for this extension and not supported**; use `[[lib("X")]]` for library ownership.
> 4. The rare `aligned` forms (bare `aligned` with no argument, `aligned(_Alignof(T))`, `aligned(type)`) are **downgraded to a light-warning ignore** in this batch.

## Current boundaries (compiler implementation status)

- `[[lib("X")]]` lowers to a `/DEFAULTLIB` linker directive in the COFF `.drectve` section, deduplicated by first appearance; this relies on the link step consuming the directive and **cannot yet** do "precise per-symbol binding to a specific dll" — per-symbol binding for same-named symbols across multiple dlls is a later linker capability.
- `[[deprecated]]` is limited to reference-site warnings on top-level functions and global variables; local declaration positions are inert, and C23's second legal spot (after the declarator) is not modeled.
- The first batch of `[[nodiscard]]` / `[[noreturn]]` semantics has landed (discard warnings / non-return verification). Boundaries: marking a non-function declaration (e.g. `[[nodiscard]] int x = 7;`) is accepted silently; marking a `struct`/`union` type or a function-pointer type (allowed by C23) is not modeled; the old `[[_Noreturn]]` spelling draws no deprecation warning.
- `[[maybe_unused]]`, `[[fallthrough]]`, `[[align]]` and others remain silently accepted — Co currently has no "unused variable" or "implicit fallthrough" class warnings for them to suppress; they will be enabled when such warnings land.
- `[[subsystem("windows")]]` is realized via the linker command-line `-subsystem:windows`, which takes precedence over the static CRT's embedded `.drectve`; the entry then resolves to `WinMainCRTStartup` for `WinMain`, otherwise pinned to `mainCRTStartup`. It is valid only for Windows/COFF targets; the wide variant `wWinMain` is not yet supported.
