@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "APP_DIR=%%~fI"
for %%I in ("%APP_DIR%\..") do set "WORKSPACE_DIR=%%~fI"

set "APP_NAME=argisense-zephyr-app"
if not defined BOARD set "BOARD=argisense_mp_u575rg"
if not defined PRISTINE set "PRISTINE=always"
if not defined ZEPHYR_SDK_INSTALL_DIR set "ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1"

set "CLOCK_SOURCE=hse"
set "SNIPPET_ARGS="
set "BUILD_MODE=app"
set "SYSBUILD_ARGS="

if /I "%~1"=="help" goto :usage
if /I "%~1"=="/?" goto :usage
if /I "%~1"=="-h" goto :usage
if /I "%~1"=="--help" goto :usage

if /I "%~1"=="mcuboot" (
	set "BUILD_MODE=mcuboot"
	set "SYSBUILD_ARGS=--sysbuild"
	if /I "%~2"=="hsi" (
		set "CLOCK_SOURCE=hsi"
		set "SNIPPET_ARGS=-S argisense-u575-hsi"
		if not "%~3"=="" set "BOARD=%~3"
	) else if /I "%~2"=="hse" (
		set "CLOCK_SOURCE=hse"
		if not "%~3"=="" set "BOARD=%~3"
	) else if not "%~2"=="" (
		set "BOARD=%~2"
	)
) else if /I "%~1"=="hsi" (
	set "CLOCK_SOURCE=hsi"
	set "SNIPPET_ARGS=-S argisense-u575-hsi"
	if not "%~2"=="" set "BOARD=%~2"
) else if /I "%~1"=="hse" (
	set "CLOCK_SOURCE=hse"
	if not "%~2"=="" set "BOARD=%~2"
) else if not "%~1"=="" (
	set "BOARD=%~1"
)

echo.
echo ArgiSense Zephyr build
echo ======================
echo Workspace : %WORKSPACE_DIR%
echo App       : %APP_DIR%
echo Board     : %BOARD%
echo Mode      : %BUILD_MODE%
echo Clock     : %CLOCK_SOURCE%
echo SDK       : %ZEPHYR_SDK_INSTALL_DIR%
echo Pristine  : %PRISTINE%
echo.

if not exist "%WORKSPACE_DIR%\.west" (
	echo ERROR: West workspace not found at "%WORKSPACE_DIR%".
	echo Run this first from "%WORKSPACE_DIR%":
	echo   west init -l %APP_NAME%
	echo   west update
	exit /b 1
)

if not exist "%WORKSPACE_DIR%\zephyr" (
	echo ERROR: Zephyr tree not found at "%WORKSPACE_DIR%\zephyr".
	echo Run "west update" from "%WORKSPACE_DIR%".
	exit /b 1
)

if not exist "%ZEPHYR_SDK_INSTALL_DIR%" (
	echo ERROR: ZEPHYR_SDK_INSTALL_DIR does not exist:
	echo   %ZEPHYR_SDK_INSTALL_DIR%
	echo Set it before running this script if your SDK is elsewhere.
	exit /b 1
)

py -3.12 --version >nul 2>nul
if errorlevel 1 (
	echo ERROR: Python 3.12 was not found by the py launcher.
	echo Install Python 3.12, then run:
	echo   py -3.12 -m pip install -r "%WORKSPACE_DIR%\zephyr\scripts\requirements.txt"
	exit /b 1
)

pushd "%WORKSPACE_DIR%" >nul

echo Running west build...
echo.
if /I "%BUILD_MODE%"=="mcuboot" (
	py -3.12 -m west build -p %PRISTINE% %SYSBUILD_ARGS% -b %BOARD% %SNIPPET_ARGS% "%APP_DIR%" -- "-DBOARD_ROOT=%APP_DIR%" "-DDTS_ROOT=%APP_DIR%" "-DSNIPPET_ROOT=%APP_DIR%"
) else (
	py -3.12 -m west build -p %PRISTINE% -b %BOARD% %SNIPPET_ARGS% "%APP_DIR%"
)
set "BUILD_RESULT=%ERRORLEVEL%"

if not "%BUILD_RESULT%"=="0" (
	popd >nul
	echo.
	echo Build failed with exit code %BUILD_RESULT%.
	exit /b %BUILD_RESULT%
)

echo.
echo Build completed successfully.
echo Output:
if /I "%BUILD_MODE%"=="mcuboot" (
	echo   %WORKSPACE_DIR%\build\mcuboot\zephyr\zephyr.hex
	echo   %WORKSPACE_DIR%\build\argisense-zephyr-app\zephyr\zephyr.signed.bin
	echo   %WORKSPACE_DIR%\build\argisense-zephyr-app\zephyr\zephyr.signed.hex
) else (
	echo   %WORKSPACE_DIR%\build\zephyr\zephyr.elf
	echo   %WORKSPACE_DIR%\build\zephyr\zephyr.bin
	echo   %WORKSPACE_DIR%\build\zephyr\zephyr.hex
)

popd >nul
exit /b 0

:usage
echo.
echo Usage:
echo   compile.bat
echo   compile.bat hse
echo   compile.bat hsi
echo   compile.bat hsi ^<board^>
echo   compile.bat ^<board^>
echo   compile.bat mcuboot
echo   compile.bat mcuboot hsi
echo   compile.bat mcuboot hsi ^<board^>
echo.
echo Defaults:
echo   BOARD=argisense_mp_u575rg
echo   PRISTINE=always
echo   ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
echo.
echo Examples:
echo   compile.bat
echo   compile.bat hsi
echo   compile.bat mcuboot
echo   compile.bat mcuboot hsi
echo   set PRISTINE=auto
echo   compile.bat hse
exit /b 0
