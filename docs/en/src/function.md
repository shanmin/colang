# Function Definition

## Syntax

```
[ public ] TYPE FUNCTION_NAME([TYPE ARG1, ...])
{
	segment
	return VALUE;
}
```

## Description

(This section describes only the externally observable semantics and behavior of functions. Their internal implementation mechanism is outside the scope of this document.)

A function definition has four parts: the return type `TYPE`, the function name `FUNCTION_NAME`, the parameter list, and the function body. A function name is unique within its module; `return VALUE;` hands back the result and ends the call.

### Return type

- The return type **must be written explicitly**. Co does not accept the early-C "implicit int" convention (omitting the type meaning `int`); a function definition without a return type is rejected.
- `void` means the function returns no value. `void` may be used only as a function's return type and **cannot** be used as the type of a variable or a parameter.
- A function may return integers, floating-point values, pointers, struct values, class references, and so on. An array cannot be returned directly (to return an aggregate, wrap it in a struct, or hand back a pointer).

### Parameters

- Each parameter is written as `TYPE ARG_NAME`; parameters are separated by commas.
- When a function takes no parameters, the parameter list may be written as `()` or `(void)` — the two are synonyms; such a function must not be called with any argument.
- A parameter is a local variable inside the function: its scope is the function body only, and it never interferes with same-named variables outside.
- Array parameters are treated as pointers: both `int a[]` and `int a[10]` are equivalent to `int *a`, so `sizeof(a)` inside the function yields the pointer width, not the array length.
- A variadic function ends its parameter list with `...` and reads the arguments through `va_start` / `va_arg` / `va_end` from `<stdarg.h>`.
- Default argument values are not supported, nor are reference parameters (such as `int &x`). Arguments are always passed by value; to let a function modify the caller's data, pass a pointer.

### Visibility and export

`public` is Co's only visibility keyword (there is no `private`, and no separate `export`). It carries both the "module interface" and the "dynamic library export" meanings at once:

| Form | Meaning |
| --- | --- |
| `[ public ]` | The function joins its module's external interface and can be used by other modules; when a dynamic library is built, it enters the export table |
| `public` omitted | The function is visible only inside the module that defines it; other modules cannot reach it and it is not exported |

### Definition order and recursion

Co does not require "declare before use": functions within the same module may reference each other **regardless of writing order**, so recursion — and two functions calling each other (mutual recursion) — needs no forward declaration. Writing a forward declaration (a prototype) is still allowed, and the parameter names in a prototype may differ from those in the definition.

### Returning and `return`

- A function whose return type is not `void` must produce a value before it finishes.
- A `void` function may omit `return` entirely, or use a bare `return;` partway through to end early.
- Inside a function body, `return` ends only that function. Top-level (file-scope) `return`, the process exit code, and program execution order are a program-level model; see the "Program Entry and Execution" chapter.
- A `void` function must not `return a value`, and a non-`void` function must not write a bare `return;`. The compiler rejects both.
- A function body may contain several `return` statements (commonly used for early exits on conditions).

### Example

The example covers `public`, `void`, recursion, variadic arguments, and the "used before defined" writing order:

```
#include <stdarg.h>

public int add(int a, int b)
{
    return a + b;
}

void greet()
{
    printf("hello\n");            // void function, no return written
}

int fact(int n)
{
    if (n <= 1)
        return 1;                 // early exit on condition
    return n * fact(n - 1);       // recursion
}

int total(int n, ...)
{
    va_list ap;
    int i;
    int s = 0;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        s = s + va_arg(ap, int);
    va_end(ap);
    return s;
}

// Uses may precede the definitions, as long as they are in the same module
printf("%d\n", add(1, 2));            // 3
greet();                               // hello
printf("%d\n", fact(5));              // 120
printf("%d\n", total(3, 10, 20, 30)); // 60
```

### Relationship to C / C++

| Aspect | Co | Compared to |
| --- | --- | --- |
| Typed parameters, pass by value, `return` | inherited | same as C |
| `static` / `extern` storage classes, function pointers | inherited | same as C |
| Definition order | irrelevant; a call may precede the definition | differs from C (C requires a prior declaration) |
| Omitting the return type | not allowed | same as C (implicit int was removed in C99) |
| Empty parameter list `()` | means "takes no parameters", synonym of `(void)` | differs from C (in C `()` leaves the parameters unspecified) |
| Overloading, default arguments, reference parameters, function templates | not provided | differs from C++ |
| `public` | expresses "module interface / export", not an access region | differs in meaning from C++'s `public` |

> **Compatibility note**: In Co, `()` and `(void)` are synonyms — neither accepts any argument. When migrating from C, code that relies on `f()` leaving the parameters unspecified so that any arguments may be passed does not hold in Co.

### Current boundaries (compiler implementation status)

Since **0.3.0**:

- **`public` on top-level functions / global variables is implemented**: lexer / parser / sema are wired, and when the artifact is a dynamic library the symbol is written into the export table (`DLLExportStorageClass`); `static public` in either order is legal. **Since 0.3.0, `public` also serves as the sole cross-module visibility gate**: symbols not marked `public` cannot be reached by `import` (both named and `*` forms report `cannot import non-public symbol`); non-`public` imported symbols are forced to `InternalLinkage` during codegen. In this batch, `public` still **does not change linkage within the same TU** (non-`public` remains `ExternalLinkage` by default, matching P211); "module-internal" is still expressed by **omitting `public`** (see the "Module" and "import" chapters).
- **Sequential execution of top-level commands is implemented** (entry dispatch, optional `main`, top-level `return` and exit code) — see the "Program Entry and Execution" chapter for the full model.

### Reserved names

`main` and the function names already declared by the standard library (such as `printf`) should not be reused for your own functions. The four Co lifetime protocol names (`__new__`, `__retain__`, `__release__`, `__delete__`) are reserved and must not be used as function names.
