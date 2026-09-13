@echo off
setlocal
rem Build with MSVC (VS 2022 Build Tools). /MT links the CRT statically: no runtime install needed.
rem Usage: build.bat [test]

rem The vcvars path contains "(x86)", so do not call it inside a parenthesized block.
if defined VCINSTALLDIR goto :vcready
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:vcready
cd /d "%~dp0"
if not exist build mkdir build

set CFLAGS=/nologo /utf-8 /W4 /O1 /MT /GS- /DUNICODE /D_UNICODE
set LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib shcore.lib

cl %CFLAGS% /Fobuild\ /Febuild\cursoring.exe src\main.c src\monitors.c src\mapping.c src\editor.c /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest %LIBS% || exit /b 1

if not "%1"=="test" goto :done
cl %CFLAGS% /Fobuild\ /Febuild\test_mapping.exe test\test_mapping.c src\mapping.c /link user32.lib || exit /b 1
build\test_mapping.exe || exit /b 1

:done
echo OK: build\cursoring.exe
