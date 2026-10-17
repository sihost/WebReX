@echo off
rem Sparse checkout of Dawn at the emdawnwebgpu revision (v20260423.175430 = 31e25af2) into the
rem SDK's thirdparty/dawn, with only what Tint's SPIR-V reader + WGSL writer need:
rem src/cmake, src/tint, src/utils, include, third_party + the abseil-cpp, spirv-headers and
rem spirv-tools submodules at Dawn's pinned revisions. ~150 MB instead of several GB.
rem Usage: scripts\fetch_dawn.bat [sdk_root]   (default: this repository)
setlocal
set SDK=%~1
if "%SDK%"=="" set SDK=%~dp0..
set REV=31e25af254ab572c77054edec4946d2244e184dd
set DST=%SDK%\thirdparty\dawn
if not exist "%DST%\.git" (
  git -c core.longpaths=true clone --filter=blob:none --no-checkout https://github.com/google/dawn.git "%DST%" || exit /b 1
)
cd /d "%DST%" || exit /b 1
git config core.longpaths true
git sparse-checkout init --cone || exit /b 1
git sparse-checkout set src/cmake src/tint src/utils include third_party || exit /b 1
git -c core.longpaths=true checkout --quiet %REV% || exit /b 1
git -c core.longpaths=true submodule update --init --depth 1 third_party/spirv-headers/src third_party/spirv-tools/src third_party/abseil-cpp || exit /b 1
git log --oneline -1
