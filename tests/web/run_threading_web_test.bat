@echo off
rem Builds and runs the web threading backend tests under node (wasm64, Emscripten pthreads).
rem Usage: tests\web\run_threading_web_test.bat [release|debug] [runs]
rem Needs emsdk at %EMSDK_ROOT% (default C:\rex\emsdk). Output goes to %TEMP%\rex_web_tests.
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
set "OUT=%TEMP%\rex_web_tests"
if not exist "%OUT%" mkdir "%OUT%"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"
set "RUNS=%~2"
if "%RUNS%"=="" set "RUNS=1"
if /i "%MODE%"=="debug" (set "OPT=-O0 -g -sASSERTIONS=1") else (set "OPT=-O2 -DNDEBUG")

rem PTHREAD_POOL_SIZE is deliberately smaller than the 24-thread test so lazy Worker creation
rem is exercised too.
em++ -std=c++23 %OPT% -m64 -pthread -fwasm-exceptions -fno-char8_t -Wall -Wextra ^
  -Wno-unsupported-floating-point-opt -I"%SDK%\include" ^
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sPTHREAD_POOL_SIZE=8 -sENVIRONMENT=node -sINITIAL_MEMORY=1GB ^
  "%SDK%\src\core\threading_web.cpp" "%SDK%\src\core\threading.cpp" "%SDK%\src\core\clock_posix.cpp" ^
  "%SDK%\tests\web\threading_web_test.cpp" -o "%OUT%\threading_web_test_%MODE%.js" || exit /b 1

rem emsdk_env sets EMSDK_NODE (node 24: memory64 without flags).
set "NODE=%EMSDK_NODE%"
if "%NODE%"=="" set "NODE=node"
echo [%MODE%] %NODE% %OUT%\threading_web_test_%MODE%.js
set FAILED=0
for /L %%i in (1,1,%RUNS%) do (
  "%NODE%" "%OUT%\threading_web_test_%MODE%.js" || set FAILED=1
)
exit /b %FAILED%
