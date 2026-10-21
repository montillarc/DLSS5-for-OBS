@echo off
setlocal

if not exist "%~dp0runtime\nvngx_dlssnr.dll" (
    if exist "%~dp0runtime\nvngx_dlssnr.part1.bin" (
        echo [1/2] Reassembling nvngx_dlssnr.dll from parts...
        copy /b "%~dp0runtime\nvngx_dlssnr.part1.bin"+"%~dp0runtime\nvngx_dlssnr.part2.bin" "%~dp0runtime\nvngx_dlssnr.dll" >nul
    )
)

if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "%~dp0build.cmd"
    if errorlevel 1 exit /b 1
)

set "DEST_BIN=C:\ProgramData\obs-studio\plugins\obs-dlss5-filter\bin\64bit"
set "DEST_DATA=C:\ProgramData\obs-studio\plugins\obs-dlss5-filter\data"

if not exist "%DEST_BIN%" mkdir "%DEST_BIN%"
if not exist "%DEST_DATA%" mkdir "%DEST_DATA%"

echo Installing plugin files to "%DEST_BIN%" ...
del /F /Q "%DEST_BIN%\*.old" 2>nul
if exist "%DEST_BIN%\obs-dlss5-filter.dll" move /Y "%DEST_BIN%\obs-dlss5-filter.dll" "%DEST_BIN%\obs-dlss5-filter.dll.old" >nul 2>nul
copy /Y "%~dp0runtime\obs-dlss5-filter.dll"         "%DEST_BIN%\obs-dlss5-filter.dll" >nul
if errorlevel 1 (
    echo [ERROR] Could not copy obs-dlss5-filter.dll.
    exit /b 1
)
if exist "%DEST_BIN%\nvngx.dll_ns-forwarder.dll" move /Y "%DEST_BIN%\nvngx.dll_ns-forwarder.dll" "%DEST_BIN%\nvngx.dll_ns-forwarder.dll.old" >nul 2>nul
copy /Y "%~dp0runtime\nvngx.dll_ns-forwarder.dll"   "%DEST_BIN%\nvngx.dll_ns-forwarder.dll" >nul
if exist "%DEST_BIN%\nvngx_dlssnr.dll" move /Y "%DEST_BIN%\nvngx_dlssnr.dll" "%DEST_BIN%\nvngx_dlssnr.dll.old" >nul 2>nul
copy /Y "%~dp0runtime\nvngx_dlssnr.dll"             "%DEST_BIN%\nvngx_dlssnr.dll" >nul
del /F /Q "%DEST_BIN%\*.old" 2>nul

echo === INSTALLATION COMPLETE ===
echo Installed to: %DEST_BIN%
