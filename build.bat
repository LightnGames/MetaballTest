@echo off
setlocal
rem Locate latest Visual Studio via vswhere and set up the x64 dev environment
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio not found.
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul

cl /nologo /std:c++17 /EHsc /W3 /O2 /utf-8 /DUNICODE /D_UNICODE main.cpp ^
   /link /SUBSYSTEM:WINDOWS /OUT:Metaball2D.exe
exit /b %errorlevel%
