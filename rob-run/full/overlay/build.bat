@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if not errorlevel 1 goto compile
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ROB_VS_ROOT=%%i"
if not defined ROB_VS_ROOT exit /b 2
call "%ROB_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 2
:compile
cl /nologo /LD /MT /std:c++20 /O2 /EHsc /DUNICODE /D_UNICODE /I imgui /I menu /I features ^
 imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
 imgui\imgui_impl_dx11.cpp imgui\imgui_impl_win32.cpp ^
 menu\ui.cpp menu\menu_compat.cpp features\il2cpp.cpp features\esp.cpp features\silent_bridge.cpp hook\present.cpp dllmain.cpp ^
 /Fe:RobRunInternal.dll /link d3d11.lib dxgi.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
