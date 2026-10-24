<#
run_tests.ps1 - standalone test runner for the colang compiler (public mirror)

usage (from the repo root):
  powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
  powershell -ExecutionPolicy Bypass -File test\run_tests.ps1 -CC <path\to\colang.exe>

verdict: each *.co under test\ is compiled and run; main returning 0 is a PASS,
         any non-zero exit code is a FAIL.
prereq : build the compiler first (run build.bat at repo root); linking is done
         by colang's built-in in-process lld linker -- no external clang.exe is
         required (the frontend emits COFF .obj and links in a single step).
exit   : 0 = all passed, 1 = some failed, 2 = environment missing.

NOTE: kept ASCII-only on purpose, safe under any Windows code page / PowerShell.
#>
param(
    [string]$CC
)

$ErrorActionPreference = "Continue"
$root = $PSScriptRoot

# locate colang.exe: explicit -CC > local build output > PATH
if (-not $CC) {
    $cand = Join-Path $root "..\build\Release\colang.exe"
    if (Test-Path $cand) {
        $CC = (Resolve-Path $cand).Path
    } else {
        $w = Get-Command colang -ErrorAction SilentlyContinue
        if ($w) { $CC = $w.Source }
        else {
            Write-Host "ERROR: colang.exe not found. Run build.bat first, or pass -CC <path>."
            exit 2
        }
    }
}
Write-Host "colang : $CC"
Write-Host ""

# temp output dir
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("colang_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$pass = 0; $fail = 0; $failed = @()
Get-ChildItem -Path $root -Filter *.co -File | Sort-Object Name | ForEach-Object {
    $name = $_.BaseName
    $src  = $_.FullName
    $exe  = Join-Path $tmp ($name + ".exe")
    Write-Host ("--- {0} ---" -f $name)

    & $CC $src -o $exe
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failed += "$name (COMPILE_FAIL)"; Write-Host "  [FAIL] compile"; return
    }

    & $exe
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $fail++; $failed += "$name (exit=$code)"; Write-Host "  [FAIL] run exit=$code"; return
    }

    $pass++; Write-Host "  [PASS]"
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("=== pass={0} fail={1} ===" -f $pass, $fail)
if ($fail -gt 0) {
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
}
exit 0
