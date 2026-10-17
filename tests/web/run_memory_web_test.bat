@echo off
rem Builds and runs the web rex::memory backend test under node (wasm64).
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
set "OUT=%TEMP%\rex_web_tests"
if not exist "%OUT%" mkdir "%OUT%"

em++ -std=c++23 -O2 -m64 -pthread -fwasm-exceptions -fno-char8_t -Wall -Wextra ^
  -Wno-unsupported-floating-point-opt -I"%SDK%\include" -I"%SDK%\thirdparty\fmt\include" ^
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sENVIRONMENT=node -sINITIAL_MEMORY=256MB ^
  "%SDK%\src\core\memory_web.cpp" "%SDK%\tests\web\memory_web_test.cpp" ^
  -o "%OUT%\memory_web_test.js" || exit /b 1

set "NODE=%EMSDK_NODE%"
if "%NODE%"=="" set "NODE=node"
"%NODE%" "%OUT%\memory_web_test.js"
exit /b %ERRORLEVEL%
