@echo off
rem Build the test programs in tools\ (MSVC). Output: build\tools\*.exe
rem ASCII only (cmd parses batch files in the OEM code page).
setlocal
if defined VCINSTALLDIR goto :build
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSPATH="
pushd "%VSINSTALLER%"
for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
popd
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
:build
cd /d "%~dp0.."
if not exist build\tools mkdir build\tools
set "CFLAGS=/nologo /utf-8 /W4 /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
cl %CFLAGS% /Fobuild\tools\ /Febuild\tools\test_zlite.exe tools\test_zlite.c src\zdeflate.c src\zinflate.c || exit /b 1
cl %CFLAGS% /Fobuild\tools\ /Febuild\tools\test_jpeg.exe tools\test_jpeg.c src\jpegenc.c ole32.lib windowscodecs.lib || exit /b 1
cl %CFLAGS% /Fobuild\tools\ /Febuild\tools\test_des.exe tools\test_des.c src\vncdes.c || exit /b 1
echo OK
exit /b 0
:novs
echo [error] Visual Studio Build Tools not found.
exit /b 1
