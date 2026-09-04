@echo off
rem ============================================================
rem build.bat — Co 语言编译器一键编译脚本
rem
rem 用法:
rem   build.bat            增量编译（build 目录不存在时自动配置）
rem   build.bat rebuild    删除 build 目录，重新配置并全量编译
rem   build.bat clean      清理编译产物
rem
rem 依赖（路径写死，换机器需修改）:
rem   CMAKE    — CMake 4.4.3（随 LLVM 源码包附带）
rem   LLVM_DIR — 本地 LLVM 23.1.0 构建树（见 PROJECT.md ADR-006）
rem
rem 注意: 本文件需保存为 ANSI(GBK) 编码，中文 Windows cmd 直接可用
rem ============================================================
setlocal

set "CMAKE=D:\svn\bp.shanmin.com\colang\llvm-23.1.0\cmake-4.4.3-windows-x86_64\bin\cmake.exe"
set "LLVM_DIR=D:\svn\bp.shanmin.com\colang\llvm-23.1.0\build\lib\cmake\llvm"
set "BUILD_DIR=build"

if not exist "%CMAKE%" (
    echo ERROR: cmake not found: %CMAKE%
    exit /b 1
)

rem rebuild: 删除构建目录
if /i "%~1"=="rebuild" (
    if exist "%BUILD_DIR%" (
        echo ---------- 删除构建目录 %BUILD_DIR% ----------
        rmdir /s /q "%BUILD_DIR%"
    )
)

rem 首次（或 rebuild 后）需要 CMake 配置
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo ---------- CMake 配置 ----------
    "%CMAKE%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release "-DLLVM_DIR=%LLVM_DIR%"
    if errorlevel 1 (
        echo ERROR: CMake 配置失败
        exit /b 1
    )
)

rem clean: 只清理编译产物
if /i "%~1"=="clean" (
    echo ---------- 清理编译产物 ----------
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target clean
    exit /b %errorlevel%
)

echo ---------- 编译 colang ----------
"%CMAKE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo ERROR: 编译失败
    exit /b 1
)

echo.
echo 编译完成: %CD%\%BUILD_DIR%\Release\colang.exe
endlocal
exit /b 0
