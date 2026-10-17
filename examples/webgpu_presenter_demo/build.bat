@echo off
rem Builds the WebGPU presenter demo into out\web next to this file.
rem   build.bat [wasm64 SDK install prefix, default <repo>\out\install\wasm64]
rem Needs emsdk on PATH (run emsdk_env.bat first) and the SDK built + installed with the
rem wasm64-release preset (see WEB_PORT.md).
setlocal
set SRC=%~dp0
set PREFIX=%~1
if "%PREFIX%"=="" set PREFIX=%SRC%..\..\out\install\wasm64
if "%EMSDK%"=="" (echo EMSDK is not set - run emsdk_env.bat first & exit /b 1)
set OUT=%SRC%out
cmake -S "%SRC%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%EMSDK%/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake ^
  "-Drexglue_DIR=%PREFIX%/lib/cmake/rexglue" ^
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH ^
  "-DCMAKE_C_FLAGS=-m64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm" ^
  "-DCMAKE_CXX_FLAGS=-m64 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm" ^
  "-DCMAKE_EXE_LINKER_FLAGS=-m64 -pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm" || exit /b 1
cmake --build "%OUT%" || exit /b 1
echo built %OUT%\web - run: node "%SRC%serve.mjs"
