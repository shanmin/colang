# 编译器命令行

## 调用格式

```
colang [选项] <源文件.co>
```

一次调用处理**一个**源文件。不指定任何动作选项时，编译并链接为可执行文件。

## 选项

| 选项 | 说明 |
| --- | --- |
| `-h`, `--help` | 显示用法与选项列表 |
| `-v`, `--version` | 显示编译器版本（四段）、所用 LLVM 版本与目标平台 |
| `-E` | 仅做预处理，把展开后的 token 清单打到标准输出 |
| `-c` | 仅编译，产出目标文件（Windows 目标为 COFF `.obj`、Linux 目标为 ELF `.o`），不链接 |
| `-o <file>` | 指定输出文件 |
| `-I <dir>` | 追加头文件搜索目录，也可写作 `-I<dir>`；可重复给出 |
| `--target <triple>` | 指定目标三元组（如 `x86_64-unknown-linux-gnu`）；省略时默认取宿主平台 |
| `--resource <file>` | 向可执行文件嵌入 PE 资源，可重复给出；接受 `.rc`（内置资源编译器编译）或预编译的 `.res`（原样嵌入）；仅 Windows 目标 |

## 产物与默认名

| 动作 | 产物 | 未写 `-o` 时的默认名 |
| --- | --- | --- |
| 默认（编译 + 链接） | 可执行文件 | Windows：`<源文件名去扩展>.exe`；其他平台：`<源文件名去扩展>` |
| `-c` | `.obj` 目标文件 | `<源文件名去扩展>.obj`，留在当前目录 |
| `-E` | 标准输出 | 固定走标准输出，`-o` 对其无效 |

链接方式调用时，中间的 `.obj` 写在系统临时目录，链接完成后自动删除。

## 版本号的四段形式

`-v` 输出形如：

```
colang 0.4.0.29
LLVM   23.1.0
target x86_64-pc-windows-msvc
model  LLP64 (windows) (long=4, void*=8, wchar_t=2)
```

前三段 `MAJOR.MINOR.PATCH` 是**发布版本**；第四段为**构建计数**，在每次真实重编编译器时自增（改三段版本时自动归零），只用于区分开发期的先后构建，不代表功能变更。产物 exe/dll 的 PE 版本信息（`FILEVERSION`/`PRODUCTVERSION`）同步使用这四段。

## 默认设置头

每次编译都会先于主源自动展开标准库目录里的 `include/colang.h`（找不到则静默跳过），供放置自己的全局宏定义。该注入**只作用于主源**，不作用于被 `import` 的模块；纯 C 档与 `.co` 档同样生效。

## `-E` 的输出形态

`-E` 打印的是**逐行 token 清单**，形如：

```
hello.co:3:1  keyword     int
hello.co:5:12  string      "hello\n"
```

它用于查看宏展开、条件裁剪与文件包含的结果，**不是**可以直接再交给编译器处理的 C 源码。此外 `-E` 到预处理为止即结束，不做语法检查：即使源文件存在语法错误，`-E` 仍会输出 token 并返回 0。

## 头文件搜索

按优先级从高到低：

1. 命令行 `-I` 给出的目录；
2. Co 自制标准头目录（`stdio.h`、`stdint.h`、`winapi.co` 等），由编译器自动定位：先读环境变量 `COLANG_STDLIB`（直接指向标准头目录）；未设置时，从 `colang` 可执行文件所在目录逐级上溯（至多 8 层），以其中存在 `include/stddef.h` 为标记定位到该 `include/` 目录。

发布包为扁平单层布局——`colang.exe` 与标准头目录 `include/` 同级放置，解压后可直接使用，`#include <stdio.h>` 与 `import "winapi.co"` 都无需给 `-I`；把编译器挪到别处时，请保持 `colang.exe` 与 `include/` 的同级相对位置，或改用 `COLANG_STDLIB` 指定。标准头定位不到时，`#include <stdio.h>` 会报 `#include file not found`。

## 目标平台与数据模型

colang 用单一目标（target）驱动整条流水线：预处理宏、类型宽度、代码生成与链接都取自同一个目标三元组。不写 `--target` 时默认为**宿主平台**（在 Windows 上即 Windows/LLP64，在 Linux 上即 Linux/LP64）。

| 目标 | 数据模型 | `long` | `void*` | `wchar_t` | 关键预定义宏 |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | LLP64 | 4 | 8 | 2 | `_WIN32` `_WIN64` `_MSC_VER` `__LLP64__` |
| Linux x86_64 | LP64 | 8 | 8 | 4 | `__linux__` `__gnu_linux__` `__ELF__` `__x86_64__` `__LP64__` |

数据模型宏（`__LLP64__`/`__LP64__`）反映编译器**真实的类型宽度**，与 `sizeof(long)` 严格一致；`size_t`/`ptrdiff_t`、`wchar_t` 等标准头 typedef 也据此分派（Linux 上 `size_t` 为 `unsigned long`、`wchar_t` 为 4 字节，与 glibc 一致）。`long` 的宽度、以及它参与的所有**整型提升与常用算术转换**，同样全链随目标贯穿（预处理、类型推导、语义分析、代码生成一致取同一宽度），因此 LP64 上含 `long` 的表达式按 64 位求值、LLP64 上按 32 位求值。

> 跨目标编译（如在 Windows 上 `--target x86_64-unknown-linux-gnu`）可产出 ELF 目标文件与 IR；但链接步骤按**当前构建所在平台**的链接后端进行，跨平台产物链接不属当前支持范围。

除上述与目标相关的宏外，colang 还**恒定定义**一个与目标无关的编译器标识宏 `__COLANG__`（值为 `1`），在任何平台、任何 `--target` 下都存在，供源码判别「本次是否由 colang 编译」（与 `__clang__`、`__GNUC__` 同类）。

## 链接器：编译器自带

colang 自带链接器，把源文件编译并链接为可执行文件这一步**无需外部 `clang`，也无需任何链接器命令**。

- **Windows**：链接需要本机已安装 Windows SDK 与 VC 运行库（提供 `libcmt` 等平台库）。colang 会自动定位它们，普通命令行即可使用，**不必**进入开发者命令提示符。默认产物**静态链接 C 运行库**，因此在你机器上编译出的 exe，可直接分发到未安装 VC 运行库的 Windows 机器上运行。
- **Linux**：进程内链接器（`lld`）以**静态**方式链入 glibc 与 libgcc，产物为原生 ELF 可执行文件。colang 会自动定位 `crt1.o`/`crti.o`/`crtn.o`、`libc.a` 与 `libgcc`；需要安装静态库包（Debian/Ubuntu 的 `libc6-dev`）。可用 `COLANG_CRT_DIR`、`COLANG_LINUX_SYSROOT`、`COLANG_GCC_LIB_DIR` 环境变量覆盖搜索位置。当前支持静态链接的可执行文件，动态 PIE / `.so` 尚在开发中。

> **兼容性告知**：需要分发的是**编译产物（exe）**，而不是 colang 本身。分发产物时无需附带任何 LLVM/clang 工具。

## PE 资源嵌入

`--resource` 把图标、版本信息、manifest 等 Windows 资源嵌入产出的 exe，可重复给出；例如 `colang --resource app.rc app.co`（`.rc` 通过 `--resource` 给出，不作为源文件位置参数）。每项接受一个文件：

- **`.res`**：已预编译的资源文件，原样嵌入。
- **`.rc`**：资源脚本，由 colang **内置的资源编译器**编译并嵌入，**无需外部 `rc.exe`**。

`.rc` 脚本可用 `#include "resource.h"` 引入自定义 ID 宏（系统头如 `<windows.h>` 无需提供，会被跳过），当前支持以下资源：

- 图标 `ICON`——自动拆分并生成组，`.ico` 文件只需给出一次；
- 位图 `BITMAP`；
- 任意二进制数据 `RCDATA`；
- 应用 manifest `MANIFEST`；
- 版本信息 `VERSIONINFO`——固定属性（`FILEVERSION`、`PRODUCTVERSION`、`FILEFLAGS`、`FILETYPE` 等），以及 `STRINGFILEINFO` 下的 `VALUE` 文本、`VARFILEINFO` 下的 `TRANSLATION`；
- 字符串表 `STRINGTABLE`（及其条目 `STRING`）——按 rc.exe 规律编码为 `RT_STRING`；同一语言可跨多个 `STRINGTABLE` 块声明，会合并到同一分组；
- 加速键 `ACCELERATORS`——按 rc.exe 每条目 8 字节布局编码为 `RT_ACCELERATOR`，并复现其「块末条 `fVirt` 置 0x80」的结束标记（以逐字节对齐）；键支持带引号单字符与 `VK_*` 虚拟键名（后者须伴随 `VIRTKEY`），选项 `VIRTKEY`/`NOINVERT`/`SHIFT`/`CONTROL`/`ALT` 以位标累积，命令须为宏展开后的数值 id；
- 扩展对话框 `DIALOGEX`——按 rc.exe 的 `DLGTEMPLATEEX`/`DLGITEMTEMPLATEEX` 布局编码为 `RT_DIALOG`，与 rc.exe **逐字节一致**；支持对话框头的 `x,y,cx,cy[,helpID]` 与可选子句 `STYLE`/`EXSTYLE`（`|` 分隔的样式表达式，可含 `WS_*`/`DS_*`/`WS_EX_*` 等常用样式宏或数值）、`CAPTION`、`MENU`、`CLASS`、`FONT pt,"face"[,weight[,italic[,charset]]]`（自动并入 `DS_SETFONT`）；控件支持 `PUSHBUTTON`/`DEFPUSHBUTTON`/`CHECKBOX`/`RADIOBUTTON`/`AUTOCHECKBOX`/`AUTORADIOBUTTON`/`GROUPBOX`/`LTEXT`/`RTEXT`/`CTEXT`/`ICON`/`EDITTEXT`/`LISTBOX`/`COMBOBOX`/`SCROLLBAR`（各自映射预定义类原子与 rc.exe 默认样式）及通用的 `CONTROL`；控件尾随可选的样式/扩展样式/helpID 覆盖值；
- 自定义类型与资源名：可写成数字 ID、带引号的字符串，或裸标识符（裸标识符按 rc.exe 惯例转为大写、去引号后作为字符串名/类型），以及逐资源的 `LANGUAGE` 语句。

`MOVEABLE`、`DISKFILEONLY` 等修饰符会被接受并忽略（不影响结果）；`LANGUAGE` 语句生效。

`.rc` 中的字符串字面量一律**遵循 rc.exe 规则**：要在串内嵌入双引号，请写成 `""`（双写），而**非** `\"`（rc.exe 不把反斜杠当作引号的转义符）。转义识别 `\t`、`\n`、`\r`、`\a`（0x08）、`\\`、八进制 `\0`–`\7`（至多 3 位）、十六进制 `\x`（至多 2 位）；其余 `\X` 按字面保留反斜杠。

内置资源编译器产出的 `VERSIONINFO`（`VS_VERSION_INFO`）与 rc.exe **逐字节一致**；固定属性 `FILEOS` 若给出则采用其值、省略时默认 0（与 rc.exe 一致）。

**当前边界**：

- 资源嵌入只作用于**链接产物**；用 `-c` 单独编译目标文件时忽略 `--resource` 并给出提示。
- 仅支持 **Windows 目标**；交叉编译到非 Windows 目标时报错。
- **对话框经典 `DIALOG`（非 EX 形态）/ 菜单 `MENU`** 等脚本型资源尚不支持，遇到会给出明确诊断；`DIALOGEX` 样式/控件走内置表，表未列的自定义控件类、罕见样式位与控件创建数据（creation data）超长形态暂不处理。

## 多文件程序

一次调用只编译一个源文件。colang 通过 `import` 机制组合多个 `.co` 源文件（详见「import（模块导入）」章节）。

`-c` 产出的单个 `.obj` 目标文件可用于查看编译结果。把多个目标文件直接链接为可执行文件的能力尚在开发中；当前组织多文件程序请经由 `import`。

## 诊断与退出码

诊断统一按 `文件:行:列: error: 说明` 的形式输出到标准错误（提示类为 `warning`）。退出码只有两种：成功 `0`，失败 `1`。

## 源文件

Co 源文件的默认扩展名为 `.co`（基于 web 页面时为 `.cop`）。文件若带 UTF-8 BOM，读取时会自动剥除。

## 当前边界

- `-h` 还会列出若干仅供编译器自身调试用的选项（token、语法树、IR 转储等）。这些选项的行为与格式不保证跨版本稳定，本文档不对它们作约定。
