@echo off
rem ============================================================
rem build.bat - build script for the colang compiler
rem
rem usage:
rem   build.bat            incremental build (configures if needed)
rem   build.bat rebuild    wipe build dir, configure and build
rem   build.bat clean      clean build output
rem
rem toolchain resolution (first match wins, so this works both on
rem the author's machine and on a fresh clone):
rem   CMake    : %%COLANG_CMAKE%%  ->  "cmake" on PATH  ->  local fallback
rem   LLVM_DIR : %%COLANG_LLVM_DIR%%  ->  local fallback  ->  CMake default lookup
rem
rem   Override examples:
rem     set COLANG_CMAKE=C:\path\to\cmake.exe
rem     set COLANG_LLVM_DIR=C:\path\to\llvm\lib\cmake\llvm
rem
rem NOTE: kept ASCII-only on purpose, safe under any Windows code page.
rem ============================================================
setlocal

set "BUILD_DIR=build"

rem ---- locate CMake ----
set "CMAKE="
if defined COLANG_CMAKE if exist "%COLANG_CMAKE%" set "CMAKE=%COLANG_CMAKE%"
if not defined CMAKE (
    where cmake >nul 2>nul
    if not errorlevel 1 set "CMAKE=cmake"
)
if not defined CMAKE if exist "D:\svn\bp.shanmin.com\colang\llvm-23.1.0\cmake-4.4.3-windows-x86_64\bin\cmake.exe" set "CMAKE=D:\svn\bp.shanmin.com\colang\llvm-23.1.0\cmake-4.4.3-windows-x86_64\bin\cmake.exe"
if not defined CMAKE (
    echo ERROR: CMake not found.
    echo        Set COLANG_CMAKE to the cmake.exe path, or add cmake to PATH.
    exit /b 1
)

rem ---- rebuild: wipe build dir ----
if /i "%~1"=="rebuild" (
    if exist "%BUILD_DIR%" (
        echo ---------- removing %BUILD_DIR% ----------
        rmdir /s /q "%BUILD_DIR%"
    )
)

rem ---- clean: only remove build output ----
if /i "%~1"=="clean" (
    echo ---------- clean ----------
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target clean
    exit /b %errorlevel%
)

rem ---- LLVM_DIR: pass -DLLVM_DIR only when resolvable, else let CMake find it ----
set "LLVM_ARG="
if defined COLANG_LLVM_DIR if exist "%COLANG_LLVM_DIR%" set "LLVM_ARG=-DLLVM_DIR="%COLANG_LLVM_DIR%""
if not defined LLVM_ARG if exist "D:\svn\bp.shanmin.com\colang\llvm-23.1.0\build\lib\cmake\llvm" set "LLVM_ARG=-DLLVM_DIR="D:\svn\bp.shanmin.com\colang\llvm-23.1.0\build\lib\cmake\llvm""

rem ---- configure if needed ----
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo ---------- cmake configure ----------
    "%CMAKE%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release %LLVM_ARG%
    if errorlevel 1 (
        echo ERROR: cmake configure failed
        echo        Set COLANG_LLVM_DIR to your LLVM cmake config dir, e.g. LLVM/lib/cmake/llvm
        exit /b 1
    )
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
