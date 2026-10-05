@echo off
REM Build the flying double-pendulum cursor (console-less background app).
REM Tries MSVC (cl) first, then MinGW (g++). Produces pendulum_cursor.exe here.
setlocal

set SRC=src\main.cpp src\Physics.cpp src\SnapMode.cpp src\Renderer.cpp src\CursorController.cpp src\ConfigFile.cpp src\CursorPoses.cpp src\AppWindow.cpp src\BeatDsp.cpp src\BeatWorker.cpp src\LoopbackCapture.cpp src\GifDecoder.cpp src\Dancer.cpp

where cl >nul 2>nul
if %errorlevel%==0 (
    echo Building with MSVC...
    rc /nologo /i resources /fo app.res resources\app.rc
    if errorlevel 1 goto :fail
    cl /nologo /EHsc /O2 /std:c++17 /Iinclude %SRC% app.res ^
        /Fe:pendulum_cursor.exe ^
        /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib winmm.lib shell32.lib ole32.lib
    if errorlevel 1 goto :fail
    del *.obj app.res >nul 2>nul
    goto :ok
)

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo Building with MinGW g++...
    windres -I resources resources\app.rc -O coff -o app.res
    if errorlevel 1 goto :fail
    g++ -O2 -std=c++17 -Iinclude %SRC% app.res -o pendulum_cursor.exe ^
        -mwindows -luser32 -lgdi32 -lwinmm -lshell32 -lole32 -static
    if errorlevel 1 goto :fail
    del app.res >nul 2>nul
    goto :ok
)

echo No C++ compiler found. Install Visual Studio Build Tools or MinGW-w64.
exit /b 1

:ok
echo.
echo Built pendulum_cursor.exe
echo Keep the actors folder (.gifbpm files) next to the exe for the beat dancer.
echo On first run it creates pendulum.conf and cursors.conf next to it.
echo Stop it via the tray icon (right-click - Exit) or Ctrl+Alt+P.
exit /b 0

:fail
echo Build failed.
exit /b 1
