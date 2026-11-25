@echo off
setlocal

set "VSCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%VSCMD%" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
        set "VSCMD=%%i\Common7\Tools\VsDevCmd.bat"
    )
)

call "%VSCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo Failed to initialize Visual Studio Developer Environment.
    exit /b 1
)

if not exist build mkdir build

echo Compiling PortTool.exe (Size-Optimized Standalone x64)...
cl /nologo /std:c++20 /EHsc /W4 /O1 /Os /GL /Gy /GF /MT /DNDEBUG ^
   /DUNICODE /D_UNICODE ^
   /Fo:build\ ^
   src\main.cpp src\port_monitor.cpp src\system_monitor.cpp src\app_ui.cpp ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
   /OUT:build\PortTool.exe ^
   d2d1.lib dwrite.lib dwmapi.lib setupapi.lib iphlpapi.lib powrprof.lib pdh.lib dxgi.lib psapi.lib ole32.lib user32.lib gdi32.lib advapi32.lib

if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

for %%F in (build\PortTool.exe) do echo Build succeeded: %%F (%%~zF bytes)
endlocal

