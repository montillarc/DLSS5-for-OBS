@echo off
setlocal

if not exist "%~dp0runtime\nvngx_dlssnr.dll" (
    if exist "%~dp0runtime\nvngx_dlssnr.part1.bin" (
        echo Reassembling runtime\nvngx_dlssnr.dll from parts...
        copy /b "%~dp0runtime\nvngx_dlssnr.part1.bin"+"%~dp0runtime\nvngx_dlssnr.part2.bin" "%~dp0runtime\nvngx_dlssnr.dll" >nul
    )
)

if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else (
    echo [ERROR] Visual Studio 2022 C++ Build Tools not found.
    exit /b 1
)
if errorlevel 1 exit /b 1

if not exist build mkdir build

echo [1/3] Compiling nvngx.dll_ns-forwarder.dll ...
cl /nologo /O2 /MD /EHsc /std:c++17 ^
   /I"third_party\ngx\include" ^
   "third_party\ngx\ngx_forwarder.cpp" ^
   /LD /Febuild\nvngx.dll_ns-forwarder.dll ^
   /Fo"build\ngx_forwarder.obj"
if errorlevel 1 exit /b 1

echo [2/3] Compiling obs-dlss5-filter.dll ...
cl /nologo /O2 /MD /EHsc /std:c++17 ^
   /I"src" ^
   /I"third_party\ngx\include" ^
   /I"third_party\obs\include" ^
   "src\obs_dlss5_filter.cpp" "src\dlss5_engine.cpp" ^
   /LD /Febuild\obs-dlss5-filter.dll ^
   /Fo"build\\" ^
   /link /LIBPATH:"third_party\ngx\lib" /LIBPATH:"third_party\obs\lib" ^
   nvsdk_ngx_d.lib obs.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo [3/3] Compiling test_engine.exe ...
cl /nologo /O2 /MD /EHsc /std:c++17 ^
   /I"src" ^
   /I"third_party\ngx\include" ^
   "src\test_engine.cpp" "src\dlss5_engine.cpp" ^
   /Febuild\test_engine.exe ^
   /Fo"build\\" ^
   /link /LIBPATH:"third_party\ngx\lib" ^
   nvsdk_ngx_d.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo [4/4] Updating runtime\ and build\ binaries ...
copy /Y "build\obs-dlss5-filter.dll"       "runtime\obs-dlss5-filter.dll" >nul
copy /Y "build\nvngx.dll_ns-forwarder.dll" "runtime\nvngx.dll_ns-forwarder.dll" >nul
copy /Y "runtime\nvngx_dlssnr.dll"         "build\nvngx_dlssnr.dll" >nul

echo === BUILD SUCCESSFUL ===
