# 修改记录

本记录按发布版本列出 Co（colang）语言/编译器的主要变化。
版本策略：0.* 为开发测试版，1.* 为正式版，2.* 为自举版。

## 0.3.1
- `winapi.co` 模块：以 `import … from "winapi.co"` 取代 `windows.h`，经 SDK 核验提供 215 个 Win32 API 原型、13 个结构/联合体布局、266 个常量；覆盖窗口创建、消息循环、绘制、文本度量等常用 GUI API（暂不含对话框、菜单、通用控件）。
- Windows GUI 程序：`[[subsystem("windows")]]` 属性（或定义 `WinMain`）产出无控制台窗口程序。
- Co 层增强：被 import 模块的 `public const int` / `public struct` 等可直接跨模块使用；指定初始化器支持链式设计符与 `union` 匿名成员。
- C 兼容性扩充：标识符通用字符名（`\uXXXX`）、`u8`/`u`/`U`/`L` 前缀字符常量、`sizeof(字符串字面量)`、`static_assert` 多作用域、`[[nodiscard]]`/`[[noreturn]]`/`[[deprecated]]` 属性。
- 诊断改进：`const` 只读强制、不完整类型前置诊断、`switch` 非常量 `case` 标签报错，措辞与主流编译器对齐。
- 修复：全局初始化式误折为 null 导致的运行期崩溃等两族问题。
- 发布包扁平为 `colang.exe + include/`（免装运行库、免额外配置）；Linux（WSL）构建打通。

## 0.3.0
- 链接器内置：`-c` 产 COFF `.obj`，默认动作把目标文件链为可执行文件，**不再依赖外部 `clang`**。
- 默认静态链接 CRT：产物 exe 免装 VC 运行库即可分发。
- `-o <名称>.dll` 产出 Windows 动态库：含导出表、并随基名生成 `.lib` 导入库。
- `[[lib("user32")]]` 声明库归属，无需命令行。
- 链接需本机装有 Windows SDK/VC 运行库（自动定位）。
- `import`（`.co` 源合并）落地：
  - 语法：`import * | name [as alias], … from ("file" | module_name) [as ModAlias];`；
  - 只消费 `.co` 源文件；`public` 是跨模块可见性的唯一闸门；
  - 顶层裸代码首次加载时集中拓序执行一次（DFS 后序、主入口前统一 emit）；
  - 规范化绝对路径去重；DFS 环检测报完整路径；
  - 支持简化标识符形式 `from a` 等价 `from "a.co"`（仅同目录）。
- 未导入符号在主源上下文引用报错；非 `public` 符号命名/star 均拒收；
- imported 非 `public` 符号在 codegen 强制 `InternalLinkage`（不注入外部可见 LLVM 值）。

## 0.2.0
- 增加裸代码、函数public、import支持；
- 增加dll/so生成；
- 根据C语言测试套件，修复若干问题。

## 0.1.0
- 开发测试版本，已完成 C 语言编译器。

## 0.0.x
- 开发测试版本（历史）。功能与语法以当时测试版本为准，非发布基准。
