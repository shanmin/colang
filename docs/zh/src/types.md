# 数据类型

本章承载 Co 语言的**内建类型**——整型、字符型、布尔型、浮点型、指针、数组，以及 `_BitInt`、`typedef` / `typeof` / `auto`、字面量的类型归属、限定符与隐式转换规则。基线与 ISO C（推荐 C23）一致；在此之上 Co 有一项**破坏性扩展**：`.co` 文件内 `long` 恒为 64 位（见「Co 扩展 · `.co` 内的 `long` 为 64 位」）。`struct` / `union` / `enum` 见「struct / union / enum」章节，`class` 见「class」章节。

以下宽度均为 **Windows x64** 实测值；Linux x64 下指针为 8、`long` 为 8（target 原生 LP64），其余不变。

## 语法

```
类型 ::=  整型  [ 限定符 … ]
       |  浮点型  [ 限定符 … ]
       |  void  [ 限定符 … ]
       |  类型  *  [ 限定符 … ]              // 指针
       |  类型  [ 常量表达式 ]                // 数组（长度可为空 → 柔性数组 / 推导）
       |  ( struct | union | enum ) [ TAG ] { … } | TAG        // 派生类型，见对应章节
       |  typeof ( 类型 | 表达式 )
       |  typeof_unqual ( 类型 | 表达式 )
       |  auto                                  // 由初始化器推导（C23）
       |  _BitInt ( 整数常量 )
```

整型说明符可任意组合、顺序不限，缺省宽度按 `int`：

| 写法 | 等价类型 |
| --- | --- |
| `char` / `signed char` / `unsigned char` | 独立类型（`char` 与 `signed char` 宽度同为 1） |
| `short` / `short int` / `signed short` / `unsigned short` | `short` / `unsigned short` |
| `int` / `signed` / `signed int` / `unsigned` | `int` / `unsigned int` |
| `long` / `long int` / `signed long` | `long` |
| `long long` / `long long int` / `unsigned long long` | `long long` / `unsigned long long` |
| `_Bool` / `bool` | 同一类型（`bool` 是 Co 的推荐拼写） |

限定符：`const`、`volatile`、`restrict`、`_Atomic`。

## 宽度与对齐

| 类型 | `.c` / `.h` | `.co` |
| --- | --- | --- |
| `_Bool` / `bool` | 1 / 1 | 1 / 1 |
| `char`、`signed char`、`unsigned char` | 1 / 1 | 1 / 1 |
| `short`、`unsigned short` | 2 / 2 | 2 / 2 |
| `int`、`unsigned int` | 4 / 4 | 4 / 4 |
| **`long`、`unsigned long`** | **4 / 4** | **8 / 8** |
| `long long`、`unsigned long long` | 8 / 8 | 8 / 8 |
| `float` | 4 / 4 | 4 / 4 |
| `double`、`long double` | 8 / 8 | 8 / 8 |
| `wchar_t` | 2 / 2 | 2 / 2 |
| `char8_t` | 1 / 1 | 1 / 1 |
| `char16_t` | 2 / 2 | 2 / 2 |
| `char32_t` | 4 / 4 | 4 / 4 |
| 一切指针、`void*` | 8 / 8 | 8 / 8 |
| `enum` | 4 / 4 | 4 / 4 |
| `size_t`、`ptrdiff_t`、`intptr_t` | 8 / 8 | 8 / 8 |
| `_BitInt(N)` | `ceil(N/8)` / 见下 | 同左 |

表中格式为「`sizeof` / `_Alignof`」。`struct` / `union` 的宽度按成员布局与对齐计算，见对应章节。

## 说明

（本节只描述对外的语义与行为；内部实现机制不在本文范围。）

### 整型

`char`、`short`、`int`、`long long` 的宽度与 ISO C 一致；`long` 是唯一在 `.co` 与 `.c` 之间取值不同的类型（见下文专节）。`signed` 与 `unsigned` 单独使用即 `int` / `unsigned int`。

- `char` 为**有符号**（`char c = -1; printf("%d", c)` 得 `-1`），与 MSVC / clang 的 Windows 默认一致。需要无符号语义请显式写 `unsigned char`。
- 无符号运算按模回绕：`unsigned v = 0u - 1u;` 得 `4294967295`。
- 有符号溢出仍是未定义行为，语言层不做饱和或回绕承诺。
- `<stdint.h>` 的 `int8_t` … `int64_t` / `uint64_t` 按上表宽度可用；`<stddef.h>` 的 `size_t` / `ptrdiff_t` / `nullptr_t` 均为指针宽度的 8 字节。

### 字符型与布尔型

`wchar_t` 为 **2 字节无符号**（对齐宿主 CRT 的宽字符语义，Windows UTF-16 码元）。`char8_t` / `char16_t` / `char32_t`（C23 / C11）分别为 1 / 2 / 4 字节且**恒无符号**。

`bool` 与 `_Bool` 是**同一类型**：占 **1 字节**，`false == 0`、`true == 1`，任何赋值都归一化为 `0` 或 `1`（`bool b = 5;` 得 `1`）。两者是同一类型的两种拼写，**不可在 `_Generic` 里并列为两条关联**。

### 浮点型

`float` 4 字节、`double` 8 字节。`long double` 语法接受、可参与运算并支持 `%Lf` 打印，但**宽度与 `double` 相同（8 字节）**——当前不提供商位扩展精度。

- 浮点型**不接受 `signed` / `unsigned`**（与 ISO C 一致）：`signed float`、`unsigned double`、`unsigned long double` 一律拒收，诊断为 `'float' cannot be signed or unsigned` 一类；诊断把 `long double` 拼作 `double`。
- 浮点字面量只认 `f` / `F` / `l` / `L` 四个后缀，且**大小写不得混用**（`1.0lF` 拒收）。`1.0d` 不是合法后缀。

### 任意宽度整数 `_BitInt(N)`

C23 的 `_BitInt(N)` 已支持，`N` 取 1..65535：

- `sizeof(_BitInt(N)) == (N + 7) / 8`。
- `_Alignof` 随宽度分档：`N ≤ 8` → 1，`N ≤ 16` → 2，`N ≤ 32` → 4，更宽一律 8（实测 `_BitInt(40)` 为 size 5 / align 8、`_BitInt(128)` 为 16 / 8）。
- 默认 `signed`，可写 `unsigned _BitInt(N)`。运算按指定位宽截断（`_BitInt(7)` 的 `100 + 27` 得 `-1`）。
- 与 `int` 混合运算时结果取 `int`（`_BitInt(N)` 的整数等级低于 `int`）。
- 需要 128 位整数请写 `_BitInt(128)`；`__int128` **不是**本语言的拼写。

### 指针与 `void`

一切对象指针宽度同为 8。`void*` 可与任何对象指针**隐式互转**，无需强转（`int v = 5; void *p = &v; int *q = p;` 直接通过）。`nullptr_t`（`<stddef.h>`）与 `void*` 是**同一类型**的两种拼写，故不可在 `_Generic` 里并列为两条关联；`NULL` 与 `nullptr` 均可用于空指针初始化与比较。函数指针可作变量、形参、返回值（`int (*f)(int, int) = add; f(2, 3);`）。

### 数组

`int a[10];` 的 `sizeof` 为元素宽度 × 长度；多维数组按行主序连续存放（`int m[3][4]` 为 48）。字符串字面量是**含尾部 `\0` 的字符数组**：`sizeof("abc") == 4`、`sizeof("") == 1`。

- `char b[4] = "hi";` 余下元素补零；`char c[2] = "hi";` 允许无 `\0` 截断。
- **数组作为函数形参退化为指针**：无论写成 `int f(int a[4])` 还是 VLA 形参 `int f(int n, int a[n])`，函数体内 `sizeof(a)` 都得 8（指针宽度），不是元素总宽。
- 变长数组（VLA）语法接受，`sizeof(a)` 按**求值时**长度表达式的当前值计算（与 ISO C 一致）：`int n = 5; int a[n];` 后把 `n` 改成 9，`sizeof(a)` 即得 36。数组对象**不随 `n` 扩容**，`n` 变大后访问越界仍是未定义行为。
- 柔性数组成员（`char data[];`）见「struct / union / enum」章节。

### 类型别名与推导

- `typedef` 与 ISO C 一致，可用于函数指针、数组、结构体；`typedef` 名参与类型拼写还原（诊断里保留别名形）。
- `typeof(x)`（C23）取表达式或类型的类型，数组、结构体均可：`typeof(a) b;` 得到同维数组；`typeof` 的产物是**类型名**，可直接写进 `sizeof(typeof(e))` 与 `_Alignof(typeof(e))`。`typeof_unqual(x)` 给出剥离顶层限定符后的同一类型。
- `auto`（C23）由初始化器推导：`auto x = 1;` 得 `int`、`auto p = &v;` 得指针、`auto n = 3.5;` 得 `double`、`auto f = 2.0f;` **保留 `float`**（不做默认提升）。当前不支持 `auto *p = &v;` 这种把 `*` 写进声明符的形式，指针请直接写 `auto p = &v;`。

### 字面量的类型

整数字面量按 C §6.4.5.2 的候选序列取**第一个能容纳该值**的类型；后缀决定候选集。

| 字面量 | `.c` / `.h` | `.co` |
| --- | --- | --- |
| `1` | `int`（4） | `int`（4） |
| `1u` | `unsigned int`（4） | `unsigned int`（4） |
| `1L` / `1l` | `long`（4） | `long long`（8） |
| `1UL` | `unsigned long`（4） | `unsigned long long`（8） |
| `1LL` / `1ULL` | `long long` / `unsigned long long`（8） | 同左 |
| `3000000000`、`0xffffffff`、`0x100000000` | 8 / 4 / 8 | 同左 |
| `1wb` / `1ub` | `_BitInt` 位精确字面量（1） | 同左 |

要点：

- 无后缀十进制超 `int` 时升到 `long`（`.c` 4 字节装不下即跳 `long long`），故 `sizeof(3000000000) == 8`。
- 八 / 十六进制同样先试无符号候选，故 `sizeof(0xffffffff) == 4`（`unsigned int`）。
- `.co` 档里 `long` 与 `long long` 同为 64 位，候选序列去重后 `1L`、`1UL` 直接落 64 位。
- 浮点字面量默认 `double`：`1.0`、`.5`、`1.0L` 皆 8 字节；`1.0f` / `1.0F` 为 `float`。
- 字符常量 `'a'` 类型是 **`int`**（`sizeof('a') == 4`），前缀形如 `L'a'` → `wchar_t`（2）、`u'a'` → `char16_t`（2）、`U'a'` → `char32_t`（4）。
- 位精确字面量的后缀**必须写完整** `wb` / `ub`：`sizeof(1wb) == 1`、`sizeof(255wb) == 2`；写漏的 `1w`、`0x1w` 按**非法后缀拒收**（`invalid suffix 'w' on integer constant`），不按普通 `int` 放行。整型后缀的大小写同样须**成对一致**：`1lL`、`1wB` 拒收，`1ll`、`1llu`、`1uwb`、`1UWB` 接受。
- 字符串前缀 `u8"…"` / `u"…"` / `U"…" / L"…"` 分别以 `char8_t` / `char16_t` / `char32_t` / `wchar_t` 为元素，宽度为（码元数 + 1）× 元素宽度。

### 限定符

`const` 有真实只读语义（写 `const` 对象即报错）。`volatile`、`restrict`、`_Atomic` 参与**类型拼写与相容性判定**，但当前**不改变代码生成**——`volatile` 不阻止优化、不保证易失访问的顺序性；需要原子操作请调用 `<stdatomic.h>` 的 `atomic_load` / `atomic_store` 等函数，它们生成真正的原子指令。`register` 接受但为 no-op。

### 隐式转换、提升与算术转换

- **整型提升**：`char` / `short` 等窄整型在运算前先提升为 `int`（能容下则 `int`，否则 `unsigned int`）。故 `(char)100 + (char)100` 得 `200`、`short a = 30000; a + a` 得 `60000`，均不溢出。
- **常用算术转换**：整数侧取更宽者，同宽时无符号侧支配——`unsigned + int` 得 `unsigned int`，`int + long long` 得 `long long`；浮点侧 `float` 与 `double` 混合取 `double`。
- **显式强转** `(T)e` 截断取值：`(char)300` 得 `44`、`(unsigned char)-1` 得 `255`；浮点转整数**向零截断**：`(float)3.99` 得 `3`、`(double)-2.7` 得 `-2`。
- **赋值**结果的类型是左操作数类型（`float f; sizeof(f = f + d)` 为 4）。
- 指针与整数可互相强转；`_Generic` 按类型精确分派，`bool` / `_Bool`、`.co` 内的 `long` / `long long` 因同型而不可并列。

### `sizeof`、`_Alignof` 与 `_Alignas`

- `sizeof` 可用于**类型**与**表达式**；作用于变量参与的算术表达式时，结果按表达式的**实际类型**计算：`double d; float f;` 下 `sizeof(d + d)`、`sizeof(-d)`、`sizeof(1 + d)`、`sizeof(d / f)` 均得 `8`，`sizeof(f + f)` 得 `4`，`sizeof(c + c)`（`char c;`，经整型提升）得 `4`。
- `sizeof` 作用于结构体返回含填充的总宽，柔性数组贡献 0；作用于字符串含 `\0`。
- `sizeof(void)` 得 **1**、`sizeof(函数)` 得 **1**——这是为兼容 GNU 源码而保留的扩展，ISO C 并不定义。
- 复合字面量可作操作数：`sizeof (int){1}` 得 `4`、`sizeof (double){0}` 得 `8`、`sizeof (int[3]){0}` 得 `12`。括号内是**类型名**时仍按类型宽度计算，故 `sizeof (int) - 1` 是 `sizeof(int) - 1`（得 `3`），不是复合字面量。
- `_Alignof(T)` 的**类型名**形式与 ISO C 一致；同时接受**表达式**形（与 clang 同口径的 GNU 扩展）：`_Alignof d`、`_Alignof(d + 1)` 均按操作数类型取对齐。无括号时按**一元表达式**结合，故 `_Alignof d + 1` 是 `(_Alignof d) + 1`（得 `9`）。`_Alignof` / `__alignof__` / `alignof` 三种拼写等价。
- `_Alignas(N)` 可用于变量与结构体成员，实际对齐取「自然对齐与 `N` 的较大者」：`_Alignas(32) char buf[8];` 使 `buf` 落址 32 字节边界，`struct { char c; _Alignas(16) int i; };` 的 `sizeof` 为 32。`__attribute__((aligned(N)))` 按同等效果参与对齐（详见「属性」章节）。

### Co 扩展 · `.co` 内的 `long` 为 64 位

这是 Co 相对 C 的**唯一一处宽度断代**，也是本章唯一的破坏性差异：

- 在 **`.co` / `.CO`** 文件内，`long` / `unsigned long` 恒为 **8 字节**（与 `long long` 等宽）；在 **`.h`、`.c`** 及一切其它源文件内保持 **target 原生**（Windows 4 / Linux 8）。组合拼写一同变宽：`.co` 内 `long int`、`signed long` 同为 8 字节。
- 判定按**每一个声明自身的来源文件**，不按「整个编译单元」：`.co` 里 `#include` 一个 `.h`，头文件中的 `long` 仍是 4 字节。因此**共享头里的结构体布局在 `.co` 与 `.c` 两侧逐字一致**，不存在同一份头文件在两种源里算出不同 `sizeof` 的情况。
- 宏展开出的 `long` 与 `L` 字面量按**调用点**口径：共享头里 `#define BIG 0x100000000L`，在 `.co` 内展开即按 64 位算，不会误报溢出。
- 由 `.co` 的 64 位 `long` 派生的两条书写纪律：
  - **调用外部（C）接口时一律写显式宽度**：对方返回 32 位（Win32 的 `DWORD` / `LONG`）就写 `int` / `unsigned int`，确实返回 64 位才写 `long` 或 `long long`。语言层不设「含 `long` 的外部声明拒收」这类检查——编译器看不到外部真实签名，无权判定对错。
  - `.co` 内 `long` 与 `long long` 是同一类型，`_Generic(x, long: 1, long long: 2)` 会因重复关联而报错；诊断信息里 `.co` 的 `long` 可能拼作 `long long`。
- 需跨 `.co` / `.c` 保持一致的取值场合，推荐直接用 `int32_t` / `int64_t`。

> **兼容性告知（`long` 相关）**：`.co` 内 `printf("%ld", v)` 会传 8 字节而 `%ld` 只读 4 字节，语言层不做格式串检查，请改用 `%lld` 或 `PRId64` 类拼写；`.co` 内 `#include <limits.h>` 取到的 `LONG_MAX` 仍是 4 字节口径（`2147483647`），判 64 位 `long` 的值域请改用 `INT64_MAX`。这两点属数据模型宏无法覆盖的语言层断代，须由书写纪律承接。

## 当前边界（暂不支持 / 未定稿）

- **`_Complex` / `_Imaginary` 与 `2.0i` 虚数字面量不实现**（写 `_Complex` 直接诊断）。
- **`__int128` 不是本语言拼写**，用 `_BitInt(128)`。
- **`long double` 无扩展精度**，等价 `double`。
- **`volatile` / `restrict` / `_Atomic` 说明符不进代码生成**，只影响类型拼写与相容性。
- **`auto *p = &v;` 不支持**（`auto` 后再接 `*` 声明符形式）；改 `auto p = &v;`。
- **裸标签作类型（`Point p;`）只在 `.co` 启用**，`.c` 内仍须写 `struct Point p;`；详见「struct / union / enum」章节。
- **GNU 语句表达式 `({ ... })` 不支持**（如 `x = ({ int t = 3; t + 4; })`；C 标准无此语法，属厂商扩展）。写即诊断 `GNU statement expression '({ ... })' is not supported`，并将整段花括号跳过，一处写法只报一条。需要同类效果时改写成普通语句或辅助函数。
- **GNU 范围指定符 `[a ... b]` 不支持**（如 `int arr[4] = { [0 ... 2] = 7 };`、`t.a[1 ... 5] = 9`；C 标准无此语法，属厂商扩展）。写即诊断 `GNU range designator '[a ... b]' is not supported`，上界表达式被跳过丢弃，其后的 `]` / `=` / 初值照常解析，一处写法只报一条。需要同范围填充时改成逐项指定初始化器（如 `[0]=7, [1]=7, [2]=7`）或循环赋值。

## 保留名

`bool`、`true`、`false`、`nullptr`、`constexpr`、`typeof`、`typeof_unqual`、`_BitInt`、`alignas`、`alignof`、`static_assert`、`thread_local`、`class`、`new`、`this`、`public`、`import`、`module` 等是 Co 的**关键字**，不可作标识符（`bool` / `true` / `false` 即便在 `.c` 内也保留，`int bool = 1;` 直接诊断）。C++ 的 `decltype` **不是** Co 的关键字、也未实现——它只是一个普通标识符，取表达式类型请用 `typeof(e)`。其余拼写（`int`、`long`、`float`、`_Bool`、`_Complex`、`_Imaginary`、`_Atomic`、`_Generic`、`_Noreturn`、`wchar_t`、`char8_t`、`char16_t`、`char32_t`、`restrict`、`volatile`）按 C 的语言层规则保留给实现，程序内不得重载其含义。
