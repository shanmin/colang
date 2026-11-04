# 修改记录

本记录按发布版本列出 Co（colang）语言/编译器的主要变化。
版本策略：0.* 为开发测试版，1.* 为正式版，2.* 为自举版。

## 0.5.4
- **`ptr - ptr` 指针差修复为 64 位**：此前两个指针相减的结果被截为 32 位有符号整数（`int`），超过 2^31 的差值取到负数；同时 `sizeof(p - q)` 折成 **4**（正确为 **8**）。本版改为与 clang 一致返回 64 位有符号（`ptrdiff_t` = `long long`），`sizeof` 同步修正。**注意**：在超大数组（>2 GiB）内计算偏移差的代码，原先得到负数或错误值，现在得到正确结果。
- **跨模块 `public struct` 含非 `public` 成员类型不再崩溃**：此前若 `public struct` 的值成员是一个未标 `public` 的 `struct`/`union`，导入侧编译器会**崩溃**（`0xC0000005`、零诊断）或发 LLVM 校验层无定位文案。本版改为：尺寸/对齐正常传递（`sizeof` 正确），但**在导入模块中展开该成员字段**按 clang 同文拒收（`incomplete definition of type '...'`），零崩溃。规避方式不变：被 `public struct` 引用的成员 struct 需一并 `public`。
- **`math.h` 补 `copysign`/`nan`/`nextafterf` 声明（5 名）**：`copysign`、`copysignf`、`nan`、`nanf`、`nextafterf` 此前在 `include\math.h` 中**无声明**——调用点按 C 隐式声明规则当返回 `int`、实际浮点结果从整数寄存器取出，**静默错值零诊断**。本版补齐原型（逐条取自 UCRT `corecrt_math.h`），原先调用这些函数得到错值的代码现在正确。
- **`float.h` 补 C23 浮点特性宏（9 条）**：新增 `FLT_DECIMAL_DIG`、`DBL_DECIMAL_DIG`、`LDBL_DECIMAL_DIG`、`FLT_EVAL_METHOD`（更新）、`FLT_HAS_SUBNORM`/`DBL_HAS_SUBNORM`/`LDBL_HAS_SUBNORM`、`DECIMAL_DIG`（更新）等 9 条宏定义，真值取本机 clang 23 资源头（`__float_float.h`），原先这些宏未定义导致特性探测失败。
- **`#if` 预处理求值器支持无符号语义**：修复 `#if` 表达式中 `u`/`U` 后缀常量、超 32 位无符号值的比较/运算按有符号处理的缺陷——如 `#if 2147483648u > -1` 原先判假、现判真（与 clang 一致）。同时修复 `#ifdef`/`#elifdef` 与 `#if defined()` 对内置伪宏（`__STDC_VERSION__` 等）判定不一致的分叉。
- **`winapi.co` 常量段修正 6 条错值 + 清 6 条凭空名**：`STD_INPUT_HANDLE`/`STD_OUTPUT_HANDLE`/`STD_ERROR_HANDLE` 三值此前**互换且偏移**（导致 `GetStdHandle(STD_OUTPUT_HANDLE)` 实际拿到输入句柄）；`GHND`、`TH32CS_SNAPALL`、`FILE_MAP_ALL_ACCESS` 值修正；6 条 SDK 中不存在的凭空名删除（其中 `THREAD_PRIORITY_REALTIME` 改名为真名 `THREAD_PRIORITY_TIME_CRITICAL`）。**使用这些常量的代码行为会改变**（原先传入了错值）。
- **新增 Win32 校验回路（基础设施，不影响用户 API）**：五件版本化脚本（`_winapi_extract_proto.ps1` / `_winapi_extract_layout.ps1` / `_winapi_extract_libmap.ps1` / `_winapi_audit_const.ps1` / `_run_winapi_verify.ps1`）建立 SDK 原型/布局/导出表/常量四张差表自动对账，登记 20 条新缺陷，为后续 winapi.co 修正提供闸门。

## 0.5.3
- **`winapi.co` 顶层类型别名改为可跨模块使用**：`DWORD` / `WORD` / `BYTE` / `BOOL` / `HANDLE` / `HWND` / `LPARAM` / `WNDPROC` 等 36 个别名此前未标 `public`，`import * from winapi;` 之后仍报 `use of undeclared identifier`（连用这些别名声明的变量名都会被级联报未声明）。本版全部改为 `public typedef`，导入后可直接按名声明、取用。**注意变化**：这些名字现在参与跨模块同名判定——被导入的模块与导入方各自若另写同名 `typedef` 或同名标签，会与之冲突。
- **跨模块调用非 `public` 函数改为编译期报错**：在非根模块里调用另一模块未标 `public` 的函数（有函数体）此前编译通过、链接才报裸名 `undefined symbol: X`（无源码位置）；现在编译期即报 `cannot import non-public symbol 'X'`，并带出错行的源码上下文。判据只覆盖**带函数体且未标 `public`** 的函数；`[[lib]]` / 外部 C 库一类非 `public` 纯声明继续放行。
- **调用参数个数检查收紧（含外部 API 声明）**：此前只有「形参数 ≥ 1 且有函数体」的函数才检查参数个数。本版起，手工声明的外部 API 原型（无函数体）与零形参函数 `int f()` / `int f(void)` 被传参同样报 `too many arguments` / `too few arguments`——此前是静默放行、按错误实参发出调用（寄存器/栈错位，`[[lib]]` 命中真实导出时连链接都能过）。`setjmp(env)` 按名豁免（其 `setjmp.h` 原型为 2 参、第二参由编译器注入）。
- **函数原型语义取 C23 口径（`()` 等价 `(void)`）**：空形参表即零参原型，旧式「非原型声明」与非原型相容规则整套取消。`void f(void); void f(int);` 现在判声明冲突；`void f(); void f(int a) { … }`（先前兼容）现在拒收；`f()` 声明后按带参调用报 `too many arguments`。K&R 花式定义仍不支持。**注意变化**：依赖旧式非原型声明的代码会转为编译错误——这与 clang `-std=c23` 判决一致。
- **`public typedef struct X Y;` 在非根模块内补齐类型完备性**：导入模块后对该别名的 `sizeof(Y)` / 局部 `Y y;` 此前伪报 incomplete；`p->a` 成员访问与 `f(Y h)` 值形参更会让编译器**崩溃（`0xC0000005`、零诊断）**；`Y inner;` 作 struct 值成员则在 LLVM 校验才失败。本版起别名五种形态与 `struct X` 标签形完全等价；真正的不完整类型仍在前端按 clang 同款诊断拒收。

## 0.5.2
- **`math.h` 补齐 `long double` 版数学函数声明（C 标准全族 56 个）**：`asinhl` / `acoshl` / `atanhl` / `sqrtl` 等 `l` 后缀函数此前在 `math.h` 中**零声明**。colang 按 C 的隐式函数声明规则把未声明函数**按返回 `int` 处理**，调用点遂从整数寄存器取浮点结果——**编译、链接全部通过且零诊断，运行得到的是垃圾整数**（同一程序每次运行还可能不同）。本版补齐 56 个 `l` 后缀声明，原型逐条对照 Windows UCRT 头取真值（含 `nexttowardl` 第二参为 `long double`、`ilogbl` 返 `int`、`lrintl`/`lroundl` 返 `long`、`llrintl`/`llroundl` 返 `long long`、`scalblnl` 第二参为 `long` 等宿主特异处）。**输出正确性修复**：原先调用这些函数而取到错值的代码，现在取到正确值。
- **新增自制最小 `complex.h`**：`#include <complex.h>` 此前因标准库无此头而直接报错。本版交付 `complex.h`，按 C 标准发出 `__STDC_NO_COMPLEX__`（如实告知实现不支持复数）。colang **仍不实现 `_Complex` / `_Imaginary` 复数类型**，本头有意**不定义 `I`**——把 `I` 定义成实数会让 `1.0 + 2.0 * I` 静默算成 `3.0`（错值且零诊断）；真写复数构造的源仍会得到显式诊断，不会退化成实数运算。只包含该头、不使用复数构造的程序现可正常编译运行。

## 0.5.1
- **告警诊断分级体系落地**：新增命令行 `-Werror`（将告警升级为错误）与 `-Wno-<组名>`（按组名静音特定告警），每条告警输出尾随其组名标注 `[-Wxxx]`，可按需精确开关。
- **补齐多个 clang `-Wall` / `-Wextra` 告警**：新增 `-Wunused-variable`、`-Wunused-function`、`-Wunused-but-set-variable`、`-Wswitch`（枚举 `switch` 未覆盖全部枚举值且无 `default`）、`-Wunused-parameter`，语义逐条与 clang 实测对齐；并修复调用无函数体的前置声明函数时告警被重复输出一次的问题。
- **位域与整型宽度诊断符合性**：位域宽度、`_BitInt` / `wchar_t` 单位宽度、同作用域重定义、`for` / `if` / `switch` 初始化作用域等原先被静默接受的写法，现一律按 clang 句式与行列定位拒收。
- **`winapi.co` 大幅扩容**：在 0.5.0 基础上新增进程/线程枚举、Shell/剪贴板/进程操作、Hook/键盘/鼠标输入、资源加载/电源/用户名、同步对象/内存映射/命名管道、文件属性/卷信息/安全令牌、堆/进程时间/句柄复制/IO 控制、优先级/控制台/环境字符串/符号链接/磁盘空间、控制台模式 / `VirtualQuery`、注册表枚举/删除、命名同步对象/可等待定时器/堆操作/页锁定等 API；现累计约 **481** 个 Win32 API 原型、**72** 个结构/联合体布局、**815** 个常量。全部结构体经 Windows SDK 10.0.26100.0 的 `sizeof` / `offsetof` 逐条实测核验。

## 0.5.0
- **`.co` 源文件里 `long` / `unsigned long` 改为 8 字节**（Windows 目标），`long` 与 `long long` 现为同一类型；`.c` / `.h` 不变，仍按目标数据模型取宽度（Windows 4 字节 / Linux 8 字节）。断代影响三项：① `_Generic` 中 `long` 与 `long long` 不能再并列；② `.co` 里 `long` 变量取地址传入以 `int` / `unsigned int` 声明的 API 原型（`winapi.co` 已按 SDK 拼写、不使用 `long`）判为指针类型不相容而报错；③ 输出 8 字节整数需用 `%lld`，`%ld` 只读 4 字节会取错值。
- `winapi.co` 常量段修正与补录（已对本段 509 条常量逐条比对 Windows SDK 10.0.26100.0 真值）：`SM_CXSCREEN` 78→`0`、`SOMAXCONN` -1→`0x7FFFFFFF`、`FILE_NOTIFY_CHANGE_LAST_WRITE` `0x8`→`0x10`、`PATCOPY` `0x00F00062`→`0x00F00021`——**使用这 4 个常量的代码运行行为会改变**（原先传入的是错值）。并补录 `FILE_NOTIFY_CHANGE_SIZE`、`SM_CYSCREEN`、`SM_XVIRTUALSCREEN`、`SM_YVIRTUALSCREEN`、`SM_CXVIRTUALSCREEN`、`SM_CYVIRTUALSCREEN`，此前需自写本地常量顶替的写法可回归 `winapi.co` 版。
- **字面量与类型说明符的诊断符合性**：以下写法原先被静默接受、折成错误值，或令编译器**崩溃且零诊断**，现一律拒收，句式与行列定位逐条对齐 clang：① 不完整的位精确后缀（`1w`、`0x1w` → `invalid suffix 'w' on integer constant`）；② 浮点型上的 `signed` / `unsigned`（`signed float`、`unsigned double` → `'float' cannot be signed or unsigned`）；③ 后缀大小写混用（`1lL`、`1wB`、`1.0lF`）；④ 数字串非法（`0b2`、`08`、`1f`）；⑤ 指数标记后缺数字（`1e`、`0x1p`）与十六进制浮点缺有效位/缺指数（`0x.`、`0x1.5`）；⑥ 超出 `unsigned long long` 全宽的整数字面量（`18446744073709551616`、`0xFFFFFFFFFFFFFFFFFFFF`）。合法拼写不受影响：`1uwb` / `1UWB` / `0b1010wb`、`1.0f` / `1.0L`、`08.5` / `09e1`、`9223372036854775808`（定 `unsigned long long`）均照常接受。
- `winapi.co` 批次 XIII 扩容与两处返回型缺陷修正：修正 `SendMessageA` 返回型 `int`→`long long`（SDK 为 LRESULT，x64 下 8 字节，原写法截断返回值高 32 位）与 `RegisterClassA` 返回型 `short`→`unsigned short`（SDK 为 ATOM=WORD，窗口类原子恒落 0xC000–0xFFFF、按 short 读为负）；新增 `WNDCLASSW` 结构、**18** 条原型（user32 `CreateMenu`/`GetMenu`/`SetMenu`/`GetSubMenu`/`EnableMenuItem`/`RegisterClassW`/`UnregisterClassW`/`RegisterHotKey`/`UnregisterHotKey`/`LoadStringW`，kernel32 `CopyFileW`/`RemoveDirectoryW`/`GetFileSizeEx`/`FormatMessageW`/`LocalAlloc`/`LocalFree`，gdi32 `GetTextMetricsW`，comdlg32 `GetSaveFileNameW`）与 **37** 条经 Windows SDK 10.0.26100.0 逐条核验的常量（`MOD_*`/`WM_HOTKEY`/`VK_F1..F4`/`MF_*`/`FORMAT_MESSAGE_*`/`LMEM_*`/`GENERIC_*`/`CREATE_*`/`FILE_*`/`ERROR_*` 等），本模块现累计 **374 原型 / 46 结构·联合体 / 546 常量**；`winapi.co` 末「已知边界」段同步为消费方登记 compiler 层剩余限制。
- **Windows x64 按值 struct/union ABI 修复**：编译器原先将按值传递的 struct/union 形参与返回一律降为 `ptr`，与 winx64 ABI（≤ 8 字节聚合体整块装进一个通用寄存器）不符——colang 自调自发两侧同降法故内部自洽，仅在调用外部 Win32 API 时错位。本版改为与 clang 同法：尺寸∈{1,2,4,8} 的按值聚合体形参/返回降为等宽整数标量（`i8/i16/i32/i64`），>8 或非 2 幂尺寸维持 `ptr`（Microsoft x64「按参考传」的正确降法）。`winapi.co` 中 `WindowFromPoint(struct POINT)` 行为修正；`SetFilePointerEx(union LARGE_INTEGER)` 同期入库；「已知边界」段相应项翻为已修复。**保留边界**：16 字节纯整型聚合体的「两寄存器」特例、及外部 API 的 >8 字节聚合体仍以指针按参考传。非 Windows x64 目标逐字不变。

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
