# class 类定义

## 语法

```
[ public ] class CLASS_NAME
{
	//构造函数
	CLASS_NAME()
	{
	}

	//析构函数
	~CLASS_NAME()
	{
	}

	//类变量定义
	[ public ] [ static ] TYPE VAR_NAME;

	//类方法定义
	[ public ] [ static ] TYPE FUNCTION_NAME( [ TYPE VAR_NAME , ... ] )
	{
	}

	//类属性定义（一对 get/set 访问器；访问器可各带 public/private，缺省 public）
	[ public ] TYPE PROPERTY_NAME
	{
		[ public | private ] get ;   // 或 get { ... }   体内可用 field
		[ public | private ] set ;   // 或 set { ... }   体内 value＝写入值、可用 field
	}
}
```

## 说明

### 只能写在 `.co` 源文件

class 是 Co 新增的类型形态——C 语言既没有这个关键字，也没有对应语义——因此 `class` 只能定义在 `.co`/`.CO` 源文件中。写在 `.c` 或 `.h` 里会被拒绝：

```
$ colang app.c
app.c:1:1: error: 'class' is only allowed in .co source files
```

判据是 `class` 关键字**实际所在的那个文件**：由 `.co` 源文件 `#include` 进来的 `.h` 并不放宽——头文件里的 class 定义照样报错，诊断位置给出该头文件自己的文件名与行号。

（不会连带刷屏：整块 `class … { … }` 一次性跳过，一处定义只报一条。）

### 与 struct 的分界

class 是 Co 的**引用类型**（数据 + 行为），与 struct（值类型、纯数据、兼容 C）相对：

| | struct | class |
| --- | --- | --- |
| 类型 | 值类型 | 引用类型 |
| 内容 | 仅成员变量 | 成员变量 + 方法 + 构造/析构 + static 成员 |
| 存储位置 | 栈值 / 堆句柄 | 堆（class 只在堆上创建） |
| 内存回收 | 随作用域 | Co 自动管理，通常无需手动释放 |
| 定位 | C 式数据聚合 | 面向对象的封装单元 |

Co 的面向对象取向是“简化”：class 提供封装与方法，但不追求完整 C++ 特性（详见“当前边界”）。

### 创建与持有

class 的实例只能在堆上创建。用 `new` 得到一个对象后，必须交给某个变量持有，或作为返回值交出；若创建后无人持有，该对象将无法访问并造成泄漏。

Co 里 class 变量持有的是对象的**引用（句柄）**：类名本身就代表这个引用，声明写 `Counter c`，**不写 `Counter*`**（后者会成指针对指针的二级引用，Co 不允许）。因此 `Counter c = new Counter();` 即用 `c` 持有新建的堆对象。

### 创建与使用

下面是一个端到端最小示例（构造/析构、方法、`this`、`static` 与 `::`、未标 `public` 的成员）：

```
class Counter
{
    Counter()                { count = 0; }              // 构造函数
    ~Counter()               { }                         // 析构函数
    public void inc()        { count = count + 1; }
    public int  value()      { return this.count; }      // this 指代当前对象
    public static int twice(int x) { return x * 2; }     // static 成员
    int count;                                           // 未标 public，仅类内可见
}

Counter c = new Counter();            // 在堆上创建，由 c（引用/句柄）持有
c.inc();                              // 用点号调用实例方法
c.inc();
printf("%d\n", c.value());            // 输出 2（仅类内可见的 count 经 public 方法读出）
printf("%d\n", Counter::twice(21));   // static 通过类名加 :: 访问，输出 42
// c 离开作用域后自动回收，无需手动释放
```

### 成员与方法

- **实例方法**：通过隐式的 `this` 指代当前对象，读写其自身的数据。
- **static 成员/方法**：属于类本身而非某个实例；在本模块内以零初始化的状态存在，通过类名加 `::` 访问，如 `C::m()`。这里的 `static` 仅修饰**类成员**，与 C 里文件作用域的 `static`（限定符号仅在单个翻译单元内可见）不是同一回事；Co 的模块内部可见性以**省略 `public`** 表达（见「模块」章节）。
- **可访问性**：类成员/方法以 `public` 决定是否对外——未标 `public`（或显式标 `private`，二者在此层面完全等价）即仅在类内可见，标了 `public` 才对外可见。`public`/`private` 在成员/方法层面**不额外限定类内不同区域的访问**；Co 里唯一真正施加访问限制之处是属性的 `get`/`set` 访问器（见下节「属性」）。

### 属性（Property）

属性是「看起来像字段、实为一对访问器函数」的封装形态：读它触发 `get`、写它触发 `set`，书写方式与普通成员一致（`obj.p` 取值、`obj.p = x` 赋值）。属性声明在类体内，形如「`[public]` 类型 名字 `{ 访问器… }`」：

```
class Box
{
    int _a;                              // 供显式访问器读写的后端字段
    Box() { _a = 0; }

    public int a {                       // 读写皆可（属性对外可见）
        get { return _a; }               // 显式 get：体内引用 _a
        set { _a = value; }              // 显式 set：value 为隐式入参
    }

    public int b { get; set; }           // 自动属性：编译器合成隐藏后备字段
    public int c { get; }                // 只读：仅 get，类外写会报错
    public int d { set; }                // 只写：仅 set，类外读会报错
    public int e { get; private set; }   // 对外只读：set 私有，仅类内可写
    public int score { get; set { field = value < 0 ? 0 : value; } }   // 混写：get 自动、set 用 field 直接读写合成后备字段
}
```

- **访问器形态**：带 `{ … }` 为显式体（体内引用你自己声明的字段，如 `_a`）；只写 `;`（无体）为**自动属性**，编译器合成一个隐藏后备字段并纳入对象布局、初值为 `0`（若要自定义初值，请改用「显式体 + 自写字段 + 构造函数赋值」）。
- **后备字段与 `field`**：自动属性（`get;`/`set;`）由编译器合成一份**后备字段**保存数据。只要属性**留有任一自动部分**（至少一个是裸 `get;`/`set;`），即便另一个访问器写成显式体，体内也可用上下文关键字 **`field`** 直接读写这份后备字段——例如 `public int score { get; set { field = value < 0 ? 0 : value; } }`。`field` 是上下文关键字、非保留字：仅在属性访问器体内有此含义，体外仍是普通标识符。
  - `field` 仅在**自动实现**的属性访问器体内有效；两个访问器都给显式体（无自动部分＝无后备）时写 `field` 会被拒：`keyword 'field' is only available in an auto-implemented property`。
  - **与同名成员的区别**：类里若另声明了名为 `field` 的成员，访问器体内裸 `field` 仍指**后备字段**，要访问那个成员须写 **`this.field`**。
- **只读 / 只写**：只给 `get` 即只读，写它会报 `property '…' is read-only (no setter)`；只给 `set` 即只写，读它会报 `property '…' is write-only (no getter)`。
- **复合赋值 / 自增自减**：`obj.p += x` 依「先 `get`、运算、再 `set`」展开；`obj.p++` / `++obj.p` / `obj.p--` / `--obj.p` 同样经此分派（后缀形返回自增/自减前的旧值、前缀形返回新值）。二者都要求属性 `get` 与 `set` 俱备且各自可见，否则按只读 / 只写 / 私有给出相应诊断。
- **不可取址**：属性是访问器函数对、没有独立存储地址，`&obj.p` 会被拒绝：`cannot take address of property 'p'`。
- **访问器级可见性**（与属性整体的 `public` 正交）：每个访问器可单独前缀 `public` 或 `private`，**缺省 `public`**。标 `private` 的访问器**只能在定义该属性的类之内调用**（含类内经同类其它实例句柄读写）；从类外使用会被拒绝——
  - `set` 私有：类外写 `obj.p = x` 报 `setter of property 'p' is private`，属性对外表现为**只读**；
  - `get` 私有：类外读 `obj.p`（含复合赋值 / 自增自减）报 `getter of property 'p' is private`，属性对外表现为**只写**。

  这与「`private` 在模块/成员层面只是『省略 `public`』的显式写法、不新增限制」不同——**属性访问器上的 `private` 会真正施加访问限制**，是 Co 可见性规则中唯一带强制的一处。
- **跨模块**：导入他模块的 `public class` 后，其属性及访问器级 `public`/`private` 同样生效——类外（含宿主模块）写私有 setter、读私有 getter 会被一致拒绝。

> 当前边界：句柄（class 引用）类型的属性**已支持**——读取（`get`）返回**借用**引用、写入（`set`）按「先保留新值、后释放旧值」维护引用计数，与其他拥有者安全共享。属性的取址（`&obj.p`）已明确拒收，自增自减（`obj.p++` / `obj.p--` 及其前缀形）已支持。

### 生命周期（对外行为）

Co 自动管理 class 对象的内存。使用者可观察到以下规则：

- **无需手动释放**：当一个对象不再被任何变量持有、且相关作用域结束时，它会被自动回收。
- **无悬垂、无双重释放**：把对象交给另一个变量不会导致同一对象被释放两次，也不会产生指向已释放内存的悬垂引用。
- **析构时机**：对象回收前会执行其析构函数；析构执行时对象的成员仍可读，可用来释放外部资源。
- **循环引用暂不保证回收**：若两个对象相互引用形成闭环，当前不保证这一组对象被自动回收；其处理策略仍在定稿中。
- **交出自有句柄安全**：把已被局部变量或成员持有的对象作为返回值交出是**合法**的，编译器会自动为其补一次引用；返回后该对象与属主安全共享，不会被提前释放。

> **兼容性告知**：Co class 的内存管理既不同于 C++（需手工 RAII 或显式释放），也不同于垃圾回收（GC，不保证回收循环引用）。请不要用其中任何一方的直觉来套用 Co 的行为。

### 构造与析构

- **构造函数** `CLASS_NAME()`：在对象创建时执行，用于初始化。构造函数与类同名、无返回类型。
- **析构函数** `~CLASS_NAME()`：在对象被回收前执行，用于释放资源。析构函数以 `~` 前缀、无返回类型。

### 当前边界（暂不支持 / 未定稿）

以下项目前不支持或仍在定稿，使用时编译器会给出诊断：

- 继承、虚函数与多态未纳入当前基准。
- 环形引用不保证自动回收；如需断开，可用 `delete` 显式销毁。

> 以下能力已支持（曾列为边界，现已开放）：句柄（class 引用）类型的属性（读取返回借用、写入按引用计数保新放旧）；成员与形参类型不再限于基元——`class` 句柄、`struct` / `union` / 数组均可作成员或形参（值语义内嵌、借用形参与宿主拥有的引用计数规则见相关章节）；`delete` 显式释放可用；被 `import` 的 `public class` 可跨模块消费（见「模块」章节）。

### 保留名

`__new__`、`__retain__`、`__release__`、`__delete__` 为语言保留名，用户代码不得用它们命名变量、函数、类或成员。


