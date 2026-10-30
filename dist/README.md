# colang

## 项目简述

colang 是 **Co 语言**的编译器（`Co ≈ C + object`，尽量兼容 C 语法并对齐 C23）。
本发布包提供编译器可执行文件及其自制标准头。语言规范的完整说明见下方文档站；
项目背景与源码构建见源代码仓库。当前为**开发/测试版本（0.4.1）**，特性仍在演进。

## 地址

- 项目主页 / 源码仓库：https://github.com/shanmin/colang
- 语言规范文档：https://shanmin.github.io/colang/ （中 / 英双语）

## 目录结构

```
dist/
├── colang.exe              编译器可执行文件
├── include/                Co 自制标准头（stdio.h、stdint.h、threads.h 等 34 个，
│                            含 Win32 API 模块 winapi.co）
├── LICENSE                 Apache License 2.0 全文
├── NOTICE                  第三方归属说明
└── README.md               本说明
```

> 标准头目录会被自动定位：编译器从 `colang.exe` 所在目录逐级上溯查找 `include/stddef.h`。
> 请保持上述相对布局；也可通过环境变量 `COLANG_STDLIB` 手动指向标准头目录。

## 简单使用说明

在命令行中调用编译器，参数格式为 `colang [选项] <源文件.co>`。

编译并链接为可执行文件（默认动作）：

```bat
colang.exe hello.co -o hello.exe
hello.exe
```

一个最小的示例源文件 `hello.co`：

```c
#include <stdio.h>

int main() {
    printf("hello, colang\n");
    return 0;
}
```

常用选项：

| 选项 | 说明 |
| --- | --- |
| `-h`, `--help` | 显示帮助 |
| `-v`, `--version` | 显示版本与目标平台信息 |
| `-o <file>` | 指定输出文件：`*.exe` 链为可执行文件（默认动作），`*.dll` 链为 Windows 动态库 |
| `-c` | 仅编译，产出 COFF `.obj` 文件（不链接） |
| `-E` | 仅预处理，输出展开后的翻译单元 |
| `--emit-llvm` | 输出 LLVM IR 文本（`.ll`） |
| `--dump-tokens` | 列出源文件中所有词法单元 |
| `--dump-ast` | 解析源文件并打印语法树 |
| `-I <dir>` / `-I<dir>` | 追加头文件搜索目录 |

> Co 层扩展（`import` 模块化、`public` 可见性/导出、`winapi.co` 的 Win32 API 封装等）
> 的完整语法与语义见上方语言规范文档。

## 运行前提

- **链接由 colang 内置的进程内 lld 完成，无需外部 `clang`**：前端产出 COFF `.obj`，
  链接器直接将其链为 `.exe` / `.dll`（依 `-o` 的目标扩展名分岔）。
- **链接需要本机装有 Windows SDK / VC 运行库**：colang 自动定位库搜索路径（`%LIB%` +
  SDK/VC 注册表发现）；若链接失败，可先用 `-c` / `-E` / `--emit-llvm` 等不链接动作定位问题。
- **产物免装 VC++ 运行库**：编译得到的 exe/dll 默认**静态链接 CRT**，目标机无需再安装
  VC++ Redistributable。

## 许可

本发布包采用 **Apache License 2.0** 授权，全文见包内 `LICENSE`，第三方归属说明见 `NOTICE`。
你用 colang 编译得到的程序不受该许可证约束。
