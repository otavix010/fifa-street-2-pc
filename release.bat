@echo off
set "GAME_EXE=FIFAStreet2"
cd /d "%~dp0"
set "OUT=out\build\release"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & exit /b 1)
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || exit /b 1
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && exit /b 1
set /p SDK=<out\build\release\sdk_dir.txt
del /q "%GAME_EXE%.zip" 2>nul
rem only the runtime DLLs the executables import; xbe_recomp.exe needs its own copy, Windows only looks beside the
rem exe it starts
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul
rem the package, gathered in one folder: the exe, the compiler folder without its work folder (no game code goes in
rem the zip, the first start builds it), an empty assets folder for the game, the upscalers and every licence
set "P=%OUT%\pacote"
rd /s /q "%P%" 2>nul
mkdir "%P%\assets" "%P%\licencas"
copy /y "%OUT%\%GAME_EXE%.exe" "%P%\" >nul || exit /b 1
robocopy "%OUT%\compiler" "%P%\compiler" /e /xd work /njh /njs /nfl /ndl >nul
for %%f in (nvngx_dlss.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll libxess.dll) do copy /y "%OUT%\%%f" "%P%\" >nul
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do copy /y "%VCRUNTIME%\%%f.dll" "%P%\" >nul
copy /y README.md "%P%\" >nul
copy /y LICENSE "%P%\LICENSE.txt" >nul
copy /y "%SDK%\THIRD_PARTY.txt" "%P%\" >nul
copy /y third_party\dlss\LICENSE.txt "%P%\licencas\NVIDIA-DLSS.txt" >nul
copy /y third_party\xess\LICENSE.txt "%P%\licencas\Intel-XeSS.txt" >nul
copy /y third_party\ffx\readme.md "%P%\licencas\AMD-FidelityFX.md" >nul
echo Coloque aqui a sua copia do jogo: a imagem .iso do disco ou os arquivos extraidos.> "%P%\assets\LEIA-ME.txt"
rem Windows' own tar (libarchive) writes zip
"%SystemRoot%\System32\tar.exe" -a -c -f "%GAME_EXE%.zip" -C "%P%" * || exit /b 1
echo pronto: %GAME_EXE%.zip
