# colang — Co 语言编译器

colang 是 **Co 语言**的编译器。Co 语言基于 C 语言扩展而来，尽量兼容既有 C 语法规则，
并在此基础上加入部分面向对象特性（可粗略理解为 `Co ≈ C + object`，或简化版 C++）。
当前所有语法均以 C 语言为基准（推荐对齐 C23），本文档中未涉及的内容以现有 C 标准为准。

> 项目处于**早期开发阶段**，基本特性仍在演进，最终语言特性以官方文档为准。

## 文档

语言规范与使用指南（中 / 英双语）：<https://shanmin.github.io/colang/>

## 下载预编译版本

打包好的编译器可执行文件（含 Co 自制标准头）见
[Releases](https://github.com/shanmin/colang/releases)。解压后包内附有 `README.md`
讲解用法，快速上手：

```bat
bin\colang.exe hello.co -o hello.exe
```

## 从源码构建

依赖一个**完整的 LLVM 开发库**（C++ 头文件 + 静态库 + CMake 配置文件）；官方
clang 安装包不含开发库，不能用于编译本项目。本项目以 LLVM 23.1 验证。

要求：CMake ≥ 3.20、支持 C++20 的编译器（MSVC 需 `/MD` 与 LLVM 静态库 CRT 一致）。

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="<LLVM 根目录>/lib/cmake/llvm"
cmake --build build --config Release
```

产物：`build\Release\colang.exe`（多配置生成器）/ `build\colang.exe`（单配置）。

运行测试（需 PATH 上有 `clang` 用于链接）：

```bat
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
```

## 仓库结构

- `src/` — 编译器源码
- `include/colang/` — 编译器内部头文件
- `stdlib/include/` — Co 自制标准头（stdio.h、stdint.h 等）
- `test/` — 独立冒烟测试用例（`*.co`）与运行器 `run_tests.ps1`
- `docs/` — 中 / 英双语 mdBook 文档站，经 CI 自动发布到 GitHub Pages

## 许可

本项目采用 **Apache License 2.0** 授权，全文见 [`LICENSE`](LICENSE)，第三方归属说明见 [`NOTICE`](NOTICE)。

> 与本编译器许可证无关：你用 colang 编译得到的程序不受本许可证约束。
