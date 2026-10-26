# struct / union / enum

本章承载 Co 语言里三类**值类型**——`struct`（数据聚合）、`union`（共享存储）、`enum`（命名整型常量集）的定义与用法。基线与 ISO C 一致；在此之上 Co 加了两项扩展：**`public` 跨模块可见性**与**裸标签作类型**（仅 `.co`）。class 的引用类型见「class」章节。

## 语法

```
[ public ] ( struct | union ) TAG
{
	成员条目 …
};

[ public ] enum TAG
{
	常量名 [ = 常量表达式 ] , …
};

成员条目 ::=  类型  名  [ : 位宽 ]  ;
	         |  ( struct | union ) [ TAG ] { 成员条目 … } ;           // 匿名成员
	         |  类型  名  []  ;                                       // 柔性数组（仅末尾）
```

变量或形参声明：

```
( struct | union | enum ) TAG  名 [ = 初始化器 ] ;    // 关键字式（C 兼容、`.c` `.co` 通用）
TAG                         名 [ = 初始化器 ] ;       // 裸标签式（仅 `.co`）
```

初始化器：普通列表 `{ v0, v1, … }`；指定初始化器 `{ .成员 = 值 , … }`（可乱序、未列成员补零）；数组 `[ 下标 ] = 值`。设计符可**链式下钻**嵌套成员/元素：`.a[2].b`、`[1][2]`（C11 §6.7.9 设计符序列）。

| 成分 | 含义 |
| --- | --- |
| `TAG` | 标签名，在同 TU 内可作类型标识符使用（裸标签式，仅 `.co`） |
| `public` | 跨模块可见性闸门；不写则仅本模块内部可见、不能被 `import` |
| 成员条目 | 顺序即布局顺序（除位域内打包外，遵循 ISO C 对齐） |
| `: 位宽` | 位域；宽度为整数常量、不超过底层类型位宽 |
| 匿名成员 | 内联的 `struct` 或 `union`（无成员名），其成员被**提升**到宿主 |
| `名 []` | 柔性数组，作为最后一个成员；`sizeof` 不计其长度 |

## 说明

（本节只描述对外的语义与行为；内部实现机制不在本文范围。）

### 定位

`struct` / `union` / `enum` 都是**值类型**：变量代表一段确定的存储、赋值即逐字节拷贝、生命周期随作用域，不含构造函数/析构函数。与 `class`（引用类型、堆上、Co 自动管理内存）相对。

### 与 class 的分界

| | struct | class |
| --- | --- | --- |
| 类型 | 值类型 | 引用类型 |
| 内容 | 仅成员变量 | 成员变量 + 方法 + 构造/析构 + static 成员 |
| 存储位置 | 栈值 / 堆句柄 | 堆（class 只在堆上创建） |
| 内存回收 | 随作用域 | Co 自动管理 |

细节见「class」章节的「与 struct 的分界」小节。

### struct 定义与使用

支持命名 tag、**前向声明**（不完整类型）、**自引用**（成员为自身指针）、初始化列表与指定初始化器、`.`/`->` 访问。

```
struct Point
{
	int x;
	int y;
};

struct Node                                // 自引用：成员是指向自身的指针
{
	int        value;
	struct Node *next;
};

struct Point a = { 3, 4 };                 // 顺序初始化
struct Point b = { .y = 7, .x = 3 };       // 指定初始化器，可乱序，未列成员补零
struct Point c;                            // 未初始化，栈上取旧值
c.x = 1; c.y = 2;

struct Point *p = &a;
printf("%d %d\n", p->x, c.y);

struct Point d = c;                        // 赋值即逐字节拷贝
```

### union 定义与使用

`union` 的所有成员**共享同一存储**，大小等于最大成员；同一时刻只有一个成员被视为「有效」——写入哪个成员，哪个成员即为有效；由程序员负责维护这一约定。

```
union Value
{
	int  i;
	char b[4];
};

union Value u;
u.i = 256;
printf("%d\n", u.i);           // 256
u.b[0] = 1;                    // 之后 i 的值不再有意义
```

**匿名 union** 作为 struct 成员时，其成员被**提升**到宿主 struct——可直接以宿主点号访问、无需通过无名中间层。这是 tagged union 的常见写法：

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
t.i   = 42;                    // 匿名 union 成员提升到宿主
```

反过来，**匿名 `struct` 作为 `union` 成员**时，其成员同样被提升到宿主 `union`——可直接以 `union` 变量访问，而各成员仍落在该 `struct` 内部的各自偏移上。Win32 `LARGE_INTEGER`（`LowPart` / `HighPart` / `QuadPart`）正是这一形态：

```
union LongLong
{
	struct
	{
		int lo;
		int hi;                     // 相对匿名 struct 起点偏移 4
	};
	long long q;                    // 与上面的 lo / hi 重叠同一 8 字节
};

union LongLong v;
v.q = 0x0000000200000001LL;         // 按 64 位整体写入
printf("%d %d\n", v.lo, v.hi);      // 1 2（小端：低 32 位落 lo、高 32 位落 hi）
v.hi = 3;                           // 单独改高 32 位
printf("%lld\n", v.q);              // 0x300000001
```

`sizeof(union LongLong) == 8`；提升上来的 `lo` / `hi` 与 `q` 是同一段存储的不同视图。指定初始化器可直接落在被提升的成员或非首成员上：`union LongLong g = { .hi = 3 };`、`union LongLong h = { .q = 1 };`。

### enum 定义与使用

`enum` 定义一组命名整型常量，底层类型等价 `int`（`sizeof(Color) == sizeof(int)`）。可具 tag 也可匿名；常量可显式赋值，未赋值的按前一个 +1。

```
enum Color { RED, GREEN, BLUE };                       // 0 / 1 / 2
enum Kind  { A = 1, B = 2, C = 10, D };               // D 顺延为 11

Color c = GREEN;
if (c == 1) { }                                        // 与整数值等价、可比较
```

匿名 `enum { … };` 亦可，其常量名直接进入普通标识符命名空间。

### 位域（bit field）

成员后接 `: 整数常量` 即指定位宽；位宽不超过底层类型位宽。同一存储单元内的位域按声明序打包；跨单元时自动接续到新单元。允许**匿名位域**（不写成员名、仅 `: N`）作占位填充。

```
struct Basic
{
	unsigned a : 3;                        // bits 0-2
	unsigned b : 5;                        // bits 3-7
	unsigned c : 8;                        // bits 8-15
	unsigned d : 16;                       // bits 16-31
};                                         // sizeof == 4（单个 i32 单元）

struct Mixed
{
	int        header;                     // 普通成员，4 字节
	unsigned   f1 : 1;
	unsigned   f2 : 7;
	int        payload;                    // 4 字节
};

struct Basic x = {0, 0, 0, 0};
x.a = 7; x.b = 31; x.c = 200; x.d = 0xABCD;
printf("a=%d b=%d c=%d d=0x%x\n", x.a, x.b, x.c, x.d);
```

位域可读、可写、支持 `+=` `|=` 等复合赋值；可通过 `->` 访问（`p->f1 = 1;`）。初始化列表里的位域值按声明序逐个写入同一存储单元（掩码合并），不覆盖相邻位域。

### 柔性数组（flexible array member）

末尾成员可写作 `TYPE 名 [];`，`sizeof` 不计其长度。常见模式：前导长度字段 + 末尾柔性数组。

```
struct FlexStr
{
	int  len;
	char data[];                    // 柔性数组，只占头、实数据由外部缓冲提供
};

char buf[32];
struct FlexStr *fs = (struct FlexStr *)buf;
fs->len     = 2;
fs->data[0] = 'H';
fs->data[1] = 'i';
fs->data[2] = '\0';
printf("len=%d s=%s\n", fs->len, fs->data);
```

### 值语义与 sizeof

- **赋值 / 传参 / 返回**均为逐字节拷贝；修改副本不影响原对象。
- **可作函数形参与返回值**：`int sum(Point a, Point b)` 合法；调用点亦可用裸标签作类型（`.co`）。
- `sizeof` 返回**结构字节总长**（含内部对齐填充、含匿名成员的宿主布局）；柔性数组部分贡献 0。
- `sizeof` 可用于**文件作用域的常量初始化器**（如 `int sz = sizeof(Point);`）——编译期折出常量，与函数体内所得一致。
- `sizeof` 作用于**字符串字面量**取**含尾部 `\0` 的数组宽度**：`sizeof("abc") == 4`；`typeof("abc")` 同为数组类型。
- 数组元素、结构成员、指针成员各自的读写按 ISO C。

### Co 扩展 · `public` 与跨模块

`public` 是跨模块**类型可见性**的唯一闸门。被 `import` 的模块里，只有标了 `public` 的 `struct` / `union` / `enum` 才能被消费方拿到；未标 `public` 的仅在源模块内部可见、`import` 会拒收。

**模块 `geometry.co`**：

```
public struct Point { int x; int y; };
public union  Val   { int i; char b[4]; };
public enum   Color { RED, GREEN, BLUE };
struct Secret { int raw; };                   // 未标 public → 仅本模块内部可见
```

**消费方 `main.co`**：

```
import * from "geometry.co";                  // 或按名：import Point, Val from "geometry.co";

int main(void)
{
	Point        p = { .x = 1, .y = 2 };       // 裸标签式
	struct Point q = p;                         // 关键字式，与上等价
	return q.x + q.y;                           // 3
}
```

`import` 的具名/`*` 两种形态、路径解析与环检测详见「import」章节。

### Co 扩展 · 裸标签作类型（仅 `.co`）

在 `.co` 文件里，`struct` / `union` / `enum` 的 tag 可**省略前面的关键字**直接作类型使用——与 C++ 一致的风格。这是承 Co「简化 C++」定位（同 `bool` 定稿方向）的一项方言扩展。

```
struct Point { int x; int y; };
public enum  Color { RED, GREEN, BLUE };

int sz = sizeof(Point);                     // sizeof 亦可裸标签，文件作用域即折出常量

int addp(Point a, Point b) { return a.x + b.y; }         // 形参可用裸标签

int main(void)
{
	Point   p  = { 1, 2 };                    // 等价 struct Point p = { 1, 2 };
	Color   c  = GREEN;                        // 同 TU 内 enum 常量名可用
	Point  *pp = &p;
	return p.x + c + pp->y + addp(p, p) + sz;
}
```

- **同 TU**：本文件里已定义的 tag 直接裸用。
- **跨模块**：被 `import` 的模块里标了 `public` 的 `struct` / `union` / `enum` 标签，在消费方同样可裸用（见上例）。
- **与老式并存**：`struct Point a; Point b;` 可在同一源文件混用，两者等价。

> **兼容性告知**：`.c` 文件中**不启用**裸标签识别——`Point p;` 仍会报 `use of undeclared identifier 'Point'`，必须写 `struct Point p;`，与 ISO C 逐字一致。纯 C 档与 C 兼容基线不受本扩展影响。

## 当前边界（暂不支持 / 未定稿）

- **跨模块 enum 常量名**：`public enum` 的类型标签可跨模块使用（`Color c;`），但**枚举常量名**（如 `GREEN`）当前不跨模块；消费方仍以整数字面量代替。相关能力属另一里程碑。
- **子模块内部**：被 `import` 的模块**自身内部**裸用其**再上层** `import` 进来的标签暂不支持；跨模块裸标签只在**主源**这一层生效。
- **不完整类型前置诊断**：只有前向声明（未定义标签体）的 `struct`/`union` 被**按值分配存储**时——全局/局部变量定义、`struct` 字段、数组的不完整元素——编译器直接报错误诊断（自 0.3.1，措辞与 clang 同形），不再拖到后端崩溃；指针用法（声明、传参、成员访问 `p->x`）与 `extern` 声明在前向声明阶段仍合法。
- **`Foo(3);` 声明/表达式歧义**：某标识符既是类型标签又被当作函数调用写在语句首位时，`.co` 下会重解析为声明——与既有 `typedef` 名的歧义同性质，彻底解化归后续。
- **块作用域遮蔽**：与同名变量遮蔽 tag 的处理粒度沿用现有实现（对 ISO C 的 tag 命名空间做了合并简化）。

## 保留名

本语言层无 `struct` / `union` / `enum` 相关保留名。`class` 的四个生命周期协议保留名（`__new__` / `__retain__` / `__release__` / `__delete__`）见「class」章节。
