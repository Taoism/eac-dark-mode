@echo off
rem Builds the x86 msacm32.dll proxy (EAC dark mode) into build\
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE ^
   src\eacdark.cpp src\icons.cpp src\proxy.cpp ^
   /Fobuild\ /Febuild\msacm32.dll ^
   /link /DEF:src\msacm32.def /SAFESEH:NO user32.lib gdi32.lib gdiplus.lib || exit /b 1
echo BUILD OK
