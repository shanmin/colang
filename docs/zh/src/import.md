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
| `ModAlias` | 给来源模块起的本地别名（**本批已解析语法位、暂未启用**，`m.f()` 限定访问归后续） |

## 文件名与路径解析

- **双形式**：`"file"` 与 `module_name` 二选一。
  - 字符串字面量：支持跨目录（相对或绝对路径），扩展名可省略（无后缀自动补 `.co`）。
  - 简化标识符：等价于 `from "<name>.co"`，仅在**引用方文件所在目录**查找。
- **路径基准**：以**引用方文件所在目录**为基准解析相对路径，而非当前工作目录。

## 可见性规则

- **`public` 是跨模块可见性的唯一闸门**：未标 `public` 的定义仅在源模块内部可见、不能被任何 `import` 拿到。
- 具名导入非 `public` 符号：编译期报错 `cannot import non-public symbol`。
- `import *` 命中的模块中非 `public` 符号：静默跳过、只导入 `public` 面。
- 主源上下文引用一个**存在于符号表但未通过 `import` 引入**的模块符号：编译期报错 `identifier not imported`。
- **模块内部**引用**同模块**的非 `public` 符号：合法、不受限制。

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

## 当前实装边界（0.3.0）

已实装：
- `.co` 源文件的完整消费链路（词法/语法/加载/合并/可见性/strip/入口集成）；
- 具名与 `*` 两种形态；`import` 语法位支持 `as ModAlias`（本批暂未启用限定访问）；
- 集中拓序 + 首次加载执行一次；环检测与去重；
- imported 非 `public` 符号在 codegen 强制 `InternalLinkage`。

本批未覆盖、归后续里程碑：
- 模块别名限定访问 `m.f()` / `m.x`（语法位已解析但未接入 Sema/Codegen）；
- `.obj` / `.dll` / `.so` 作为可导入模块（须携带编译期声明元数据）；
- 跨模块 `struct` / `enum` 类型引用（本批 `import` 主要面向函数、全局变量与顶层裸指令）；
- DLL 场景下 `DllMain` 与顶层段触发时机。
