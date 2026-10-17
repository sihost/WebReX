@echo off
rem Builds and runs the HttpRangeDevice test under node (wasm64).
rem Needs the wasm64 SDK static libs in out\wasm64 (cmake --preset wasm64-release, then
rem cmake --build <dir> --target rexcore rexfilesystem) and the generated include dir of that
rem build (default C:\rex\b\w64c\include; override with REX_WASM_BUILD).
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
if "%REX_WASM_BUILD%"=="" set "REX_WASM_BUILD=C:\rex\b\w64c"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
set "OUT=%TEMP%\rex_web_tests"
if not exist "%OUT%" mkdir "%OUT%"
set "LIBS=%SDK%\out\wasm64"

em++ -std=c++23 -O2 -m64 -pthread -fwasm-exceptions -fno-char8_t -Wno-unsupported-floating-point-opt ^
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -I"%SDK%\include" -I"%REX_WASM_BUILD%\include" ^
  -I"%SDK%\thirdparty\fmt\include" -I"%SDK%\thirdparty\spdlog\include" -I"%SDK%\thirdparty\simde" ^
  -I"%SDK%\thirdparty\tomlplusplus\include" ^
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sPTHREAD_POOL_SIZE=8 -sENVIRONMENT=node -sINITIAL_MEMORY=1GB ^
  "%SDK%\tests\web\http_range_device_test.cpp" "%LIBS%\librexfilesystem.a" "%LIBS%\librexcore.a" ^
  "%LIBS%\libspdlog.a" "%LIBS%\libfmt.a" -o "%OUT%\http_range_device_test.js" || exit /b 1

rem Every node Worker copies the environment when it is spawned, so set the data root here.
set "REX_WEB_TEST_DATA_ROOT=%OUT%\http_range_data"
set "NODE=%EMSDK_NODE%"
if "%NODE%"=="" set "NODE=node"
"%NODE%" "%OUT%\http_range_device_test.js"
exit /b %ERRORLEVEL%
