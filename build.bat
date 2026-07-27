@echo off
rem ---------------------------------------------------------------------------
rem  Reference build for `sec`, the sim-engine compiler driver.
rem
rem  A full, from-scratch compile into a single translation unit: the build model
rem  SPECIFICATION.md section 12.4 prescribes for generated models, applied to
rem  the compiler itself. No object files, so "incremental" is not a feature
rem  that was skipped but a category that does not exist.
rem
rem  Run from a Developer Command Prompt, or anywhere `cl` is on PATH.
rem      build.bat            release
rem      build.bat debug      /Od /Zi
rem
rem  ASCII + CRLF only: cmd mis-splits an LF-only batch file.
rem ---------------------------------------------------------------------------
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo build: `cl` not found. Open a Developer Command Prompt for VS, or run
    echo        vcvarsall.bat x64 first.
    exit /b 2
)

if not exist build mkdir build

set FLAGS=/nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /D_CRT_SECURE_NO_WARNINGS
if /I "%~1"=="debug" (
    set FLAGS=%FLAGS% /Od /Zi /MDd /Fdbuild\sec.pdb
) else (
    set FLAGS=%FLAGS% /O2 /MD /DNDEBUG
)

echo cl %FLAGS%
cl %FLAGS% /Fobuild\ /Fe:build\sec.exe src\unity.cpp
if errorlevel 1 (
    echo build: FAILED
    exit /b 1
)

echo build: build\sec.exe
endlocal
