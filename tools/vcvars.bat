@echo off
rem Locate a C++ toolset, allowing the caller to provide VCVARSALL.
if defined VCVARSALL goto :have
set "_rr_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_rr_vswhere%" goto :missing
set "_rr_vsout=%TEMP%\mw4-rr-vswhere-%RANDOM%.txt"
"%_rr_vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%_rr_vsout%"
for /f "usebackq tokens=* delims=" %%i in ("%_rr_vsout%") do set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
del "%_rr_vsout%" >nul 2>&1
:have
if not exist "%VCVARSALL%" goto :missing
call "%VCVARSALL%" x64 >nul
exit /b %errorlevel%
:missing
echo Install the Visual Studio C++ workload or set VCVARSALL to vcvarsall.bat. 1>&2
exit /b 1
