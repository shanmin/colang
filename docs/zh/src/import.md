# import（模块导入）

> 自 **0.3.0** 起，`import` 已实装、用于消费 `.co` 源文件模块。`.obj` / `.dll` / `.so` 作可导入模块仍待后续里程碑。
>
> **位置与目标两条硬限制**：① `import` 语句本身**只能写在 `.co`/`.CO` 源文件**里，`.c` 及一切其它源文件出现即报错（见「谁可以写 `import`」）；② 导入**目标**只允许 `.co`/`.CO` 模块，`.c`/`.h` 永久拒收、`.bc` 属已规划未实装（见「目标文件后缀白名单」）。

## 用途

`import` 从其他 `.co` 源文件导入名称，用于组织大工程与划定接口边界，避免 C/C++ 头文件分离带来的重复定义与同步成本。

## 语法

两种**互斥**形态：

```
import name [as alias], … from ("file" | module_name) [as ModAlias];
import * from ("file" | module_name) [as ModAlias];
```

| 成分 | 含义 |
| --- | --- |
| `name` | 要导入的具名符号（须在被导入模块中已标 `public`） |
| `name as alias` | 给导入的名称起本地别名 |
| `…, …` | 逗号后可继续追加多个 `name [as alias]` 项 |
| `*` | 导入被导入模块中所有 `public` 名称 |
| `"file"` | 字符串字面量形式，支持相对路径 / 跨目录 / 特殊字符 |
| `module_name` | 简化标识符形式（无引号、无后缀、无分隔符），仅同目录内解析 |
| `ModAlias` | 给来源模块起的本地别名，用于**限定访问** `ModAlias.f()` / `ModAlias.x`（自 **0.3.2** 实装）；缺省时以模块文件名为默认限定名 |

## 文件名与路径解析

- **双形式**：`"file"` 与 `module_name` 二选一。
  - 字符串字面量：支持跨目录（相对或绝对路径），扩展名可省略（无后缀自动补 `.co`）。
  - 简化标识符：等价于 `from "<name>.co"`，仅在**引用方文件所在目录**查找。
- **路径基准**：以**引用方文件所在目录**为基准解析相对路径，而非当前工作目录。

## 谁可以写 `import`：只限 `.co` 源文件

`import` 是 Co 面的语法，只允许出现在 `.co`/`.CO` 源文件里：

- **`.c` 文件写 `import`**：编译期报错 `'import' is only allowed in .co source files`，不静默接受、也不降级为「只识别不加载」。
- **判定按语句实际所在文件**：`.co` 主源 `#include` 进来的头文件里若写了 `import`，同样报错、且诊断定位到**该头文件**的行列——`.h` 恒为 C 口径面，不因被 `.co` 包含而放宽。
- **纯 C 档（`.c`）取跨文件声明的手段只有 `#include`**；`.c` 也无法消费任何 `.co` 模块（含 `winapi.co`）。

## 目标文件后缀白名单

导入目标只允许 `.co`/`.CO`：

| 目标写法 | 现状 | 诊断 / 行为 |
| --- | --- | --- |
| `"mod.co"` / `"mod.CO"` | ✅ 可用 | — |
| `"mod"`（无后缀） | ✅ 自动补 `.co` | — |
| `"mod.bc"` | ⏳ 已规划、未实装 | `.bc modules are planned but not implemented yet` |
| `"mod.dll"` / `".lib"` / `".so"` | ⏳ 链接期消费预留位 | `binary library import (.dll/.lib/.so) is not supported yet` |
| `"mod.c"` / `"mod.h"` | ❌ 永久拒收 | `must be a .co/.bc module; C source files and headers cannot be imported (use #include)` |
| 其它后缀（如 `.txt`、`.obj`） | ❌ 拒收 | `must be a .co/.bc module (unsupported extension '…')` |

两类目标性质不同、不得混淆：`.co` 是**源码合并**（提供类型、原型与定义），将来的 `.bc` / `.dll` / `.lib` / `.so` 是**链接期消费**（只提供符号、不提供编译期声明，因而不能替代声明来源）。白名单在**主源与每个被导入模块**两处同样生效，子模块没有豁免。

## 独立模块闭包

自 **0.3.2** 起，每个 `.co` 模块都是**独立的编译闭包**：逐模块各自建类型表与符号表、各自分析，主源只经**导出面**（`public` 名称）看到模块。这带来两条用户可见的保证：

- **同名类型异布局互不干扰**：两个模块各定义同名 `struct Config` 而布局不同，各自 `sizeof` 取自身布局，不存在「后导入者顶替」。
- **ambient 切断**：模块内 `#include`（如 `<stdio.h>`）带入的**环境符号**（`printf` 等）**不进入模块导出面**。宿主即使也 `#include <stdio.h>`，其 `printf` 与模块互不冲突、不再报「导入非 public 符号」。

## 可见性规则

- **`public` 是跨模块可见性的唯一闸门**：未标 `public` 的定义仅在源模块内部可见、不能被任何 `import` 拿到。
- 具名导入非 `public` 符号：编译期报错 `cannot import non-public symbol`。
- `import *` 命中的模块中非 `public` 符号：静默跳过、只导入 `public` 面。
- 限定访问 `ModAlias.x` 命中模块中不存在的或非 `public` 的名称：报错 `no public symbol 'x' in module 'ModAlias'`；若该名在模块里是**类型**（非 `public` 的 `struct`/`enum`/`class`），诊断改按类型口径：`no public type 'X' in module 'ModAlias'`。
- 主源上下文引用一个**存在于符号表但既未通过 `import` 具名引入、又未以限定名访问**的模块符号：编译期报错 `identifier not imported`。
- **模块内部**引用**同模块**的非 `public` 符号：合法、不受限制。

## 主源与子模块同口径：导入类型/常量的可用范围

自 **0.4.1** 起，`import` 进来的 `public` `struct`/`union`/`enum` 与 `public const` 在**主源与任何被导入模块里用法完全一致**——「自己是第几层模块」不影响可用性：

- **按值使用**：可声明本地变量、作函数形参/返回值、作结构体字段、取地址、访问成员、`sizeof`；
- **裸标签**：省略 `struct`/`union`/`enum` 关键字直接写 `Leaf l;`；
- **编译期常量**：作数组维度、`switch` 的 `case` 标签。

即：模块 `mid.co` 若 `import * from leaf;`，则 `mid.co` 内可像主源一样 `Leaf l;` 按值实例化并访问成员。**在 0.4.1 之前不成立**——被导入模块的标签/常量种子只注入主源，子模块里按值实例化会退化成不完整类型（`IR verification: Cannot allocate unsized type`，带成员访问时进程直接崩溃），只能用在指针与文件作用域字段位置；该限制在 0.4.1 已移除（加载每个子模块时按**其直接依赖**注入种子，与主源同一条管线）。

```co
// leaf.co
public struct Leaf { int a; int b; };

// mid.co —— 子模块，按值使用导入类型
import * from leaf;

public int leaf_sum(void)
{
    Leaf l;            // 直接实例化（0.4.1 之前会失败）
    l.a = 1;
    l.b = 2;
    return l.a + l.b;
}
```

> **同名标签不再静默劫持（0.4.1 起）**：同一文件 `import` 了两个都 `public` 了**同名标签**的模块时，编译期发**警告**；宿主再用裸名引用该标签则**报错**并要求改用限定名（`a.Leaf` / `b.Leaf`）。详见下节「同名标签：警告 + 限定类型名」。

## 同名标签：警告 + 限定类型名

类型名与函数/全局同属「跨模块同名」问题，但类型还牵扯**布局**——取错一份就是静默错编译。自 **0.4.1** 起按「定义侧报警 + 使用侧拒绝 + 限定名兜底」三段处理：

**① 定义侧（无条件警告）**：只要**两个以上**被导入模块导出了同名 `public` 标签（`struct` / `union` / `enum` / `class` 一律适用），宿主编译即得一条警告，列出全部候选：

```text
leaf_a.co:1:22: warning: tag 'Leaf' is exported by multiple imported modules (leaf_a.Leaf, leaf_b.Leaf);
              unqualified references are an error, use a qualified name
```

该警告**不看两份布局是否相同、也不看宿主是否真的用了裸名**——同名即提，属「须知情」性质，不影响编译与链接。候选按**你实际能写出来的名字**列出：有 `as ModAlias` 就报别名，没写别名才报文件名（故第①例的导入无别名→报 `leaf_a.Leaf`；第②例起了 `a`/`b`→报 `a.Leaf`）。

**② 使用侧（裸名报错）**：宿主（主源）随后以**裸名**引用该标签即报错，并给出候选限定名：

```text
main.co:3:23: error: reference to 'Leaf' is ambiguous (defined in multiple imported modules:
               a.Leaf, b.Leaf); use a qualified name
```

覆盖所有类型引用点：局部/全局声明、函数签名、成员类型、转换 `(Leaf)x`、`sizeof(Leaf)` / `_Alignof(Leaf)`、`typeof(Leaf)`、`new Leaf()`、`Leaf::static`。以下两种情况**不报错**：

- **宿主自己定义了同名类型** → 宿主胜出（与函数/全局的「本地遮蔽」同口径）；
- **只有一个模块导出该标签** → 裸名照旧可用，无需限定名。

**③ 限定类型名 `mod.Tag`**：`mod` 取 `as ModAlias`，无别名时取文件名去扩展名。类型名位置一律接受：

```co
import * from leaf_a as a;      // public struct Leaf { int a; };
import * from leaf_b as b;      // public struct Leaf { char *s; int y; };

b.Leaf l;                       // 声明：取 leaf_b 的 16 字节布局
l.s = nullptr;
l.y = 7;
if (sizeof(a.Leaf) != 4) return 1;   // sizeof：各取自身布局
if (sizeof(b.Leaf) != 16) return 2;
a.Leaf* p = (a.Leaf*)0;              // 指针 / 转换
```

同时支持 `struct a.Leaf l;`（关键字形式）、`union` / `enum` / `class` 同构（`a.Slot` / `a.Color` / `new a.Box()`）。**模块内部**（含被导入模块自身）的裸名引用不受任何影响——各模块内部的裸名恒绑自身定义。

**④ 限定枚举常量 `mod.RED`**：`mod.X` 在函数/全局之后再看被导入模块的 `public enum` 常量表，命中即折成同值整型常量（与裸名 `RED` 同语义、零运行期开销）：

```co
// palette.co
public enum Color { RED, GREEN = 5, BLUE };

// main.co
import * from palette as p;

if (p.RED != 0) return 1;        // 0
if (p.BLUE != 6) return 2;       // GREEN = 5 → BLUE = 6
```

> 自 **0.4.1.47** 起，**编译期常量位置**同样识别限定写法：`case p.GREEN:`、`int arr[p.BLUE];`、`enum { E = p.GREEN };`、`int g = p.GREEN;`（含文件作用域与 `static` 初值）均按同值整型常量折叠。
>
> 仍不开放：**裸名**枚举常量不跨模块（`import` 后进 `main.co` 写 `GREEN` 依旧报 `use of undeclared identifier`）——需跨模块使用的整型常量请用 `public const`（见「跨模块编译期常量」）。

**④-补 文件作用域初值必须是常量表达式（0.4.1.48 起拒收）**：把**非 `public const`、非枚举常量**的全局用作文件作用域初值——无论是同文件的普通 `int`，还是跨模块的 `public int`——不再「编得过但值是 0」，而是编译期硬错，文案与 clang `-std=c23` 逐字一致、位置指向被引用的那个变量：

```co
// main.co
import * from cfg as p;      // public int M = 5;  public const int K = 7;  public enum Size { DIM, WIDE };

int g = p.M;                 // 0.4.1.48 起：error: initializer element is not a compile-time constant
int h = M;                   // 裸名（由 `import *` 带入）同拒；同文件自写的普通 `int M` 也一样
int ok = p.K;                // const 全局：折真值（0.4.1.46 起）
int e = p.DIM;               // 限定枚举常量：折真值（0.4.1.47 起）
int *q = &p.M;               // 取址：地址常量，不受影响
```

复合形态同样拒收（`M + 1`、`-M`、`(int)M`、`double d = M`），限定写法的复合形态一并拒收（`-p.M`、`(int)p.M`、`double d = p.M`、`p.M + 1` 实测各一条诊断）。四条口径须知情：

- **纯 C 档（`.c`）同样适用**——C §6.7.9p4 本就要求静态存储期初值为常量表达式，本轮只是把「静默折 0」改成拒收；
- **有运行期初始化载体时不报错**：`.co` 里写了顶层裸句（或产 `*.dll` / `WinMain` 载体）时，折不动的初值改由合成载体在运行期 store，因此同一行代码在两种形态下结论不同；
- **地址常量族不受影响**：`&p.M` 取址、数组名/函数名退化（`int *gp = arr;`、`int (*fp)(int) = add6;`）、`sizeof` 均不构成「加载」，照旧接受；**地址常量算术**自 **0.4.1.49** 起折成**真实地址**——`arr + 1`、`1 + arr`、`arr - 1`、`&x + 1`、`&arr[0] + 2`、`&arr + 1`、链式 `arr + 1 + 1`、字符串字面量基址 `"abc" + 1` 均按**元素步进**计算（`arr + 1` 前进 `sizeof(元素)`；`&arr + 1` 前进整个数组宽度）——此前该族能编过却折成 `null`，运行期解引用即崩溃；
- **指针目标同样进拒收面（0.4.1.49 起）**：`int *o = &M; int *p = o;` 中的 `o` 是被**加载**的非 const 全局，clang `-std=c23` 报错，colang 从「静默折 null 指针」改为拒收（基址为加载的复合形 `o + 1` 一并拒收）；**0.4.1.50 起拒收面扩到三族**——聚合成员位置（`int a[2] = {M, 1};`、`{[0]=M}`、`{{M}, 1}`、`{.in.x = M}`）、解引用与下标（`*(&M)`、`*o`、`*arr`、`*(arr + 1)`、`arr[0]`、`*(&s.a)`）、成员访问（`oo.y`、`oo.in.x`、`op->in.x`）均在同一行报错、文案与 clang 逐字一致；**0.4.1.52 起再收「指针→整型显式转换」一族**——`(long)(arr + 1)`、`(long)arr`、`(long)&M`、`(int)&M`、`(long)(&arr[1])`、`(int)(long)arr`、`(long)add9`、`(long)&K` 以及聚合成员位置里的 `{(long)arr, 1}` 均改为同一行拒收（C 里指针转整型本身就不构成常量表达式，clang 一律报错）；**无转换的同对象指针差** `arr - arr` 与 `(long)(arr - arr)`（被转侧已是整型）照旧接受。仍不覆盖的形态知情：`extern` 纯声明被加载作初值、块作用域 `static int s = M;`。**0.4.1.53 起「成员数组退化」折成真实地址**：`oo.buf`、`oo.buf + 2`、`oa[1].buf`、union 成员 `uu.c`、聚合成员位置里的 `{oo.buf, oa[1].buf}` 不再折 `null`；同批治愈三形**误拒**（此前比 clang 更严，clang 零诊断）——`int *q = oo.m[1];`、`int *q = oo.m[0] + 2;`、`int *q = g2[1] + 1;`（成员数组的下标形与二维数组的行退化形），现与 clang 同样接受且折出真地址；步进按退化后的元素宽计（`oo.buf + 2` 步 1、`oo.m[0] + 2` 步 4）。链中只要含一层加载（箭头 `op->buf`、解引用 `(*op).buf`、指针下标）仍**不是**常量地址——自 **0.4.1.57** 起这类取址形（`&op->buf[1]`）与退化形（`(*op).buf`）与 clang 同行同文拒收，此前它们被误收并折成错误地址（把指针变量自身的存储当对象基址）；而对指针变量**自身**取址（`&op`）仍是合法地址常量、照旧接受。**0.4.1.55 起「指针差」与「转换后步进宽」两处折叠缺口已收口**：同对象指针差按「相对同一根全局的字节差 ÷ 有效 pointee 宽」折出元素数，`long d = &arr[1] - &arr[0];` 得 1、`long d = &arr[0] - &arr[2];` 得 -2、`&s.b - &s.a` 得成员间距，且 `int d8 = g2[1] - g2[0];` 一类此前被**误拒**的形一并治愈；步进宽一律取**转换后的 pointee**而非基数组元素宽——`(char *)arr + 1` 步 1 字节、`(short *)arr + 3` 步 2、`(void *)arr + 1` 步 1（GNU 语义，与 clang 一致）。**0.4.1.58 起「跨对象指针差」的漏报已收口**：`other - arr`、`&oo.in.x - &arr[0]`、`&other[1] - &arr[1]`、`&M - &N`、`ex - arr`、`arr + 1 - other` 以及聚合成员位 `{ other - arr, 0 }` 一类两侧取址落到**不同根全局**的差，此前不报、静默得 0，现与 clang 同行同文拒收（判据与折叠器同源：两侧都解析得出根全局、根不同、且步进宽相等并为正）；只有**同一对象**内的差才是合法形，照旧折成真值。**0.4.1.62 起「异 pointee 类型的指针差」的漏报已收口**：`sa - arr`、`(short *)arr - arr`、`(long *)arr - (int *)arr`、`(long *)&s.a - (int *)&s.a`、`int (*)[3] - int (*)[4]`、`enum E1 * - enum E2 *` 一类两侧指向**不相容类型**的差，此前不报、静默得 0，现由 sema 与 clang **同行同列同文**拒收（文案 `'short *' and 'int *' are not pointers to compatible types`，C §6.5.9p4；诊断定在 `-` 运算符那一列，文件作用域初值与函数体内的形同样覆盖）。相容的形照旧接受、**不误拒**：`int *` ↔ `const int *`（限定符差异）、`enum *` ↔ `int *`、同型异对象、数组名退化后同型。同批两处：两侧都是 `void *` 的差此前使 colang **编译期崩溃**（`0xC0000005`），现已修好并按 GNU 语义以**字节**步长得值（`vp - vp2` 得 -12；注意 `void *` ↔ 对象指针的差仍是不相容形、照 clang 拒收）；诊断里的指针类型拼写与 clang 对齐（`int (*)[4]` 不再印成 `int[4] *`、`int **` 不再印成 `int * *`、`unsigned char *` 不再印成 `char *`）。**0.4.1.63 起「`&函数名` 的类型被回退成 `void *`」的缺口已收口**：`&函数名` 现按**真实函数原型**解析（`&f` 得 `int (*)(void)`），诊断随之与 clang 对齐——差面 `long d = &f - arr;` 由「同行同列却印 `'void *'`」转为**同行同列同文**；异原型函数名之差（`&f - &g`，即 `int (*)(void)` ↔ `int (*)(int)`）此前两侧同退 `void *` 被判相容而**整条漏报**，现已能报；赋值面（`int (*p)(void) = &g;`）此前因任一侧是 `void *` 而被静默放行，现同样报错且文案与 clang 逐字一致（clang 定在声明符名、colang 定在初值，列位差异属既有口径）。同批治愈一处**编译期崩溃**：合法 C `(*&f)()`（函数指针解引用后再调用）此前使 colang 崩溃于编译期，现与 clang 同样编译、同样运行。自 **0.4.1.67** 起「裸函数名作右值」的 decay 形已收口：`VarRef` 解析落空后改按**真实函数原型**取型（未 decay 的函数型），衰减统一交给既有左值转换——赋值面（`int (*q)(void) = f;`、`int *ip = f;`）与差面（`f - g`、`f - arr`）现与 clang **同行同文**（前者印未 decay 的函数型、后者印 decay 后的函数指针；列位差异属既有口径）；`sizeof(函数名)` 按 GNU 扩展与 clang 同值 **1**（文件域初值折叠的时序一并修好）；C23 `auto pf = f;`、`auto pa = arr;` 按左值转换推出函数指针／元素指针，同批治愈 **`auto p = &f;` 被误拒**的回归（合法 C 不再报错）。自 **0.4.1.73** 起**隐式「指针 ↔ 整型」转换**不再静默：`long z = arr;`、`long z = &sobj;`、`long z = arr + 1;`、`long z = str;`、`long z = 1 + arr;` 一类初始化，赋值形（`z = arr;`）、实参形与块作用域声明形一并覆盖，与 clang **同行同文**——初始化 `incompatible pointer to integer conversion initializing 'long' with an expression of type 'int[4]'`（右侧印未 decay 的数组型、定在声明符名）、赋值 `assigning to 'long' from 'int[4]'`（定在 `=`）、传参 `passing 'int[4]' to parameter of type 'long'`（源在前）；反向另有 `incompatible integer to pointer conversion …`（`int *q = 1;` 一类）。仍**照旧接受、不报**：**布尔目标**（`bool b = arr;`，clang 只发 `-Wpointer-bool-conversion` 警告）、**空指针常量**（`int *p = 0;`、`int *p = 1 - 1;`、`(void *)0` 等值 0 的整型常量表达式）、以及算不出值的整型变量源（`int *p = x;` 仍漏报）。仍余**四处已知缺口**知情：`char *` 与 `signed char *` 的差不报（类型表征里 `char` 与 `signed char` 不可区分）；**指针→浮点**是另一句文案（`float f1 = arr;` 报 `initializing 'float' with an expression of incompatible type 'int[4]'`，colang 仍接受）；聚合初始化器的**元素位**（`int (*tbl[2])(void) = { f, g };`）与 `?:` 作数组维度（`int a[1 == 1 ? 3 : 5];`）两族仍不报；另知情 `bool b = 指针;` 的**值面**按截断实现（非零判断不可靠）。自 **0.4.1.75** 起**下标基型**只认「数组 / 指向对象的指针」：`f[1]`（函数名）、`int (*fp)(void); fp[1]`、`int (*tbl[2])(void); tbl[0][0]`、文件作用域初值 `int v = f[1];` 与聚合元素位 `{ f[1], 0 }` 一律与 clang **同行同文**——`subscript of pointer to function type 'int (void)'`（按原型印、定位在基址起点）；函数**指针**基此前**完全不报**、运行期取到函数体字节（实测崩溃 `0xC0000005`），现已一并消除。自 **0.4.1.76** 起 **`void *` 的算术与下标**按 GNU 语义以 **1 字节**步长收口：`void *vp; vp[1];` 语句形不再使 colang 编译期崩溃（此前 `0xC0000005`，clang 只发 `-Wunused-value`），`vp + 1`／`1 + vp`／`vp - 1`／`vp++`／`vp += 1` 不再被 `GEP into unsized type` 误拒（步长 1 字节，与 `(void *)arr + 1`、`void * - void *` 同口径），`sizeof(void)` 与 `sizeof(vp[1])` 亦为 1；`void` 元素被取出时**不读内存**（零占位），故 `vp` 为 null 也安全。自 **0.4.1.77** 起**标量基下标**的文案与定位亦与 clang 同行同文：`int x; x[1]`、`double d; d[1]`、`struct S s; s[1]`、`union U u; u[0]`、`enum E e; e[0]`、`1[0]`、`a[0][0]`（定第二个 `[`）、`x[1][2]`（定第一个 `[`）、`s.a[0]`、`(s.a)[0]`、`g()[0]`、`vf()[0]` 一律报 `subscripted value is not an array, pointer, or vector`，定位定在**出错那一层的 `[` 列**（此前是自研句、且定在整条基址起点，列差 1~4）；自 **0.4.1.79** 起指向**不完整记录**的指针下标（`struct I *ip; ip[1];`）亦进 sema 拒收面、与 clang 同行同文（`subscript of pointer to incomplete type 'struct I'`，定在该层 `[`；前置声明的 `enum` 照旧接受，初始化/实参/取址/算术/成员五类外壳一律只报这一条——此前它落不到 sema，由 codegen 的 `GEP into unsized type` 兜底且外壳抢先报出异族一句）；自 **0.4.1.84** 起，`void` 值用于**二元运算、普通/复合赋值、初始化、实参与 `++/--`**（如 `vp[1] != 0`、`vp[1] = vp`、`int x = vp[1];`、`int y = f();`，其中 f 返回 void）已进 sema 拒收面，本批主错误与 clang **同行同文同列**，不再静默接受、编译期崩溃或由 verifier 兜底；二元/赋值/自增减定运算符列，void 源初始化定**声明符标识符**（含函数指针、多个声明符与跨行初值），其他相容族定位不变。显式丢弃、void 调用语句和逗号左操作数仍接受；void 函数中的 `return vp[1];` / `return (void)0;` / `return f();` 按 **clang 默认扩展口径**接受，返回表达式的副作用保留（不宣称严格 ISO C 合法）。void 函数错误返回非 void 值仍报错并带函数名。剩余 void 值的**一元运算（如 `-`/`!`）、条件位置与返回到非 void 结果**属于另一批已知缺口。自 **0.4.1.80** 起 `sizeof` / `_Alignof` 施于**不完整记录**（`struct I *ip; sizeof(*ip)`、`sizeof *ip`、`sizeof(struct I)`、`_Alignof(struct I)`、typedef 形与 union 形）亦进 sema 拒收面、与 clang **同行同文同列**——类型形定在 `sizeof` 关键字列、表达式形定在**关键字之后的第一个 token**（括号形即 `(`、无括号形即操作数首 token），文案 `invalid application of 'sizeof' to an incomplete type 'struct I'`（`_Alignof` 即使源码写 `_Alignof` 也印 `alignof`；typedef 形带 `(aka 'struct I')` 双轨）；前置声明的 `enum`（`sizeof(*ep)`）照旧接受，`ip->a` / `ip[0]` 一类已被更早诊断命中的操作数不叠此句，此前该族**静默折 0**（误收 + 静默错值）；仍余两处知情漏报：记录定义在 `sizeof` **之后**（按「全 TU 有定义即完整」判）、以及 `int arr[sizeof(*ip)];` 的维度位置。自 **0.4.1.82** 起**指向不完整记录的指针算术**（`ip + 1`、`1 + ip`、`ip - 1`、`ip++`、`--ip`、`ip += 1`、`ip -= 1`，含文件作用域初值 `struct I *gq = gp + 1;`）亦进 sema 拒收面、与 clang **同行同文同列**——文案 `arithmetic on a pointer to an incomplete type 'struct I'`，二元形定在**运算符列**、后缀/前缀 `++/--` 与 `+=/-=` 定在**运算符起点**，union 印 `'union U'`、typedef 带 `(aka 'struct I')` 双轨；算术错后结果型按「不可知」截断，故 `int x = ip + 1;` / `char *cp = ip + 1;` 只报这一条（此前先报 int-conversion / 指针不相容），两侧都是指针且不相容时仍只报 `'X *' and 'Y *' are not pointers to compatible types`；仍余两处知情缺口：`1 - ip` 属 `invalid operands to binary expression` 另族（仍由 codegen 的 `GEP into unsized type` 兜底），以及**记录定义在算术之后**的文件域全局形（同上按全 TU 口径判）。

要引用常量请用 `public const` / `public enum`；要取运行期的值请在函数体内赋值。

**⑤ 限定类静态成员 `mod.Counter::total`**：`mod.Counter` 作为 `::` 的左值先合并成限定类名，再按类作用域解析静态数据成员/静态方法；读、写、调用与裸类名同权：

```co
import * from counter as c;      // public class Counter { public static int total; public static int twice(int x); };

if (c.Counter::twice(21) != 42) return 1;
c.Counter::total = 10;                // 写：静态变量跨模块同一符号（linkInModule 归并）
if (c.Counter::total != 10) return 2;
```

名字校核：`c.Counter::nope` → `no member named 'nope' in 'c.Counter'`；`c.Counter::n`（非静态成员）→ `invalid use of non-static data member 'n'`；拼错模块名/非 public 类 → `no public type 'X' in module 'c'`。

**⑥ 限定 `typedef`：`public typedef` 才算导出**（0.4.1.40 起）：模块里标了 `public` 的**文件作用域** `typedef` 进入导出面，宿主可写 `a.MyInt`；裸名同样可用（与标签同权），同名多来源时按上面的三段口径裁定——定义侧警告 + 裸名报错 + 限定名可用：

```co
// types.co
public typedef int MyInt;
public struct Point { int x; int y; };
public typedef struct Point MyPoint;   // 结构体别名：宿主侧取 types 的布局
typedef int Hidden;                    // 未标 public → 不导出

// main.co
import * from types as t;

t.MyInt a = 3;      // 限定名
MyInt b = 4;        // 裸名（单来源）同样可用
t.MyPoint p;        // 结构体别名 → t 的 2 成员布局（即使宿主另有同名 struct Point）
p.x = 1;
p.y = 2;
```

- **只有 `public typedef` 导出**：非 `public` 的在宿主里写 `t.Hidden` 报 `no public type 'Hidden' in module 't'`。
- **与限定类型名完全同权**：声明、形参、返回类型、指针/数组、`sizeof`、转换 `(t.MyInt)x`、初始化表达式等一律接受。
- **底层不得是导入来的类型**：`public typedef a.Point Bad;`（底层引用别的模块导出的类型）报 `public typedef 'Bad' cannot alias imported type 'a.Point'`——首版刻意拒收，避免一个模块的依赖被半带进另一个模块的导出面。
- **宿主自己定义了同名 typedef** → 宿主胜出（裸名归宿主），`t.X` 仍取模块那份，与标签同口径。

> **破坏性断代（附迁移例）**：0.4.1 之前上述场景是**静默错编译**（后导入者赢，另一份布局的字段被折到 0 号槽、读写互串）。依赖该「碰巧能编过」的旧代码在 0.4.1 会得到**报错**，须改用限定名（`a.Leaf` / `b.Leaf`）或改用不同标签名。

同名两份的定义（布局不同）：

```co
// leaf_a.co
public struct Leaf { int a; };              // 4 字节

// leaf_b.co
public struct Leaf { char *s; int y; };     // 16 字节
```

**迁移前（0.4.0 及更早：能编过，但静默错编译）**

```co
import * from "leaf_a.co" as a;
import * from "leaf_b.co" as b;

Leaf g;                            // cc=0、零诊断：裸名只绑到其中一份（后导入者胜出）
if (sizeof(Leaf) != 4) return 1;   // 这里得 16 —— 编译期不说，运行期才暴露
```

**迁移后（0.4.1：编译期就拦下）**

```text
leaf_a.co:1:22: warning: tag 'Leaf' is exported by multiple imported modules (a.Leaf, b.Leaf);
              unqualified references are an error, use a qualified name
main.co:4:1: error: reference to 'Leaf' is ambiguous (defined in multiple imported modules:
              a.Leaf, b.Leaf); use a qualified name
```

**两种改法**

```co
// 改法①（推荐）：按诊断列出的候选补限定名，各取自身布局
a.Leaf g;          // 4 字节：语义回到原本意图
b.Leaf h;          // 16 字节
if (sizeof(a.Leaf) != 4) return 1;
if (sizeof(b.Leaf) != 16) return 2;
```

改法②：若本就不需要两份同名类型，把标签改成不同名字（如 `LeafA` / `LeafB`），裸名继续可用、无需限定。

## 模块别名限定访问 `m.f()` / `m.x`

导入模块后可用 `m.名称` 形式**直接访问该模块的 `public` 成员**，无需把名称逐一具名导入：

- **默认限定名** = 模块文件名去掉扩展名（如导入 `counter.co` → `counter.bump(3)` / `counter.total`）。
  - 若文件名不是合法标识符（含 `-`、`.` 等），**必须**用 `as ModAlias` 显式起别名。
- **别名覆盖**：`import * from "counter.co" as CNT;` → `CNT.bump(3)` / `CNT.total`。
- **可访问内容**：`m.f(...)`（函数调用）、`m.f`（函数引用）、`m.x`（全局变量读）、`m.x = v`（全局变量写）。
- **写与本地同权**：`m.x = v` 视同对本地变量的赋值；唯一例外是 `m.const全局 = v` 被拒（`read-only variable is not assignable`）。
- 限定访问仅对 `public` 成员有效，非 `public` 一律 `no public symbol` 报错。

```co
import * from "counter.co" as CNT;   // counter.co: public int total; public int bump(int n){...}
CNT.total = 5;                        // 限定写全局
int a = CNT.bump(3);                  // 限定调用 → a == 8
return CNT.total == 8 && a == 8 ? 0 : 1;
```

## 撞名分流

多个模块向主源导出**同名**符号时，按下列口径裁定（`m` 指限定名/别名，`裸名` 指不带限定符的直接引用）：

1. **显式具名导入 与 本地同名** 冲突 → 报错（同一裸名两个显式来源）。
2. **`import *` 通配 与 本地同名** 冲突 → **裸名归本地**、通配项被遮蔽（发遮蔽警告）；通配只携带 `public` 面，非 `public` 静默不入。
3. **两个通配模块同名** → 该**裸名歧义、禁用**（引用报 `identifier not imported` 类），但 `m1.name` / `m2.name` **限定名各可访问、各绑其主**。
4. **默认限定名 = 文件名 stem**，`as ModAlias` 覆盖；stem 非合法标识符时须显式别名。
5. **两个显式具名导入跨模块同名** → 报错。
6. **导出面最小化**：模块侧 `public` 仅在合并进宿主时充当可见性闸门，产物为 exe 时不逐个进 DLL 导出表。
7. **函数与全局变量两张表同规则**：`m.f` 与 `m.x` 的解析、遮蔽、歧义三件套一致。
8. **限定访问可写性同权**：不设「跨模块只读」门，`m.x = v` 与本地同权（仅 `const` 全局写被拦，见上）。
9. **类型标签（`struct`/`union`/`enum`/`class`）同属一套裁定**：两个以上模块导出同名 public 标签 → 定义侧发**无条件警告**；宿主裸名引用 → 报 `reference to 'X' is ambiguous ...; use a qualified name`；改用限定类型名 `m.Tag` 即可各取自身布局（详上节「同名标签：警告 + 限定类型名」）。宿主自有同名类型时裸名归宿主（同分流②）。

> **跨模块同名共存**：分流③允许两个模块各自定义同名 `get` 而互不覆盖——链接期以合成符号名（`__<模块短名>__<名称>`）并存，各自的 `m1.get()` / `m2.get()` 与模块内部调用都绑回自身定义。

## 顶层代码的执行模型

被 `import` 的模块若含**顶层裸指令**（不属于任何函数/类型定义的顶层语句），其执行遵循：

- **首次加载时执行一次**：同一模块无论被引用多少次，其顶层裸代码只跑一次。
- **集中拓序**：所有被导入模块的顶层代码在**主源入口之前**统一执行；顺序 = 依赖闭包的 **DFS 后序**（被依赖方先跑、依赖方后跑），与 `import` 行在主源里的书写位置无关。
- **非主模块的 `main` 不作为加载钩子**：若模块内定义了 `main`，它只是普通函数符号，不会自动执行。

## 去重与环检测

- **去重**：以规范化绝对路径为唯一 key（Windows 下大小写不敏感），同一模块被多处 `import` 只加载一次、拓入一次。
- **环检测**：加载阶段以 DFS 遍历依赖图，检测到环时报告完整路径，例如 `cycle detected: a.co -> b.co -> a.co`，随后终止编译。

## 示例

**主源 `main.co`**：

```
import add, wrapper from "mod.co";
if (add(3, 4) != 7) return 1;
if (wrapper(2) != 7) return 2;
return 0;
```

**被导入 `mod.co`**：

```
static int helper(int x) { return x * 3; }
public int add(int a, int b) { return a + b; }
public int wrapper(int x) { return helper(x) + 1; }
printf("mod initialized\n");
```

编译运行：先打印 `mod initialized`（顶层裸指令集中拓序 emit），然后按 `main.co` 的顶层顺序执行、退出码由顶层 `return` 决定。

## 当前实装边界（0.4.1）

已实装：
- `.co` 源文件的完整消费链路（词法/语法/加载/逐模块闭包分析/合并/可见性/strip/入口集成）；
- 具名与 `*` 两种形态；`import … as ModAlias` 模块别名**已启用限定访问**；
- **模块别名限定访问** `m.f()` / `m.f` / `m.x` / `m.x = v`（含写与本地同权、`const` 全局写拦截）；
- **独立模块闭包**：逐模块类型/符号隔离，同名异布局 `struct` 互不顶替；
- **ambient 切断**：模块内 `#include` 带入的环境符号不入导出面；
- **撞名分流①~⑧**：遮蔽/歧义/跨模块同名共存（链接期以 `__<短名>__<名>` 合成符号并存）；
- 集中拓序 + 首次加载执行一次；环检测与去重；
- imported 非 `public` 符号在 codegen 强制 `InternalLinkage`；
- 跨模块 `public struct` / `union` / `enum` 类型引用（可作值类型变量、成员访问、取地址传参）；**主源与子模块同口径**（0.4.1 起，子模块内按值实例化不再退化为不完整类型）；
- 跨模块**裸标签**：被导入模块里标 `public` 的 `struct`/`union`/`enum`，消费方引用时可省略关键字（`Point p;`）；
- **限定类型名 `mod.Tag`**（0.4.1）：类型名位置一律接受（声明/`struct` 关键字形式/转换/`sizeof`/`_Alignof`/`typeof`/`new`），`struct`/`union`/`enum`/`class` 同构；同名标签按「定义侧警告 + 裸名报错 + 限定名可用」三段裁定；限定**枚举常量**（`p.RED`）、限定**类静态成员**（`c.Counter::total`）与限定 **`typedef`**（`a.MyInt`，仅 `public typedef` 导出）同批开放；限定枚举常量自 **0.4.1.47** 起亦可用于**编译期常量位置**（`case` 标签/数组长度/`enum` 初值/全局初值），**裸名**枚举常量仍不跨模块。
- 跨模块**编译期常量**：被导入模块的 `public const int`（如 `winapi.co` 的 `WM_*` 系列）在消费方可作普通只读值、数组维度、`switch` 的 `case` 标签使用；**非 `public const`/非枚举常量**的全局（含跨模块 `public int`）作文件作用域初值，自 **0.4.1.48** 起拒收（`initializer element is not a compile-time constant`），不再静默得 0；取址等地址常量形态不受影响；自 **0.4.1.50** 起拒收面扩至**聚合成员位置**（`{M, 1}`/`{[0]=M}`/`{{M}, 1}`/`{.in.x = M}`）、**解引用与下标**（`*(&M)`/`*o`/`*arr`/`arr[0]`/`*(&s.a)`）与**成员访问**（`oo.y`/`oo.in.x`/`op->in.x`）三族，与 clang 同行同文；自 **0.4.1.52** 起再收**指针→整型显式转换**一族（`(long)arr`/`(long)&M`/`(long)add9`；无转换的同对象指针差 `arr - arr` 不受影响）；`extern` 纯声明与块作用域 `static` 初始化器仍为知情缺口；**0.4.1.53 起成员数组退化（`char *pc = oo.buf;`、`oo.m[1]`、`g2[1] + 1`）折成真实地址**，其中下标形与行退化形此前被误拒、现与 clang 同收；**0.4.1.55 起同对象指针差（`&arr[1] - &arr[0]` 折 1）与转换后步进宽（`(char *)arr + 1` 步 1、`(void *)arr + 1` 步 1）折成真值**；**0.4.1.57 起经指针的取址链/退化链（`&op->buf[1]`、`&(*op).buf[1]`、`(*op).buf`）与 clang 同行同文拒收、不再折错地址**（对指针变量自身取址 `&op` 仍照旧接受）；**0.4.1.58 起跨对象指针差（`other - arr`、`&M - &N`、`ex - arr`、`arr + 1 - other`、聚合成员位 `{ other - arr, 0 }`——两侧取址落到不同对象）作文件作用域初值与 clang 同行同文拒收**（判据与折叠器同源，同对象差照旧折真值）；**0.4.1.62 起异 pointee 类型的指针差（`sa - arr`、`(long *)arr - (int *)arr`、`int (*)[3] - int (*)[4]`、`enum E1 * - enum E2 *`）由 sema 与 clang 同行同列同文拒收**（C §6.5.9p4，文案 `'X *' and 'Y *' are not pointers to compatible types`、定在 `-` 运算符列；限定符差异与 `enum *` ↔ `int *` 等相容形照旧接受），同批两侧 `void *` 的差不再编译期崩溃且按 GNU **字节**步长得值、诊断里的指针拼写与 clang 对齐（`int (*)[4]`/`int **`/`unsigned char *`）；**0.4.1.63 起 `&函数名` 按真实函数原型解析（`&f` 得 `int (*)(void)`），指针差的异 pointee 文案与赋值面随之与 clang 同行同文、异原型函数名之差由漏报转报，合法形 `(*&f)()` 不再编译期崩溃**；**0.4.1.67 起**裸函数名 decay 形已收口、**0.4.1.73 起**隐式「指针 ↔ 整型」转换（初始化/赋值/传参三句式）与 clang 同行同文（布尔目标与空指针常量族仍合法）；仍余 `char *` ↔ `signed char *` 与**指针→浮点**（`float f1 = arr;`，是另一句文案）两处知情缺口；**下标基型**族自 0.4.1.75 起函数名/函数指针基已同文，自 **0.4.1.76** 起 `void *` 的算术与下标按 1 字节步长、语句形 `vp[1];` 不再编译期崩溃（`sizeof(void)`/`sizeof(vp[1])` 亦为 1），标量基下标的文案与列位自 **0.4.1.77** 起亦与 clang 同行同文（定在出错那一层的 `[`），指向**不完整记录**的指针下标自 **0.4.1.79** 起亦与 clang 同行同文（`subscript of pointer to incomplete type 'struct I'`，定在该层 `[`，前置声明的 `enum` 照旧接受），`sizeof` / `_Alignof` 施于不完整记录自 **0.4.1.80** 起亦与 clang 同行同文同列（类型形定关键字列、表达式形定关键字后第一个 token），指向**不完整记录**的指针算术自 **0.4.1.82** 起亦同（二元形定运算符列、`++/--`/`+=/-=` 定运算符起点，`int x = ip + 1;` 一类只报算术句、不叠 int-conversion），**0.4.1.84 起 void 值用于二元运算、赋值、初始化、实参与 `++/--` 已与 clang 同行同文同列拒收**（运算符定其本列，void 源初始化定声明符名，含复杂声明符与跨行初值）；显式丢弃照旧接受，void 函数返回 void 表达式按 clang 默认扩展口径接受并保留副作用。仍余 void 值的一元/条件/返回到非 void 结果三族、`1 - ip` 的 invalid-operands 另族，以及记录定义在算术之后的文件域形等知情缺口；
- **DLL / WinMain 顶层初始化载体**：产 `-o *.dll` 时合成 `DllMain`、在 `PROCESS_ATTACH` 跑各模块顶层初始化（用户自写 `DllMain` 改名转发）；GUI exe 的 `WinMain` 入口下，顶层裸句与运行期初值全局由合成 WinMain 载体承载（见「模块」章节）。

保留名：`__init__*__` 为模块初始化调度协议保留前缀，**禁止用户代码自定义**（编译期硬错 `reserved protocol name '__init__*' cannot be defined by user code`）。

本批未覆盖、归后续里程碑：
- `.bc` / `.dll` / `.lib` / `.so` / `.obj` 作为可导入模块（均须携带编译期声明元数据）；当前对它们一律给**明确诊断**（见「目标文件后缀白名单」），不会退化成「找不到模块」；
- `__init__` 用户手写定制。
