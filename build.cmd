@echo off
setlocal
rem Run in an x64 Native Tools Command Prompt for Visual Studio with WDK installed.
if "%~1"=="" (
  echo Usage: build.cmd SDK_VERSION, for example build.cmd 10.0.22621.0
  exit /b 2
)
if not defined WindowsSdkDir (
  echo Run from an x64 Native Tools Command Prompt with Windows SDK and WDK installed.
  exit /b 2
)
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /c /W4 /WX /kernel /GS- /Zl /D_AMD64_ /D_WIN64 /D_WIN32_WINNT=0x0A00 /I"%WindowsSdkDir%Include\%~1\km" /I"%WindowsSdkDir%Include\%~1\shared" /Fobuild\dumper.obj driver\dumper.c
if errorlevel 1 goto fail
link /nologo /driver /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 /out:build\dumper.sys build\dumper.obj /libpath:"%WindowsSdkDir%Lib\%~1\km\x64" ntoskrnl.lib hal.lib
if errorlevel 1 goto fail
cl /nologo /W4 /WX /TC /Febuild\dumper.exe /Fobuild\client.obj client\main.c
if errorlevel 1 goto fail
popd
exit /b 0
:fail
popd
exit /b 1
