# class

## Syntax

```
[ public ] class CLASS_NAME
{
	// constructor
	CLASS_NAME()
	{
	}

	// destructor
	~CLASS_NAME()
	{
	}

	// member variable
	[ public ] [ static ] TYPE VAR_NAME;

	// member method
	[ public ] [ static ] TYPE FUNCTION_NAME( [ TYPE VAR_NAME , ... ] )
	{
	}

	// property (a get/set accessor pair; each accessor may carry public/private, default public)
	[ public ] TYPE PROPERTY_NAME
	{
		[ public | private ] get ;   // or get { ... }   field is available in the body
		[ public | private ] set ;   // or set { ... }   value is the written value; field is available
	}
}
```

## Description

### `.co` source files only

class is a type form introduced by Co — C has neither this keyword nor the corresponding semantics — so `class` can only be defined in `.co`/`.CO` source files. A class written in `.c` or `.h` is rejected:

```
$ colang app.c
app.c:1:1: error: 'class' is only allowed in .co source files
```

What counts is **the file the `class` keyword actually comes from**: a `.h` pulled in by `#include` from a `.co` source gets no exemption — a class definition in that header is still rejected, and the diagnostic points at the header's own name and line number.

(No cascade: the whole `class … { … }` block is skipped in one go, so one definition yields exactly one diagnostic.)

### Boundary with struct

class is Co's **reference type** (data + behavior), in contrast to struct (a value type, pure data, C-compatible):

| | struct | class |
| --- | --- | --- |
| Kind | value type | reference type |
| Contents | member variables only | member variables + methods + constructor/destructor + static members |
| Storage | stack value / heap handle | heap (class is only ever created on the heap) |
| Reclamation | follows scope | managed automatically by Co; manual release is normally not needed |
| Role | C-style data aggregate | encapsulation unit for OOP |

Co's object model is deliberately "simplified": class provides encapsulation and methods but does not aim for full C++ feature coverage (see "Current boundaries").

### Creation and ownership

A class instance can only be created on the heap. After obtaining an object via `new`, it must be held by some variable or handed off as a return value; if nothing holds a newly created object, it becomes unreachable and leaks.

A class variable holds a **reference (handle)** to the object: the class name itself already denotes that reference, so you write `Counter c`, **not `Counter*`** (which would be a pointer-to-pointer and is not allowed in Co). Thus `Counter c = new Counter();` uses `c` to hold the newly created heap object.

### Creating and using

A minimal end-to-end example (constructor/destructor, methods, `this`, `static` with `::`, and a member without `public`):

```
class Counter
{
    Counter()                { count = 0; }              // constructor
    ~Counter()               { }                         // destructor
    public void inc()        { count = count + 1; }
    public int  value()      { return this.count; }      // this refers to the current object
    public static int twice(int x) { return x * 2; }     // static member
    int count;                                           // no public -> visible only inside the class
}

Counter c = new Counter();            // created on the heap, held by c (a reference/handle)
c.inc();                              // call instance method with a dot
c.inc();
printf("%d\n", c.value());            // prints 2 (count, visible only inside the class, read via a public method)
printf("%d\n", Counter::twice(21));   // static accessed via class name + ::, prints 42
// once c leaves scope the object is reclaimed automatically; no manual release needed
```

### Members and methods

- **Instance methods** refer to the current object via the implicit `this` and read/write its own data.
- **static members / methods** belong to the class itself rather than any instance; they exist within the module in a zero-initialized state and are accessed through the class name with the `::` operator, e.g. `C::m()`. The `static` here modifies **class members** only; it is not the file-scope `static` of C (which restricts a symbol to a single translation unit). Module-internal visibility in Co is expressed by **omitting `public`** (see the "Module" chapter).
- **Accessibility**: a member/method is exposed based on `public` — one that is not marked `public` (or is explicitly marked `private`, which is exactly equivalent at this level) is visible only inside the class; only those explicitly marked `public` are visible from outside. At the member/method level, `public`/`private` do **not further restrict access between regions inside the class**; the only place in Co that actually enforces an access restriction is the `get`/`set` accessors of a property (see the "Properties" section below).

### Properties

A property looks like a field but is really a pair of accessor functions: reading it runs `get`, writing it runs `set`, and it is spelled just like an ordinary member (`obj.p` to read, `obj.p = x` to write). Properties are declared in the class body as `[public] TYPE NAME { accessors … }`:

```
class Box
{
    int _a;                              // backing field used by the explicit accessors
    Box() { _a = 0; }

    public int a {                       // both readable and writable (property is public)
        get { return _a; }               // explicit get: body refers to _a
        set { _a = value; }              // explicit set: value is the implicit parameter
    }

    public int b { get; set; }           // auto property: the compiler synthesizes a hidden field
    public int c { get; }                // read-only: only get, writing from outside errors
    public int d { set; }                // write-only: only set, reading from outside errors
    public int e { get; private set; }   // read-only to outsiders: private set, writable only inside the class
    public int score { get; set { field = value < 0 ? 0 : value; } }   // mixed: auto get, set reads/writes the synthesized backing field via field
}
```

- **Accessor forms**: a `{ … }` body is an explicit accessor (its body refers to a field you declared, e.g. `_a`); a bare `;` (no body) is an **auto property** — the compiler synthesizes a hidden backing field, which participates in the object layout and starts at `0` (to set a custom initial value, use an explicit accessor + your own field + an assignment in the constructor instead).
- **Backing field and `field`**: an auto property (`get;`/`set;`) gets a compiler-synthesized **backing field** to hold its data. As long as the property **keeps at least one auto part** (at least one bare `get;`/`set;`), the other accessor may be an explicit body that reads/writes that backing field directly via the contextual keyword **`field`** — e.g. `public int score { get; set { field = value < 0 ? 0 : value; } }`. `field` is a *contextual* keyword, not reserved: it means the backing field only inside property accessors and is an ordinary identifier elsewhere.
  - `field` is valid only in the accessor of an **auto-implemented** property; using it where both accessors give explicit bodies (no auto part = no backing) is rejected: `keyword 'field' is only available in an auto-implemented property`.
  - **Versus a same-named member**: if the class also declares a member named `field`, a bare `field` in an accessor body still means the **backing field**; write **`this.field`** to reach that member.
- **Read-only / write-only**: giving only `get` makes it read-only — writing reports `property '…' is read-only (no setter)`; giving only `set` makes it write-only — reading reports `property '…' is write-only (no getter)`.
- **Compound assignment / increment-decrement**: `obj.p += x` expands to "get, compute, set"; `obj.p++` / `++obj.p` / `obj.p--` / `--obj.p` dispatch the same way (the postfix form yields the old value before the update, the prefix form the new value). Both require the property to have `get` and `set` present and each visible, otherwise a read-only / write-only / private diagnostic is reported as appropriate.
- **No address-of**: a property is a pair of accessor functions with no storage address of its own, so `&obj.p` is rejected: `cannot take address of property 'p'`.
- **Per-accessor visibility** (orthogonal to the property's overall `public`): each accessor may be individually prefixed with `public` or `private`; the **default is `public`**. An accessor marked `private` **can only be called from within the class that defines it** (including reads/writes through other instances of the same class inside the class); using it from outside is rejected—
  - a `private` `set`: writing `obj.p = x` from outside reports `setter of property 'p' is private`, so the property looks **read-only** to outsiders;
  - a `private` `get`: reading `obj.p` from outside (including compound assignment and increment/decrement) reports `getter of property 'p' is private`, so the property looks **write-only** to outsiders.

  This differs from the rule that "`private` at the module/member level is just an explicit spelling of *omitting* `public` and adds no restriction"—**`private` on a property accessor does enforce a real access restriction**; it is the only place in Co's visibility rules that carries enforcement.
- **Across modules**: after importing another module's `public class`, its properties and their per-accessor `public`/`private` behave the same — writing a private setter or reading a private getter from outside (including the host module) is rejected consistently.

> Current boundaries: properties of handle (class-reference) type are **now supported** — reading (`get`) returns a **borrowed** reference, and writing (`set`) maintains reference counting by "retain the new value, then release the old one", sharing safely with other owners. Taking a property's address (`&obj.p`) is now rejected with a diagnostic, and increment/decrement (`obj.p++` / `obj.p--` and their prefix forms) is now supported.

### Lifetime (observable behavior)

Co manages the memory of class objects automatically. The following rules are observable by users:

- **No manual release**: when an object is no longer held by any variable and the relevant scope ends, it is reclaimed automatically.
- **No dangling, no double free**: handing an object to another variable never causes the same object to be freed twice, nor does it produce a dangling reference into freed memory.
- **When destruction runs**: before an object is reclaimed, its destructor runs; members are still readable at that point, so it can be used to release external resources.
- **Cycles are not guaranteed to be collected**: if two objects reference each other forming a cycle, automatic reclamation of that group is not currently guaranteed; the policy is still being finalized.
- **Handing over an owned handle is safe**: returning an object that is already held by a local variable or a member is **legal**; the compiler automatically adds one reference for it, so after the return the object is safely shared with its owner and is not released prematurely.

> **Compatibility note**: Co's class memory management is neither like C++ (manual RAII or explicit release) nor like garbage collection (GC does not guarantee collecting cycles). Do not carry over intuition from either.

### Constructor and destructor

- **Constructor** `CLASS_NAME()`: runs when the object is created, for initialization. It shares the class name and has no return type.
- **Destructor** `~CLASS_NAME()`: runs before the object is reclaimed, for releasing resources. It is prefixed with `~` and has no return type.

### Current boundaries (not yet supported / being finalized)

The following are currently unsupported or still being finalized; using them yields a compiler diagnostic:

- Inheritance, virtual functions, and polymorphism are not part of the current baseline.
- Cycles are not guaranteed to be reclaimed automatically; to break one you can use `delete` to destroy explicitly.

> The following are now supported (previously listed as boundaries, now open): properties of handle (class-reference) type (reading returns a borrowed reference, writing retains-new/releases-old by reference counting); member and parameter types are no longer limited to primitives — a `class` handle, `struct` / `union` / array can all be used as members or parameters (the value-semantics embedding, borrow-parameter, and host-ownership reference-counting rules are covered in the relevant sections); explicit `delete` is available; a `public class` brought in via `import` can be consumed across modules (see the "Module" chapter).

### Reserved names

`__new__`, `__retain__`, `__release__`, and `__delete__` are reserved by the language and must not be used to name variables, functions, classes, or members.
