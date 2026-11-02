@echo off
cd /d "H:\Users\Administrator\Downloads\gage new\gage-mp-keep\gage-mp-new\gage-mp"
set PATH=%CD%\.toolchain\winlibs-gcc13.3\mingw64\bin;%PATH%

echo === Configuring (build dir: %TEMP%\x) ===
cmake -S . -B %TEMP%\x -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++.exe -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc.exe -DCMAKE_RC_COMPILER=windres.exe
if errorlevel 1 (
    echo === CMAKE CONFIGURE FAILED ===
    pause
    exit /b 1
)

echo === Building gage_mp.dll ===
cmake --build %TEMP%\x --target gage_mp --parallel
if errorlevel 1 (
    echo === BUILD FAILED ===
    pause
    exit /b 1
)

echo === Building SolarLauncher.exe + full chain ===
cmake --build %TEMP%\x --parallel
if errorlevel 1 (
    echo === FULL BUILD FAILED ===
    pause
    exit /b 1
)

echo.
echo === BUILD SUCCEEDED ===
echo Output: %TEMP%\x\bin\
dir /b %TEMP%\x\bin\*.exe %TEMP%\x\bin\*.dll 2>nul
echo.
pause
