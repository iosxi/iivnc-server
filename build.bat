@echo off
rem Build iivnc-server.exe with MSVC (VS 2022 Build Tools). /MT links the CRT
rem statically, so the exe needs no runtime install.
rem
rem   build.bat          release build -> iivnc-server.exe
rem   build.bat debug    debug build   -> build\iivnc-server-debug.exe
rem
rem ASCII only and CRLF on purpose: cmd parses batch files in the OEM code
rem page (932 on Japanese Windows), and goto/labels can misbehave with LF.
rem The vcvars path contains "(x86)", so it is never expanded inside a
rem parenthesized block (see reg-rename\build.bat).
setlocal
if defined VCINSTALLDIR goto :build
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSINSTALLER%\vswhere.exe" goto :novs
set "VSPATH="
pushd "%VSINSTALLER%"
for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
popd
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto :novs

:build
cd /d "%~dp0"
if not exist build\obj mkdir build\obj
rc /nologo /fo build\obj\iivnc-server.res src\iivnc-server.rc || exit /b 1

set "SRC=src\main.c src\config.c src\capture.c src\pool.c src\encode.c src\server.c src\input.c src\clip.c src\ui.c src\theme.c src\zdeflate.c src\zinflate.c src\jpegenc.c src\vncdes.c src\svc.c"
set "LIBS=user32.lib gdi32.lib shell32.lib comctl32.lib dwmapi.lib uxtheme.lib ole32.lib ws2_32.lib iphlpapi.lib bcrypt.lib d3d11.lib dxgi.lib synchronization.lib advapi32.lib dxguid.lib wtsapi32.lib userenv.lib oleaut32.lib"
set "CFLAGS=/nologo /utf-8 /W4 /wd4201 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00"

if /i "%~1"=="debug" goto :debug
cl %CFLAGS% /O2 /Fobuild\obj\ /Feiivnc-server.exe %SRC% build\obj\iivnc-server.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || exit /b 1
for %%F in (iivnc-server.exe) do echo built iivnc-server.exe (%%~zF bytes)
exit /b 0

:debug
cl %CFLAGS% /Od /Zi /Fobuild\obj\ /Fdbuild\ /Febuild\iivnc-server-debug.exe %SRC% build\obj\iivnc-server.res /link /DEBUG /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || exit /b 1
echo built build\iivnc-server-debug.exe
exit /b 0

:novs
echo [error] Visual Studio Build Tools (vcvars64.bat) not found.
exit /b 1
