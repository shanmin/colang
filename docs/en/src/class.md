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
}
```

## Description

(This section describes only the externally observable semantics and behavior of class. Its internal implementation mechanism is outside the scope of this document.)

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
- **Accessibility**: `public` is Co's only visibility keyword (there is no `private`, and no separate `export`). Members/methods that are not marked `public` are visible only inside the class; only those explicitly marked `public` are visible from outside.

### Lifetime (observable behavior)

Co manages the memory of class objects automatically. The following rules are observable by users:

- **No manual release**: when an object is no longer held by any variable and the relevant scope ends, it is reclaimed automatically.
- **No dangling, no double free**: handing an object to another variable never causes the same object to be freed twice, nor does it produce a dangling reference into freed memory.
- **When destruction runs**: before an object is reclaimed, its destructor runs; members are still readable at that point, so it can be used to release external resources.
- **Cycles are not guaranteed to be collected**: if two objects reference each other forming a cycle, automatic reclamation of that group is not currently guaranteed; the policy is still being finalized.
- **Writes the compiler rejects**: for example, returning an object that is already held by a local variable.

> **Compatibility note**: Co's class memory management is neither like C++ (manual RAII or explicit release) nor like garbage collection (GC does not guarantee collecting cycles). Do not carry over intuition from either.

### Constructor and destructor

- **Constructor** `CLASS_NAME()`: runs when the object is created, for initialization. It shares the class name and has no return type.
- **Destructor** `~CLASS_NAME()`: runs before the object is reclaimed, for releasing resources. It is prefixed with `~` and has no return type.

### Current boundaries (not yet supported / being finalized)

The following are currently unsupported or still being finalized; using them yields a compiler diagnostic:

- Parameter types of methods and the types of member variables are currently limited to primitive types; using a class as a member type, or nested objects, are not yet open.
- Inheritance, virtual functions, and polymorphism are not part of the current baseline.
- Explicit release (`delete`) and the cycle-reclamation policy are not finalized.

### Reserved names

`__new__`, `__retain__`, `__release__`, and `__delete__` are reserved by the language and must not be used to name variables, functions, classes, or members.
