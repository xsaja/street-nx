@echo off
rem STREET-vk.exe (Phase 2): MVP's Windows build with the Switch's Vulkan backend. Also recompiles the xbe into
rem generated\game for the Switch build. Needs Visual Studio 2026 with its Clang component; output in build-pc\.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=%VS%\VC\Tools\Llvm\x64\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
if not exist "game\assets\default.xbe" (
 echo No game\assets\default.xbe: link or copy the game files into street-nx\game\assets first.
 exit /b 1
)
if "%VERBOSE%"=="" set VERBOSE=OFF
cmake -S pc -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSTREET_VERBOSE_LOG=%VERBOSE%
if errorlevel 1 exit /b 1
rem first build: the config from the XDK signatures, then configure again with it
if not exist "game\config\game.toml" (
 ninja -C build-pc xbe_symbols || exit /b 1
 build-pc\xbe_symbols.exe game\assets\default.xbe sdk\xdk --out game\config\game.toml || exit /b 1
 cmake -S pc -B build-pc || exit /b 1
)
if "%JOBS%"=="" set JOBS=10
ninja -C build-pc -k 0 -j%JOBS% STREET-vk STREET-d3d12
if errorlevel 1 exit /b 1
echo Built build-pc\STREET-vk.exe
