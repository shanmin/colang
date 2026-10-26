@echo off
rem ============================================================
rem build.bat - build script for the colang compiler
rem
rem usage:
rem   build.bat            incremental build
rem   build.bat rebuild    wipe build dir, configure and build
rem   build.bat clean      clean build output
rem
rem NOTE: this file is kept ASCII-only on purpose, so it is safe
rem       under any Windows code page (ANSI / GBK / UTF-8).
rem ============================================================
setlocal

set "CMAKE=D:\svn\bp.shanmin.com\colang\llvm-23.1.0\cmake-4.4.3-windows-x86_64\bin\cmake.exe"
set "LLVM_DIR=D:\svn\bp.shanmin.com\colang\llvm-23.1.0\build\lib\cmake\llvm"
set "BUILD_DIR=build"

if not exist "%CMAKE%" (
    echo ERROR: cmake not found: %CMAKE%
    exit /b 1
)

if /i "%~1"=="rebuild" (
    if exist "%BUILD_DIR%" (
        echo ---------- removing %BUILD_DIR% ----------
        rmdir /s /q "%BUILD_DIR%"
    )
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo ---------- cmake configure ----------
    "%CMAKE%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release "-DLLVM_DIR=%LLVM_DIR%"
    if errorlevel 1 (
        echo ERROR: cmake configure failed
        exit /b 1
    )
)

if /i "%~1"=="clean" (
    echo ---------- clean ----------
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target clean
    exit /b %errorlevel%
)

echo ---------- building colang ----------
"%CMAKE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo ERROR: build failed
    exit /b 1
)

echo.
rem VS generator is multi-config -> build\Release\colang.exe; single-config -> build\colang.exe
if exist "%BUILD_DIR%\Release\colang.exe" (
    echo output: %BUILD_DIR%\Release\colang.exe
) else (
    echo output: %BUILD_DIR%\colang.exe
)
endlocal
exit /b 0
