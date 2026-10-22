# Program Entry and Execution

> This chapter describes Co's program entry and execution model. **Entry dispatch, optional `main`, top-level `return` and the exit-code model are implemented since 0.2.0**; the command-line `argv` and cross-module top-level execution timing are still not provided (see "Currently implemented boundary" at the end).

## Top-level statements run in order

A Co program starts at the "top level": in a `.co` file, the commands written **outside** definitions of functions, types, and so on (top-level bare statements) run **in the order they appear in the source**. You do not have to write a `main` function to serve as the entry point.

## With or without `main`

To stay compatible with C-style code, Co accepts both kinds of program:

- **No `main`**: the program simply runs its top-level statements in order, then ends.
- **With `main`**: `main` is an ordinary function, entered only **after all top-level statements have run**.

## Process exit code

The number the program returns to the operating system when it ends (the exit code) is determined as follows:

| Situation | Exit code |
| --- | --- |
| A top-level `return V;` is reached | `V`, and the whole program ends immediately (later top-level statements and `main` are not run) |
| Top level finishes with no `return`, and a `main` exists | `main`'s return value |
| Top level finishes with no `return`, and no `main` exists | `0` |
| A bare `return;` (no value) | `0` |

Key point: **a `return` at the top level ends the whole program** (not some function), so it **skips `main`**; a `return` inside a function body still ends only that function (see the "Function Definition" chapter).

## Command-line arguments

Command-line arguments are read through a single global `argv`:

- `argv` is an array of strings; `argv[0]` is usually the program's own path, followed by the arguments in order;
- the argument count is taken from the length of the `argv` array (Co arrays carry their own length, so no separate count variable is needed);
- `argv` is globally visible and can be read inside functions and methods too.

## When there is no executable entry

If a program has **neither top-level statements nor `main`** (there is genuinely nothing to do) and the target is an executable file: the compiler **issues a warning** and produces an **empty executable that does nothing and exits with code `0`** — the build still counts as successful. This rule does not apply when building a dynamic / static library (a library never requires an entry point).

## Cross-module execution timing

When a program consists of several modules (`.co` files) that `import` one another, the **execution timing and the "run exactly once" rule** for each module's top-level statements are still being finalized together with the module system; see the "Module" chapter.

## Currently implemented boundary

Since **0.2.0**, entry dispatch and exit-code behavior are implemented as specified above:

- When a file contains top-level bare statements, the compiler synthesizes an entry and runs the top level in source order; if a user-defined `main` also exists, `main` is entered automatically after all top-level statements finish, and `main`'s return value becomes the exit code.
- `return V;` at the top level ends the whole program immediately with exit code `V`; a bare `return;` uses exit code `0`.
- A file with neither top-level bare statements nor `main`: when producing an executable, the compiler warns and still emits an empty entry that does nothing and exits with code `0`.
- Pure C style (no top-level bare statements + a `main`) keeps the traditional C entry chaining and does not enter the synthesized entry.

Not yet provided:

- Command-line `argv` is not exposed in this version (no built-in global `argv` array, and no array-carries-own-length infrastructure yet);
- Cross-module top-level execution timing belongs to the module layer and is not open yet (see the "Module" and "import" chapters).
