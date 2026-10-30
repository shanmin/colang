# 属性（`[[...]]`）

## 语法

Co 采用 C23 的属性拼写：一对方括号 `[[` `]]` 包住若干属性，属性之间用逗号分隔，写在声明之前：

```
[[ ATTRIBUTE1, ATTRIBUTE2(...) ]]  声明
```

Co 当前赋予实际意义的属性有五个：

| 属性 | 作用于 | 效果 |
| --- | --- | --- |
| `[[lib("X")]]` | 顶层函数声明、全局变量声明 | 声明该符号由外部库 `X` 供给，链接期自动拉入 `X` |
| `[[deprecated]]`、`[[deprecated("消息")]]` | 顶层函数、全局变量 | 引用该符号时发一条告警 |
| `[[nodiscard]]`、`[[nodiscard("理由")]]` | 有返回值的函数（含原型） | 调用返回值在语句位被丢弃时告警；显式 `(void)f()` 不告警 |
| `[[noreturn]]` | `void` 函数 | 校验函数体不返回：体内 `return` 与落穿都告警；调用点按「不返回」处理 |
| `[[subsystem("windows")]]` | 入口函数（`main` / `WinMain`） | 把链接产物切为 Windows GUI 子系统（无控制台） |

属性只用 `[[` 这一种拼写；C 里等价的 `<:` `::>` 替代拼写 Co 不支持。

## `[[lib("X")]]`——声明库归属

外部函数（如操作系统 API）由哪个库实现，过去无法表达：只写原型而不告诉链接器库名，链接就会报未定义符号。`[[lib("X")]]` 补上这个归属：

```
[[lib("user32")]] int GetSystemMetrics(int nIndex);
[[lib("user32")]] unsigned int GetDoubleClickTime(void);

public int screen_width()
{
    return GetSystemMetrics(0);   // 无需命令行给 -luser32，链接器自动拉入 user32
}
```

`X` 是**链接名**（不含扩展名、路径、空白、引号、冒号），由链接器按目标平台补全，写法与命令行 `-l` 同形。因此 `"user32"` 合法，`"user32.lib"`、`""`、`"sub/user32"`、`"C:x64/user32"` 都会被拒绝。

约束：

- **只能写在 `.co`/`.CO` 源文件里**：`[[lib]]` 是 Co 新定义属性，`.c`/`.h` 及其它来源（含被 `.co` `#include` 的头文件）出现即报错 `'lib' attribute is only allowed in .co source files`——判定看**属性实际所在文件**，`.h` 不因被 `.co` 包含而放宽。因此「把外部库归属写在头文件里供多个 `.co` 共用」行不通，必须写在 `.co` 内。
- **只能挂在声明上**：函数须是纯原型（无函数体），全局变量须是纯外部声明（无初始化式）。声称来自外部库却又在本模块写出定义，是矛盾，编译器拒绝。
- **与 `public`、`static` 互斥**：`public` 是导出、`lib` 是导入，不能落在同一符号上；`static` 表示内部链接，也不可能由外部库供给。
- **同名符号的库归属必须一致**：同一名字重复声明且库名相同是合法的；同名却标了两个不同的库，编译器报错（不做静默取舍）。
- 一个声明上写两个 `[[lib]]` 报错。

## `[[deprecated]]`——标记弃用

给函数或全局变量标 `[[deprecated]]`，此后每次引用都会发一条告警，提醒改用别的接口；带消息可写明替代方案：

```
[[deprecated]] int old_add(int x);
[[deprecated("use new_api instead")]] int new_api(int x);

int use(int x)
{
    return new_api(x);   // warning: 'new_api' is deprecated: use new_api instead
}
```

- 属性挂在前置声明、定义写在另一处时，按**同一实体**合并——调用点照样告警。
- **同一位置重复标注幂等接受**（如 `[[deprecated, deprecated]]`、相邻两个属性列表），理由串**取最先写的一个**（first-wins），后续重复整体忽略——与 clang 同口径。
- 告警**不改变程序退出码**，也不阻断编译。
- 局部作用域里的声明上的 `deprecated` 当前不生效。

## `[[nodiscard]]`——返回值不得丢弃

给有返回值的函数标 `[[nodiscard]]`，此后调用它却把返回值在语句位丢弃，就会收到一条告警；带理由可写明应当如何处理：

```
[[nodiscard]] int check(int x);
[[nodiscard("记得处理返回值")]] int compute(int x);

int run(int x)
{
    check(x);                 // warning: ignoring return value of function declared with 'nodiscard' attribute
    compute(x);               // 同上，告警尾部附带理由： : 记得处理返回值
    (void)check(x);           // 显式丢弃，视为有意为之，不告警
    return check(x) + 1;      // 使用返回值，不告警
}
```

- 告警文本与 clang（`-std=c23`）同形；带理由时尾部 `: 理由` 逐字保留。
- **只有原型（无函数体的声明）标了也生效**：原型与定义分开书写时按同一实体登记。
- 标在**无返回值**（`void`）的函数上报错：`attribute 'nodiscard' cannot be applied to functions without return value`。
- 同一位置重复标注（`[[nodiscard, nodiscard]]`）幂等接受，与 clang 同口径。

## `[[noreturn]]`——声明函数不返回

给 `void` 函数标 `[[noreturn]]`，编译器校验它确实不返回：函数体内出现 `return` 逐条告警，函数体落穿（执行到末尾自然结束）另报一次；同时调用点按「不返回」处理，被调函数之后的死代码不再引发「缺 `return`」类误报：

```
[[noreturn]] void bail(void);

int work(int c)
{
    if (c)
        bail();               // 调用点按不返回处理
    return 0;                 // 不再误报「某路径无返回值」
}
```

- 告警文本与 clang 同形：`function 'h' declared 'noreturn' should not return`。
- `[[_Noreturn]]` 为 C23 已弃用的旧拼写，Co 接受且与 `[[noreturn]]` 同义（不对旧拼写另发弃用警告）。
- 同样**只看标注、不看书写顺序**：原型标注即登记；重复标注幂等接受。

## `[[subsystem("windows")]]`——切换链接子系统

默认情况下，Co 生成的是**控制台**子系统程序（运行时会带一个控制台窗口）。把 `[[subsystem("windows")]]` 标在入口函数上，链接产物即切为 **Windows GUI 子系统**（无控制台窗口）：

```
[[subsystem("windows")]]
int WinMain(void* hInstance, void* hPrevInstance, char* lpCmdLine, int nCmdShow)
{
    // 创建窗口、跑消息循环……
    return 0;
}
```

取值只有两个：

- `"windows"`：Windows GUI 子系统，无控制台。
- `"console"`：控制台子系统（显式写法，等价于不标）。

约束与要点：

- **只在入口函数（`main` 或 `WinMain`）上生效**；标在其它函数上会发一条告警（不阻断编译）。
- **一个声明上写两个 `[[subsystem]]` 报错**；取值既非 `"windows"` 也非 `"console"` 报错。
- **定义 `WinMain` 即隐含 windows 子系统**，无须再写 `[[subsystem("windows")]]`（写了也不冲突）。
- 程序入口与 `WinMain` 的更多约定见「程序入口与执行」章节。

## 探测与未识别属性

预处理指令 `__has_c_attribute(名字)` 返回 `1` 或 `0`，可用于条件编译：

```
#if __has_c_attribute(lib)
[[lib("user32")]] int GetSystemMetrics(int nIndex);
#endif
```

- 已知但当前尚无实际效果的属性（`maybe_unused`、`fallthrough` 等）**静默接受**——不报错也不刷警，为的是不打扰从 C/C++ 迁来的代码。
- 未识别的裸名属性发一条 `unknown attribute 'x' ignored` 告警并跳过，不阻断编译。
- 带命名空间的属性名（如 `[[colang::lib]]`）**一律报错拒收**：Co 不假装支持自己兑现不了的厂商属性。

## 与 C / C++ 的关系

| 方面 | Co | 相对语言 |
| --- | --- | --- |
| 属性拼写 `[[...]]` | 沿用 C23 | 与 C23 相同 |
| `[[deprecated]]` | 支持（诊断层，不进产物） | 与 C23 语义一致 |
| `[[nodiscard]]` / `[[noreturn]]` | 首批语义已生效（告警与校验，与 clang 同形） | 与 C23 基本一致（类型级标注等边角见下） |
| `[[maybe_unused]]` / `[[fallthrough]]` | 接受但不产生效果 | 与 C23 不同（C23 有实际语义） |
| `[[lib("X")]]` | Co 新定义，C 标准无此属性；且仅限 `.co`/`.CO` 源文件 | Co 独有 |
| `[[subsystem("windows")]]` | Co 新定义，C 标准无此属性 | Co 独有 |
| `<:` `::>` 替代拼写 | 不支持 | 与 C23 不同 |

> **兼容性告知**：
> 1. `[[lib("X")]]` 是 Co 自定义属性。把 `.co` 交给真正的 clang/clangd 看时，它会按未知属性 `-Wunknown-attributes` 警告并忽略——**这是必然如此、不是缺陷**；外部工具不会校验库归属，只有 Co 编译器消费它。
> 2. 从 C/C++ 迁移时，`#pragma comment(lib, "X")` 与 `__declspec(dllimport)` 本编译器**既不实现、也不模拟**，库归属须显式改写为 `[[lib("X")]]`；而 `[[lib]]` **只能写在 `.co` 文件里**——纯 `.c` 档请改用命令行 `--lib <name>` 指定外部库（见「编译器命令行」章节），不必把声明搬进 `.co`。
> 3. `[[deprecated]]` 是纯前端诊断，Co 与 clang 一样**不把它编进产物 IR**，因此不会传导到依赖该产物的下游模块。
> 4. `[[subsystem("windows")]]` 同样是 Co 自定义属性：真 clang/clangd 会按未知属性警告并忽略（**必然如此、非缺陷**）；子系统切换只有 Co 编译器消费。

## 厂商兼容扩展：`__attribute__`（GCC/Clang）

> **定性（仅为兼容性而加）**：本小节的语法**非 ISO C**（C89–C23 皆无此拼写，标准属性是上一节的 `[[...]]`），**也不是 Co 方言特性**。它**纯粹为兼容来自 GCC/Clang 的既有源码而增设**——让这些代码不必为编译而逐处改写——**不属于 Co 的语言设计**。新写代码请优先用 C23 的 `[[...]]` 属性与 `_Alignas(N)`，不要依赖 `__attribute__`。

Co 接受 GNU 的 `__attribute__((...))` 拼写，并**分级**处理：

- **`aligned(N)`——赋予真语义**：抬高对象 / 成员 / `struct`·`union` 类型 / `typedef` 的对齐（可经 `sizeof`、`_Alignof`、`offsetof` 观察到），与本机 clang `-std=c23` 逐字一致。它只做**单向提升**：对齐取 `max(自然对齐, N)`，**永不降低**。`N` 须为 2 的幂，否则报错（与 clang 同）。
- **其余所有属性——接受语法、忽略语义、发告警**，按风险分**两级**：
  - **强警告（可能影响内存布局 / 调用约定 / 链接）**：`packed`、调用约定类（`stdcall` / `cdecl` / `fastcall` / `thiscall` / `vectorcall`）、`section`、`weak`、`alias`、`dllimport` / `dllexport`、`constructor` / `destructor` 等。文案形如 `gnu attribute 'packed' ignored: may affect layout/ABI (non-standard, GCC/Clang compatibility)`。
  - **轻警告（仅优化 / 诊断 / 信息性，忽略不改变可观察行为）**：`noinline`、`always_inline`、`format`、`unused`、`used`、`pure`、`const`、`visibility`、`deprecated` 等，以及**未识别的属性名**。文案形如 `gnu attribute 'noinline' ignored (non-standard, GCC/Clang compatibility)`。
- **以上告警都不阻断编译、不改变退出码。**

`__attribute__` 可出现的位点（与 GCC 一致）：声明说明符之前；`struct`/`union` 关键字与标签名、标签名与 `{` 之间；闭花括号 `}` 之后（`typedef`/变量名之前）与 `typedef` 名之后；返回类型与函数名之间；cast 的抽象声明符类型之前；函数类型括号内 `*` 之前。

```
struct __attribute__((aligned(32))) Block { char c; };   // _Alignof(Block) == 32
int  __attribute__((aligned(16))) giant;                  // 抬高对齐
void __attribute__((stdcall)) api(void);                  // 接受 + 强警告（stdcall 被忽略）
int  __attribute__((noinline)) leaf(void) { return 42; }  // 接受 + 轻警告
```

> **兼容性告知（重要）**：
> 1. 真 GCC/Clang 会**完整实现**这些属性，Co **只兑现 `aligned`**、其余一律忽略。若源码依赖 `packed` 压缩布局或某种调用约定，它在 Co 下**能编过、但行为与 GCC/Clang 不同**（结构不按 1 字节对齐、调用约定不变）——这正是 Co 对每个被忽略的「布局/ABI 类」属性都发**强警告**、而非静默接受的原因。
> 2. 需要真实布局效果时，`aligned` 可改写为 C23 的 `_Alignas(N)`；`packed` 等在标准里无对应物、暂无等价写法。
> 3. `__declspec(...)`（MSVC 扩展）**不在本扩展范围内、不支持**；库归属请用 `[[lib("X")]]`。
> 4. `aligned` 的罕见形式（无参 `aligned`、`aligned(_Alignof(T))`、`aligned(类型)`）本批**降级为轻警告忽略**。

## 当前边界（编译器实装状态）

- `[[lib("X")]]` 落地为 COFF `.drectve` 段的 `/DEFAULTLIB` 链接指令，按声明首现序去重；这依赖链接步能消费该指令，**尚不能**做到「同名符号按具体 dll 精确绑定」——多 dll 同名符号的 per-symbol 绑定属后续链接器能力。
- `[[deprecated]]` 生效范围限于顶层函数与全局变量的引用点告警；局部声明位不生效，C23 允许的第二个标注位点（声明符之后）未建模。
- `[[nodiscard]]` / `[[noreturn]]` 首批语义已落地（丢弃告警 / 不返回校验）。边界：标在非函数声明上（如 `[[nodiscard]] int x = 7;`）静默接受；挂在 `struct`/`union` 类型或函数指针类型上（C23 允许）未建模；`[[_Noreturn]]` 旧拼写不报弃用警告。
- `[[maybe_unused]]`、`[[fallthrough]]`、`[[align]]` 等仍仅静默接受——Co 当前没有「未使用变量」「隐式贯穿」类告警可供抑制，待相应告警落地时再启用。
- `[[subsystem("windows")]]` 经链接器命令行 `-subsystem:windows` 落地，其优先级高于静态 CRT 内嵌的 `.drectve`；入口随之在 `WinMain` 时取 `WinMainCRTStartup`、否则钉 `mainCRTStartup`。仅 Windows/COFF 目标有效；W 版 `wWinMain` 暂未支持。
