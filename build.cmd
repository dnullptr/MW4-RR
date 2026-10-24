@echo off
setlocal
cd /d "%~dp0"
if "%~2"=="" (
 echo Usage: build.cmd "NGX include directory" "DXC bin\x64 directory"
 exit /b 2
)
if not exist "%~1\nvsdk_ngx.h" exit /b 2
if not exist "%~2\dxc.exe" exit /b 2
set "DIAGNOSTIC_FLAGS="
if /i "%~3"=="filter-diagnostic" set "DIAGNOSTIC_FLAGS=/DRR_FILTER_DIAGNOSTIC"
if not "%~3"=="" if /i not "%~3"=="filter-diagnostic" exit /b 2
call tools\vcvars.bat || exit /b 1
if not exist build mkdir build
python -B tools\prepare.py --dxc "%~2" || exit /b 1
cl %DIAGNOSTIC_FLAGS% /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\imgui /Iexternal\streamline /Iexternal\reshade\include /I"%~1" /Iexternal\minhook\include /Iexternal\dxc-1.8.2505.1\inc /Fobuild\ /Fdbuild\ src\mw4_rr.cpp external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c /link /OUT:build\mw4-rr.addon64 /IMPLIB:build\mw4-rr.lib kernel32.lib user32.lib psapi.lib d3d12.lib dxgi.lib d3dcompiler.lib || exit /b 1
copy /y "%~2\dxcompiler.dll" build\mw4-rr-dxcompiler.dll >nul || exit /b 1
echo Built build\mw4-rr.addon64 and its shader-reflection companion.
exit /b 0
