@echo off
rem Builds and runs the HostPathDevice (web cache:/xstorage:) test under node (wasm64, WasmFS).
rem Needs the wasm64 SDK static libs in out\wasm64 (cmake --build C:\rex\b\w64r --target rexcore
rem rexfilesystem) and the generated include dir of that build (default C:\rex\b\w64r\include;
rem override with REX_WASM_BUILD).
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
if "%REX_WASM_BUILD%"=="" set "REX_WASM_BUILD=C:\rex\b\w64r"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
set "OUT=%TEMP%\rex_web_tests"
if not exist "%OUT%" mkdir "%OUT%"
set "LIBS=%SDK%\out\wasm64"

em++ -std=c++23 -O2 -m64 -pthread -fwasm-exceptions -fno-char8_t -Wno-unsupported-floating-point-opt ^
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -I"%SDK%\include" -I"%REX_WASM_BUILD%\include" ^
  -I"%SDK%\thirdparty\fmt\include" -I"%SDK%\thirdparty\spdlog\include" -I"%SDK%\thirdparty\simde" ^
  -I"%SDK%\thirdparty\tomlplusplus\include" ^
  -sWASMFS -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sPTHREAD_POOL_SIZE=8 -sENVIRONMENT=node -sINITIAL_MEMORY=256MB ^
  "%SDK%\tests\web\host_path_device_test.cpp" "%LIBS%\librexfilesystem.a" "%LIBS%\librexcore.a" ^
  "%LIBS%\libspdlog.a" "%LIBS%\libfmt.a" -o "%OUT%\host_path_device_test.js" || exit /b 1

set "NODE=%EMSDK_NODE%"
if "%NODE%"=="" set "NODE=node"
"%NODE%" "%OUT%\host_path_device_test.js"
exit /b %ERRORLEVEL%
