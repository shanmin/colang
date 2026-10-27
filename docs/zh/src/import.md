# import（模块导入）

> 自 **0.3.0** 起，`import` 已实装、用于消费 `.co` 源文件模块。`.obj` / `.dll` / `.so` 作可导入模块仍待后续里程碑。

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

## 独立模块闭包

自 **0.3.2** 起，每个 `.co` 模块都是**独立的编译闭包**：逐模块各自建类型表与符号表、各自分析，主源只经**导出面**（`public` 名称）看到模块。这带来两条用户可见的保证：

- **同名类型异布局互不干扰**：两个模块各定义同名 `struct Config` 而布局不同，各自 `sizeof` 取自身布局，不存在「后导入者顶替」。
- **ambient 切断**：模块内 `#include`（如 `<stdio.h>`）带入的**环境符号**（`printf` 等）**不进入模块导出面**。宿主即使也 `#include <stdio.h>`，其 `printf` 与模块互不冲突、不再报「导入非 public 符号」。

## 可见性规则

- **`public` 是跨模块可见性的唯一闸门**：未标 `public` 的定义仅在源模块内部可见、不能被任何 `import` 拿到。
- 具名导入非 `public` 符号：编译期报错 `cannot import non-public symbol`。
- `import *` 命中的模块中非 `public` 符号：静默跳过、只导入 `public` 面。
- 限定访问 `ModAlias.x` 命中模块中不存在的或非 `public` 的名称：报错 `no public symbol 'x' in module 'ModAlias'`。
- 主源上下文引用一个**存在于符号表但既未通过 `import` 具名引入、又未以限定名访问**的模块符号：编译期报错 `identifier not imported`。
- **模块内部**引用**同模块**的非 `public` 符号：合法、不受限制。

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

## 当前实装边界（0.3.2）

已实装：
- `.co` 源文件的完整消费链路（词法/语法/加载/逐模块闭包分析/合并/可见性/strip/入口集成）；
- 具名与 `*` 两种形态；`import … as ModAlias` 模块别名**已启用限定访问**；
- **模块别名限定访问** `m.f()` / `m.f` / `m.x` / `m.x = v`（含写与本地同权、`const` 全局写拦截）；
- **独立模块闭包**：逐模块类型/符号隔离，同名异布局 `struct` 互不顶替；
- **ambient 切断**：模块内 `#include` 带入的环境符号不入导出面；
- **撞名分流①~⑧**：遮蔽/歧义/跨模块同名共存（链接期以 `__<短名>__<名>` 合成符号并存）；
- 集中拓序 + 首次加载执行一次；环检测与去重；
- imported 非 `public` 符号在 codegen 强制 `InternalLinkage`；
- 跨模块 `public struct` / `union` / `enum` 类型引用（可作值类型变量、成员访问、取地址传参）；
- 跨模块**裸标签**：被导入模块里标 `public` 的 `struct`/`union`/`enum`，消费方引用时可省略关键字（`Point p;`）；
- 跨模块**编译期常量**：被导入模块的 `public const int`（如 `winapi.co` 的 `WM_*` 系列）在消费方可作普通只读值、数组维度、`switch` 的 `case` 标签使用；
- **DLL / WinMain 顶层初始化载体**：产 `-o *.dll` 时合成 `DllMain`、在 `PROCESS_ATTACH` 跑各模块顶层初始化（用户自写 `DllMain` 改名转发）；GUI exe 的 `WinMain` 入口下，顶层裸句与运行期初值全局由合成 WinMain 载体承载（见「模块」章节）。

保留名：`__init__*__` 为模块初始化调度协议保留前缀，**禁止用户代码自定义**（编译期硬错 `reserved protocol name '__init__*' cannot be defined by user code`）。

本批未覆盖、归后续里程碑：
- `.obj` / `.dll` / `.so` 作为可导入模块（须携带编译期声明元数据）；
- `__init__` 用户手写定制。
