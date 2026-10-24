# Attributes (`[[...]]`)

## Syntax

Co adopts the C23 attribute spelling: a pair of brackets `[[` `]]` encloses one or more attributes, separated by commas, written before a declaration:

```
[[ ATTRIBUTE1, ATTRIBUTE2(...) ]]  declaration
```

Two attributes currently carry meaning in Co:

| Attribute | Applies to | Effect |
| --- | --- | --- |
| `[[lib("X")]]` | top-level function declarations, global variable declarations | declares that the symbol is supplied by external library `X`; the linker pulls `X` in automatically |
| `[[deprecated]]`, `[[deprecated("message")]]` | top-level functions, global variables | emits a warning at each reference to the symbol |

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
- Warnings **do not change the program's exit code** and do not block compilation.
- `deprecated` on a declaration in local scope is not effective at present.

## Feature probing and unrecognized attributes

The preprocessor directive `__has_c_attribute(name)` returns `1` or `0` and can drive conditional compilation:

```
#if __has_c_attribute(lib)
[[lib("user32")]] int GetSystemMetrics(int nIndex);
#endif
```

- Known attributes that carry no effect in the current batch (such as `nodiscard`, `maybe_unused`, `noreturn`, `fallthrough`, and others) are **accepted silently** — no error, no warning — so that code migrated from C/C++ is not disturbed.
- An unrecognized bare name emits one `unknown attribute 'x' ignored` warning and is skipped, without blocking compilation.
- A namespaced attribute name (such as `[[colang::lib]]`) is **rejected with an error**: Co does not pretend to support vendor attributes it cannot honor.

## Relationship to C / C++

| Aspect | Co | Compared to |
| --- | --- | --- |
| Attribute spelling `[[...]]` | adopted from C23 | same as C23 |
| `[[deprecated]]` | supported (diagnostic layer, not in the artifact) | matches C23 semantics |
| `[[nodiscard]]` / `[[maybe_unused]]` / `[[noreturn]]`, etc. | accepted but no effect | differs from C23 (C23 has real semantics) |
| `[[lib("X")]]` | newly defined by Co; no such attribute in the C standard | Co-specific |
| `<:` `::>` alternative spelling | not supported | differs from C23 |

> **Compatibility note**:
> 1. `[[lib("X")]]` is a Co-defined attribute. When a real clang/clangd reads `.co`/`.c` files, it warns `-Wunknown-attributes` and ignores it — **this is necessarily so, not a defect**; external tools will not validate library ownership, only the Co compiler consumes it.
> 2. When migrating from C/C++, `#pragma comment(lib, "X")` and `__declspec(dllimport)` are **neither implemented nor simulated** by this compiler; library ownership must always be rewritten explicitly as `[[lib("X")]]`.
> 3. `[[deprecated]]` is a front-end diagnostic only; like clang, Co **does not encode it into the artifact's IR**, so it does not propagate to downstream modules that consume the artifact.

## Current boundaries (compiler implementation status)

- `[[lib("X")]]` lowers to a `/DEFAULTLIB` linker directive in the COFF `.drectve` section, deduplicated by first appearance; this relies on the link step consuming the directive and **cannot yet** do "precise per-symbol binding to a specific dll" — per-symbol binding for same-named symbols across multiple dlls is a later linker capability.
- `[[deprecated]]` is limited to reference-site warnings on top-level functions and global variables.
- The real effects of `[[nodiscard]]`, `[[maybe_unused]]`, `[[noreturn]]`, `[[align]]`, and others are not yet implemented (currently accepted silently only).
