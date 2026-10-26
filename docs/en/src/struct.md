# struct / union / enum

This chapter covers the three **value types** in Co — `struct` (data aggregate), `union` (shared storage), and `enum` (named integer constants) — with their ISO C baseline plus two Co-only extensions: **`public` cross-module visibility** and **bare tag as type** (only in `.co`). For the reference type `class`, see the "class" chapter.

## Syntax

```
[ public ] ( struct | union ) TAG
{
	member-declaration …
};

[ public ] enum TAG
{
	CONST [ = const-expression ] , …
};

member-declaration ::=  type  name  [ : bit-width ]  ;
	                    |  ( struct | union ) [ TAG ] { member-declaration … } ;    // anonymous member
	                    |  type  name  []  ;                                        // flexible array member (last position)
```

Variable and parameter declarations:

```
( struct | union | enum ) TAG  name [ = initializer ] ;   // keyword form (ISO C compatible, `.c` and `.co`)
TAG                         name [ = initializer ] ;      // bare-tag form (`.co` only)
```

Initializers: positional list `{ v0, v1, … }`; designated initializer `{ .member = value , … }` (may be out of order; unlisted members are zero-filled); array subscript `[ index ] = value`. Designators may **chain downward** into nested members/elements: `.a[2].b`, `[1][2]` (C11 §6.7.9 designator sequences).

| Element | Meaning |
| --- | --- |
| `TAG` | Tag name; usable as a type identifier within the TU (bare-tag form, `.co` only) |
| `public` | Cross-module visibility gate; without it the type stays module-internal and cannot be `import`-ed |
| member-declaration | Order in source equals layout order (subject to ISO C alignment rules except within a bit-field run) |
| `: bit-width` | Bit field; width must be an integer constant not exceeding the underlying type's width |
| anonymous member | Inline `struct` or `union` with no name; its members are **promoted** into the host |
| `name []` | Flexible array member; must be the last member; `sizeof` excludes its length |

## Semantics

(This section describes the externally-observable semantics; internal implementation mechanisms are out of scope.)

### Positioning

`struct`, `union`, and `enum` are all **value types**: a variable denotes a fixed region of storage, assignment copies bytes, lifetime follows scope, and there are no constructors or destructors. Contrast with `class`, which is a reference type stored on the heap and reclaimed automatically by Co.

### Boundary with class

| | struct | class |
| --- | --- | --- |
| Kind | value type | reference type |
| Contents | member variables only | fields + methods + ctor/dtor + static members |
| Storage | stack value / heap handle | heap (class is always heap-allocated) |
| Reclamation | follows scope | Co automatic memory management |

See the "Boundary with struct" subsection of the "class" chapter for details.

### struct: definition and use

Supports named tags, **forward declarations** (incomplete types), **self-reference** via pointers, positional and designated initializers, and `.` / `->` member access.

```
struct Point
{
	int x;
	int y;
};

struct Node                                // self-referential: member is a pointer to itself
{
	int          value;
	struct Node *next;
};

struct Point a = { 3, 4 };                 // positional
struct Point b = { .y = 7, .x = 3 };       // designated (may be out of order; unlisted fields zero-fill)
struct Point c;                            // uninitialized: holds whatever was on the stack
c.x = 1; c.y = 2;

struct Point *p = &a;
printf("%d %d\n", p->x, c.y);

struct Point d = c;                        // assignment is a byte-for-byte copy
```

### union: definition and use

All members of a `union` **share one storage region**; the size of the union equals its largest member. Only one member is considered "active" at any moment — writing a member makes that member active. Maintaining this invariant is the programmer's responsibility.

```
union Value
{
	int  i;
	char b[4];
};

union Value u;
u.i = 256;
printf("%d\n", u.i);           // 256
u.b[0] = 1;                    // after this write, reading i is no longer meaningful
```

An **anonymous union** placed inside a `struct` has its members **promoted** into the host — you access them via the host directly, with no intermediate name. This is the common tagged-union idiom:

```
struct Tagged
{
	int tag;
	union
	{
		int   i;
		float f;
	};
};

struct Tagged t;
t.tag = 1;
t.i   = 42;                    // anonymous union member promoted to host
```

Conversely, an **anonymous `struct` placed inside a `union`** has its members **promoted** into the host `union` — you access them through the union directly, while each member keeps its own offset inside that `struct`. This is exactly the Win32 `LARGE_INTEGER` shape (`LowPart` / `HighPart` / `QuadPart`):

```
union LongLong
{
	struct
	{
		int lo;
		int hi;                     // offset 4 from the anonymous struct's start
	};
	long long q;                    // overlaps the same 8 bytes as lo / hi
};

union LongLong v;
v.q = 0x0000000200000001LL;         // write all 64 bits at once
printf("%d %d\n", v.lo, v.hi);      // 1 2 (little-endian: low 32 bits in lo, high 32 bits in hi)
v.hi = 3;                           // update only the high 32 bits
printf("%lld\n", v.q);              // 0x300000001
```

`sizeof(union LongLong) == 8`; the promoted `lo` / `hi` and `q` are different views of the same storage. A designated initializer may target a promoted member or a non-first member directly: `union LongLong g = { .hi = 3 };` and `union LongLong h = { .q = 1 };`.

### enum: definition and use

`enum` defines a set of named integer constants. Its underlying type is equivalent to `int` (`sizeof(Color) == sizeof(int)`). Both named (`enum TAG { … }`) and anonymous (`enum { … }`) forms are accepted. A constant may carry an explicit integer value; a constant without an explicit value takes the previous value plus one.

```
enum Color { RED, GREEN, BLUE };                       // 0 / 1 / 2
enum Kind  { A = 1, B = 2, C = 10, D };                // D follows C, so 11

Color c = GREEN;
if (c == 1) { }                                        // equivalent to plain integer comparison
```

An anonymous `enum { … };` places its constant names directly into the ordinary identifier namespace.

### Bit fields

A member followed by `: integer-constant` declares a bit field whose width must not exceed the underlying type's bit width. Bit fields declared back-to-back are packed into the same storage unit in declaration order; overflow starts a new unit. Anonymous bit fields (a bare `: N` with no name) serve as padding.

```
struct Basic
{
	unsigned a : 3;                        // bits 0-2
	unsigned b : 5;                        // bits 3-7
	unsigned c : 8;                        // bits 8-15
	unsigned d : 16;                       // bits 16-31
};                                         // sizeof == 4 (single i32 unit)

struct Mixed
{
	int        header;                     // regular field, 4 bytes
	unsigned   f1 : 1;
	unsigned   f2 : 7;
	int        payload;                    // 4 bytes
};

struct Basic x = {0, 0, 0, 0};
x.a = 7; x.b = 31; x.c = 200; x.d = 0xABCD;
printf("a=%d b=%d c=%d d=0x%x\n", x.a, x.b, x.c, x.d);
```

Bit fields can be read, written, updated via compound assignments (`+=`, `|=`, …), and accessed through pointers (`p->f1 = 1;`). In initializer lists, bit-field values are written into the same storage unit one by one in declaration order (mask-merged), never clobbering neighbouring fields.

### Flexible array member

The last member may be declared as `type name [];`. `sizeof` on the enclosing struct does not count the flexible portion. The usual pattern is a leading length field followed by the flexible array.

```
struct FlexStr
{
	int  len;
	char data[];                    // header only; the payload comes from an external buffer
};

char buf[32];
struct FlexStr *fs = (struct FlexStr *)buf;
fs->len     = 2;
fs->data[0] = 'H';
fs->data[1] = 'i';
fs->data[2] = '\0';
printf("len=%d s=%s\n", fs->len, fs->data);
```

### Value semantics and sizeof

- **Assignment / by-value parameters / returns** all copy the underlying bytes; modifying the copy does not touch the original.
- Structs may be **passed and returned by value**: `int sum(Point a, Point b)` is valid; bare-tag forms of `Point` also work at the call site (`.co` only).
- `sizeof` yields the **total byte size** including internal padding, and includes any bytes occupied by anonymous members of the host. The flexible-array portion contributes 0.
- `sizeof` may appear in a **file-scope constant initializer** (e.g. `int sz = sizeof(Point);`) — it is folded to a constant at compile time, matching the value inside a function.
- `sizeof` applied to a **string literal** yields the **array width including the trailing `\0`**: `sizeof("abc") == 4`; `typeof("abc")` is likewise an array type.
- Array elements, struct members, and pointer members follow ISO C semantics.

### Co extension: `public` and cross-module

`public` is the only gate for **cross-module type visibility**. In an imported module, only `struct` / `union` / `enum` types marked `public` can be consumed by other modules; anything without `public` stays module-internal and is refused by `import`.

**Module `geometry.co`**:

```
public struct Point { int x; int y; };
public union  Val   { int i; char b[4]; };
public enum   Color { RED, GREEN, BLUE };
struct Secret { int raw; };                   // no public → module-internal only
```

**Consumer `main.co`**:

```
import * from "geometry.co";                  // or by name: import Point, Val from "geometry.co";

int main(void)
{
	Point        p = { .x = 1, .y = 2 };       // bare-tag form
	struct Point q = p;                         // keyword form, equivalent to the line above
	return q.x + q.y;                           // 3
}
```

See the "import" chapter for the two `import` forms, path resolution, and cycle detection.

### Co extension: bare tag as type (`.co` only)

Inside a `.co` file, the tag of a `struct` / `union` / `enum` may be used **without the leading keyword** as a type — mirroring C++ style. This is part of Co's "simplified C++" positioning (the same direction as the `bool` decision).

```
struct Point { int x; int y; };
public enum  Color { RED, GREEN, BLUE };

int sz = sizeof(Point);                     // sizeof accepts a bare tag; folded at file scope

int addp(Point a, Point b) { return a.x + b.y; }         // bare tag in parameter list

int main(void)
{
	Point   p  = { 1, 2 };                    // same as struct Point p = { 1, 2 };
	Color   c  = GREEN;                        // enum constant visible in same TU
	Point  *pp = &p;
	return p.x + c + pp->y + addp(p, p) + sz;
}
```

- **Same TU**: any tag already defined in this file may be used bare.
- **Cross module**: `public struct` / `union` / `enum` tags from an imported module may likewise be used bare in the consumer (see the example above).
- **Coexists with the traditional form**: `struct Point a; Point b;` may appear side-by-side in one source file; they refer to the same type.

> **Compatibility note**: bare-tag recognition is **not enabled** in `.c` files — `Point p;` still reports `use of undeclared identifier 'Point'` and you must write `struct Point p;`, exactly as in ISO C. The pure-C mode is unaffected.

## Current boundaries (not yet supported / not finalized)

- **Cross-module enum constant names**: a `public enum`'s *tag* may be used across modules (as a type: `Color c;`), but the *constant names* themselves (e.g. `GREEN`) are not currently visible across modules; consumers still write integer literals. A separate milestone covers this.
- **Inside a submodule**: within an imported module, that module cannot use bare tags pulled in by its own imports. Cross-module bare-tag support applies only at the top-level main source.
- **Incomplete types, diagnosed up front**: when a `struct`/`union` tag that has only a forward declaration (no body) is **allocated by value** — a global or local variable definition, a `struct` field, or an array of incomplete elements — the compiler reports an error directly (since 0.3.1, wording matching clang) instead of letting it crash in the backend. Pointer uses (declarations, parameters, `p->x` member access) and `extern` declarations remain legal with only a forward declaration.
- **`Foo(3);` declaration/expression ambiguity**: when an identifier is simultaneously a tag and appears as the leading token of a call-statement, `.co` re-parses it as a declaration. This is the same class of ambiguity as existing `typedef` names and will be addressed later.
- **Block-scope shadowing**: the granularity of shadowing a tag by an inner variable follows the existing implementation (a simplified merge of the ISO C tag namespace).

## Reserved names

There are no reserved names specific to `struct` / `union` / `enum`. The four lifecycle-protocol reserved names of `class` (`__new__`, `__retain__`, `__release__`, `__delete__`) are covered in the "class" chapter.
