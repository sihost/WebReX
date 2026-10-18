@echo off
rem Builds the HttpRangeDevice browser benchmark (tests/web/http_range_bench.cpp) into
rem %OUT% (default %TEMP%\rex_web_bench): http_range_bench.html + .js + .wasm.
rem Needs a wasm64 SDK build: REX_WASM_BUILD = its build dir (static libs + generated include).
rem Run: serve %OUT% with the game server, e.g.
rem   node server/index.mjs --assets <data root> --web %OUT% --port 8103 --throttle 4 --throttle-latency 150
rem   node tests/web/run_in_chrome.mjs --url "http://127.0.0.1:8103/http_range_bench.html" --done "BENCH DONE" --seconds 900
setlocal
set "SDK=%~dp0..\.."
if "%EMSDK_ROOT%"=="" set "EMSDK_ROOT=C:\rex\emsdk"
if "%REX_WASM_BUILD%"=="" set "REX_WASM_BUILD=C:\rex\b\w64r-rt"
if "%OUT%"=="" set "OUT=%TEMP%\rex_web_bench"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1
if not exist "%OUT%" mkdir "%OUT%"
set "B=%REX_WASM_BUILD%"
rem The wasm64 presets put the static libs in <sdk>\out\wasm64 (override with REX_WASM_LIBS).
if "%REX_WASM_LIBS%"=="" set "REX_WASM_LIBS=%SDK%\out\wasm64"
set "L=%REX_WASM_LIBS%"

em++ -std=c++23 -O2 -m64 -pthread -fwasm-exceptions -fno-char8_t -Wno-unsupported-floating-point-opt ^
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -I"%SDK%\include" -I"%B%\include" ^
  -I"%SDK%\thirdparty\fmt\include" -I"%SDK%\thirdparty\spdlog\include" -I"%SDK%\thirdparty\simde" ^
  -I"%SDK%\thirdparty\tomlplusplus\include" ^
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sPTHREAD_POOL_SIZE=12 -sENVIRONMENT=web,worker -sINITIAL_MEMORY=1GB ^
  "%SDK%\tests\web\http_range_bench.cpp" "%L%\librexfilesystem.a" "%L%\librexcore.a" ^
  "%L%\libspdlog.a" "%L%\libfmt.a" -o "%OUT%\http_range_bench.js" || exit /b 1
copy /y "%SDK%\tests\web\http_range_bench.html" "%OUT%\http_range_bench.html" >nul
echo built %OUT%\http_range_bench.html
