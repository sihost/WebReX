@echo off
rem Builds and runs the web fiber backend tests (src/core/fiber_web.cpp) under node or in
rem headless Chrome (tests\web\run_in_chrome.mjs, Playwright from REX_PLAYWRIGHT).
rem Usage: tests\web\run_fiber_web_test.bat [node|chrome] [release|debug]
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
set "OUT=%TEMP%\rex_web_tests"
if not exist "%OUT%" mkdir "%OUT%"
set "HOST=%~1"
if "%HOST%"=="" set "HOST=node"
set "MODE=%~2"
if "%MODE%"=="" set "MODE=release"
if /i "%MODE%"=="debug" (set "OPT=-O0 -g -sASSERTIONS=1") else (set "OPT=-O2 -DNDEBUG")
if /i "%HOST%"=="chrome" (set "ENV=-sENVIRONMENT=web,worker" & set "EXT=html") else (set "ENV=-sENVIRONMENT=node" & set "EXT=js")

em++ -std=c++23 %OPT% -m64 -pthread -fwasm-exceptions -fno-char8_t -Wall -Wextra ^
  -Wno-unsupported-floating-point-opt -I"%SDK%\include" ^
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sPTHREAD_POOL_SIZE=8 %ENV% -sINITIAL_MEMORY=1GB ^
  "%SDK%\src\core\fiber_web.cpp" "%SDK%\tests\web\cvar_stub.cpp" "%SDK%\src\core\threading_web.cpp" "%SDK%\src\core\threading.cpp" ^
  "%SDK%\src\core\clock_posix.cpp" "%SDK%\tests\web\fiber_web_test.cpp" ^
  -o "%OUT%\fiber_web_test_%HOST%_%MODE%.%EXT%" || exit /b 1

if /i "%HOST%"=="chrome" (
  node "%SDK%\tests\web\run_in_chrome.mjs" "%OUT%\fiber_web_test_%HOST%_%MODE%.html" --seconds 120
  exit /b %ERRORLEVEL%
)
set "NODE=%EMSDK_NODE%"
if "%NODE%"=="" set "NODE=node"
"%NODE%" "%OUT%\fiber_web_test_%HOST%_%MODE%.js"
exit /b %ERRORLEVEL%
