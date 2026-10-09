@echo off
setlocal
rem ===================================================================
rem  One-click build: Robot.exe (console) + RobotGUI.exe (Win32 GUI)
rem  Toolchain: MSYS2 MinGW-w64 g++ (static link, no external DLL deps)
rem  Usage:  build.bat          build both
rem          build.bat clean    remove intermediates then build
rem ===================================================================
rem --- Toolchain: MSYS2 MinGW-w64 -------------------------------------
rem 本机 MSYS2 安装根目录。换机器或换安装位置时，只需要改这一行。
set "MSYS2_ROOT=D:\MSYS2"
set "CXX=%MSYS2_ROOT%\mingw64\bin\g++.exe"
rem 必须把 mingw64\bin 加进 PATH，否则编译会失败且不给任何报错：
rem   g++.exe 在 bin 目录里，能靠自身目录找到依赖 DLL；
rem   但它调用的 cc1plus.exe 在 lib\gcc\x86_64-w64-mingw32\<ver>\ 下，
rem   要在 PATH 上才能找到 bin 里的 libgmp-10.dll / libisl-23.dll 等。
rem   缺 DLL 时 cc1plus 以 0xC0000135 静默退出，表面现象就是
rem   “编译无输出、退出码 1、不生成 .o”，很容易误判成源码问题。
set "PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%"
rem -------------------------------------------------------------------

if not exist "%CXX%" (
  echo [ERROR] compiler not found: %CXX%
  echo         edit MSYS2_ROOT at the top of this script to your local MSYS2 root.
  exit /b 1
)

cd /d "%~dp0"

set "COMMON=robot_common.cpp serial_port.cpp motion.cpp hanzi.cpp polyline.cpp gui_service.cpp gui_trail.cpp ble_motor.cpp"
set "STD=-std=c++17 -g -Wall -Inlohmann -static -static-libgcc -static-libstdc++"
rem ble_motor.cpp 走 WinRT（蓝牙 GATT 透传），必须链这几个：
rem   runtimeobject = RoInitialize / RoGetActivationFactory
rem   windowsapp    = WindowsCreateString 等 HSTRING 辅助
rem   ole32 / uuid  = COM 与 GUID
set "BLELIBS=-lruntimeobject -lwindowsapp -lole32 -luuid"

if /i "%~1"=="clean" (
  echo [clean] removing *.o
  if exist *.o del /q *.o
)

echo [1/2] building Robot.exe (console) ...
"%CXX%" %STD% %COMMON% main.cpp -o Robot.exe %BLELIBS%
if errorlevel 1 (
  echo [FAILED] Robot.exe
  exit /b 1
)

echo [2/2] building RobotGUI.exe (Win32 GUI) ...
"%CXX%" %STD% -municode %COMMON% gui_win32.cpp -o RobotGUI.exe -lcomctl32 -lgdi32 -luser32 -lsetupapi %BLELIBS%
if errorlevel 1 (
  echo [FAILED] RobotGUI.exe
  exit /b 1
)

echo.
echo [DONE] Robot.exe and RobotGUI.exe generated.
echo        Robot.exe --dryrun     console debug run
echo        RobotGUI.exe --dryrun  GUI debug run
endlocal
