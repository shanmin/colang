# 修改记录

本记录按发布版本列出 Co（colang）语言/编译器的主要变化。
版本策略：0.* 为开发测试版，1.* 为正式版，2.* 为自举版。

## 0.4.1
- 诊断符合性大批：编译期错误/警告及诊断上下文全面向 clang 与 ISO C/C++ 对齐，收敛多处静默错值/误判（含文件作用域 `bool`/整型常量折叠按「非零即真」修正）。
- 诊断改进：每条 error/warning 默认附出错行 ±2 行源码与 caret 定位；新增 `-fno-diag-context` 关闭。
- 命令行与源面收紧：新增 `-l`/`--lib` 指定外部库；`[[lib]]` 与 `class` 定义限定于 `.co` 源（`.c`/`.h` 中拒收）。
- Co 层健壮性：修复被 `import` 模块内按值使用跨模块 `public struct` 的退化/崩溃。
- `winapi.co` 继续扩容：在 0.4.0 的 323 原型 / 39 结构·联合体 / 396 常量基础上增至约 356 原型 / 45 结构·联合体 / 503 常量，新增 winsock（ws2_32）套接字族、crypt32 字符串编解码族等。

## 0.4.0
- `class` 引用类型（heap-only + 自动引用计数）：`new` 实例化、`this` 成员访问、实例方法与静态成员 `C::m()` 访问；出作用域按帧逆序自动释放，`break`/`continue`/`goto` 路径终结同样正确回收。
- 成员与形参类型放开：基元、`class`、`struct`/`union`/数组均可——`struct`/`union`/数组作成员为值语义内嵌（不计引用数、随宿主分配与释放）；class 句柄作形参取**借用**语义（不转移所有权），作成员取**宿主拥有**语义（赋值即持有，析构时级联释放）。
- `delete` 语句：强制销毁（绕过计数，执行析构 + 级联释放 + 回收），槽位置空防二次释放；可用于手工断开环形引用。
- `public class`：单模块及 `-o *.dll` 导出生命周期协议、实例/静态方法与成员。
- 当前边界：无继承与虚函数；被 `import` 模块里的 `public class` 暂不可跨模块消费；环形引用不做自动断环（用 `delete` 强破）。
- `winapi.co` 大幅扩容：经 Windows SDK 10.0.26100.0 逐条核验，现累计 323 个 Win32 API 原型、39 个结构/联合体布局、396 个常量；在窗口、消息循环、绘制等基础上新增 对话框、菜单、通用控件、系统托盘图标、DWM、PDH、输入法（imm32）、文件时间/目录枚举 等 API 与类型。
- Co 层裸标签（C++ 风格）：`.co` 里 `struct`/`union`/`enum` 变量定义、形参与 `sizeof` 可省略关键字（`Point p;`），被 import 的 `public` 标签同样可裸用；`.c` 档不启用，与 ISO C 逐字一致。
- 版本号扩为四段 `MAJOR.MINOR.PATCH.Build`：第四段为构建计数，MSVC 真实重编时自增（改三段版本时自动归零）；`-v` 输出与产物 PE 版本信息同步显示。
- 新增默认设置头 `include/colang.h`：编译时先于主源展开（只作用主源、不作用于被 import 的模块），供放置全局宏定义。

> 以上三条（裸标签、四段版本号、默认设置头）属先前已发布但漏记的改动（前两条在 0.3.1 周期内落地、第三条在 0.3.2 周期内落地），现一并补入。

## 0.3.2
- 内置资源编译器：解析 `.rc` 语法，支持 ICON / BITMAP / RCDATA / STRINGTABLE / VERSIONINFO / ACCELERATORS / DIALOGEX / MENU 等资源类型，产物直接嵌入 PE 可执行文件（与 rc.exe 逐字节对齐）。
- Linux 交叉编译与原生链接：`--target=x86_64-unknown-linux-gnu` 产出 ELF 目标文件，进程内 lld ELF 链接器端到端可执行；LP64 数据模型（long=8, void*=8, wchar_t=4）如实分派。
- per-module 编译模型转正：每模块独立闭包，跨模块同名符号自动分流（不撞名）；支持限定访问 `mod.func()` / `mod.var`。
- `wchar_t` 全链路 target-aware：宽度/符号性随 target（Win unsigned short 2 字节 / Linux int 4 字节）；`wint_t`、`L'…'` 溢出判据、TC 前缀字符均正确分派。
- const 诊断对齐 clang：只读违规后附 `note: … declared const here` 指向声明位置；新增 `-Wdefault-const-init` 告警（函数内 const 对象/含 const 成员无初始化器）。
- DLL 动态库产出：合成 `DllMain` 入口载体、顶层初始化代码驱动、`public` 函数写入导出表。
- 整型字面量类型选取、位域掩码、动态链接 CRT 等多项修复与覆盖面强化。

## 0.3.1
- `winapi.co` 模块：以 `import … from "winapi.co"` 取代 `windows.h`，经 SDK 核验提供 Win32 API 原型 / 结构·联合体布局 / 常量；模块随版本滚动扩容，至 0.4.0 累计 323 原型 / 39 结构·联合体 / 396 常量。首发覆盖窗口创建、消息循环、绘制、文本度量等常用 GUI API。
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
