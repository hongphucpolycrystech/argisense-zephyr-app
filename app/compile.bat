@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "APP_DIR=%%~fI"
for %%I in ("%APP_DIR%\..") do set "WORKSPACE_DIR=%%~fI"

set "APP_NAME=argisense-zephyr-app"
if not defined BOARD set "BOARD=argisense_mp_u575rg"
if not defined PRISTINE set "PRISTINE=always"
if not defined ZEPHYR_SDK_INSTALL_DIR set "ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1"
if not defined ARGISENSE_DEFAULT_FLASH_DEV_ID set "ARGISENSE_DEFAULT_FLASH_DEV_ID=003600253234510237333934"

set "CLOCK_SOURCE=hse"
set "CLOCK_SNIPPET_ARGS="
set "SNIPPET_ARGS="
set "APP_SNIPPET_VALUE="
set "APP_SYSBUILD_SNIPPET_ARG="
set "USB_CONSOLE=no"
set "BUILD_MODE=app"
set "SYSBUILD_ARGS="
set "FIRMWARE_VERSION="
set "VERSION_FILE=%APP_DIR%\VERSION"
set "FLASH_AFTER_BUILD=no"
set "FLASH_ONLY=no"
set "FLASH_TARGET=app"

set "ARG1=%~1"
set "ARG2=%~2"
set "ARG3=%~3"
set "ARG4=%~4"
set "ARG5=%~5"
set "ARG6=%~6"
set "ARG7=%~7"
set "ARG8=%~8"
set "ARG9=%~9"

if /I "%ARG1%"=="help" goto :usage
if /I "%ARG1%"=="/?" goto :usage
if /I "%ARG1%"=="-h" goto :usage
if /I "%ARG1%"=="--help" goto :usage

goto :parse_args

:after_parse_args

set "SNIPPET_ARGS="
if defined CLOCK_SNIPPET_ARGS set "SNIPPET_ARGS=%SNIPPET_ARGS% %CLOCK_SNIPPET_ARGS%"
if /I "%USB_CONSOLE%"=="yes" (
	if /I "%BUILD_MODE%"=="mcuboot" (
		if /I "%CLOCK_SOURCE%"=="hsi" (
			set "APP_SNIPPET_VALUE=argisense-u575-hsi;argisense-usb-console"
		) else (
			set "APP_SNIPPET_VALUE=argisense-usb-console"
		)
		set APP_SYSBUILD_SNIPPET_ARG="-D%APP_NAME%_SNIPPET=!APP_SNIPPET_VALUE!"
	) else (
		set "SNIPPET_ARGS=%SNIPPET_ARGS% -S argisense-usb-console"
	)
)

if not defined FLASH_DEV_ID if defined STLINK_SN set "FLASH_DEV_ID=%STLINK_SN%"
if not defined FLASH_DEV_ID if defined ARGISENSE_DEFAULT_FLASH_DEV_ID set "FLASH_DEV_ID=%ARGISENSE_DEFAULT_FLASH_DEV_ID%"

if /I not "%FLASH_ONLY%"=="yes" (
	if defined FW_VERSION if not defined FIRMWARE_VERSION set "FIRMWARE_VERSION=%FW_VERSION%"
)

if /I "%FLASH_ONLY%"=="yes" (
	if defined FIRMWARE_VERSION (
		echo ERROR: flash-only cannot change firmware version.
		echo Build/sign first with a command like: compile.bat mcuboot hsi version 1.2.1+8
		exit /b 1
	)
) else (
	if defined FIRMWARE_VERSION (
		call :write_version "%FIRMWARE_VERSION%"
		if errorlevel 1 exit /b 1
	) else (
		call :read_version
		if errorlevel 1 exit /b 1
	)
)

echo.
echo ArgiSense Zephyr build
echo ======================
echo Workspace : %WORKSPACE_DIR%
echo App       : %APP_DIR%
echo Board     : %BOARD%
echo Mode      : %BUILD_MODE%
echo Clock     : %CLOCK_SOURCE%
echo USB shell : %USB_CONSOLE%
echo Snippets  : %SNIPPET_ARGS%
echo App snippet: %APP_SNIPPET_VALUE%
echo Version   : %FIRMWARE_VERSION%
echo Flash     : %FLASH_AFTER_BUILD%
echo Flash only: %FLASH_ONLY%
echo Flash target: %FLASH_TARGET%
echo Flash dev : %FLASH_DEV_ID%
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

if /I not "%FLASH_ONLY%"=="yes" if not exist "%ZEPHYR_SDK_INSTALL_DIR%" (
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

if /I "%FLASH_ONLY%"=="yes" (
	call :flash_firmware
	set "SCRIPT_RESULT=!ERRORLEVEL!"
	popd >nul
	exit /b !SCRIPT_RESULT!
)

echo Running west build...
echo.
if /I "%BUILD_MODE%"=="mcuboot" (
	py -3.12 -m west build -p %PRISTINE% %SYSBUILD_ARGS% -b %BOARD% %SNIPPET_ARGS% "%APP_DIR%" -- "-DBOARD_ROOT=%APP_DIR%" "-DDTS_ROOT=%APP_DIR%" "-DSNIPPET_ROOT=%APP_DIR%" %APP_SYSBUILD_SNIPPET_ARG%
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

if /I "%FLASH_AFTER_BUILD%"=="yes" (
	call :flash_firmware
	if errorlevel 1 (
		set "SCRIPT_RESULT=!ERRORLEVEL!"
		popd >nul
		exit /b !SCRIPT_RESULT!
	)
)

popd >nul
exit /b 0

:flash_firmware
if not exist "%WORKSPACE_DIR%\build" (
	echo ERROR: Build directory not found:
	echo   %WORKSPACE_DIR%\build
	echo Build once before using flash-only.
	exit /b 1
)

echo.
if /I "%FLASH_TARGET%"=="all" (
	echo Flashing MCUboot and application without rebuilding...
) else (
	echo Flashing application image only without rebuilding...
)
echo.
set "FLASH_DEV_ID_ARGS="
if defined FLASH_DEV_ID set "FLASH_DEV_ID_ARGS=--dev-id %FLASH_DEV_ID%"
if /I "%FLASH_TARGET%"=="all" (
	py -3.12 -m west flash --no-rebuild -d "%WORKSPACE_DIR%\build" %FLASH_DEV_ID_ARGS%
) else if exist "%WORKSPACE_DIR%\build\domains.yaml" (
	py -3.12 -m west flash --no-rebuild -d "%WORKSPACE_DIR%\build" --domain %APP_NAME% %FLASH_DEV_ID_ARGS%
) else (
	py -3.12 -m west flash --no-rebuild -d "%WORKSPACE_DIR%\build" %FLASH_DEV_ID_ARGS%
)
set "FLASH_RESULT=!ERRORLEVEL!"
if not "!FLASH_RESULT!"=="0" (
	echo.
	echo Flash failed with exit code !FLASH_RESULT!.
	exit /b !FLASH_RESULT!
)

echo.
echo Flash completed successfully.
exit /b 0

:usage
echo.
echo Usage:
echo   compile.bat
echo   compile.bat hse
echo   compile.bat hsi
echo   compile.bat usbconsole
echo   compile.bat hsi usbconsole
echo   compile.bat hsi ^<board^>
echo   compile.bat ^<board^>
echo   compile.bat version ^<major.minor.patch+build^>
echo   compile.bat hsi version ^<major.minor.patch+build^>
echo   compile.bat flash
echo   compile.bat flash-only
echo   compile.bat flash-all
echo   compile.bat flash-all-only
echo   compile.bat stlink ^<st-link-serial^> flash-only
echo   compile.bat --dev-id ^<st-link-serial^> flash-only
echo   compile.bat hsi flash
echo   compile.bat mcuboot
echo   compile.bat mcuboot hsi
echo   compile.bat mcuboot usbconsole
echo   compile.bat mcuboot hsi usbconsole
echo   compile.bat mcuboot hsi ^<board^>
echo   compile.bat mcuboot hsi version ^<major.minor.patch+build^>
echo   compile.bat mcuboot hsi version ^<major.minor.patch+build^> flash
echo   compile.bat mcuboot hsi version ^<major.minor.patch+build^> flash-all
echo.
echo Defaults:
echo   BOARD=argisense_mp_u575rg
echo   PRISTINE=always
echo   ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
echo   VERSION is read from %APP_NAME%\VERSION unless FW_VERSION or the version argument is set
echo   FLASH is disabled unless the flash or flash-all argument is set
echo   FLASH targets the application domain only by default
echo   FLASH-ALL also flashes MCUboot; use it only when the bootloader changed
echo   FLASH-ONLY runs west flash --no-rebuild from the existing build directory
echo   FLASH_DEV_ID or STLINK_SN selects the debugger when multiple ST-LINKs are connected
echo   ARGISENSE_DEFAULT_FLASH_DEV_ID defaults to 003600253234510237333934
echo.
echo Examples:
echo   compile.bat
echo   compile.bat hsi
echo   compile.bat usbconsole
echo   compile.bat hsi usbconsole
echo   compile.bat flash-only
echo   compile.bat flash-all-only
echo   compile.bat hsi flash
echo   compile.bat mcuboot
echo   compile.bat mcuboot hsi
echo   compile.bat mcuboot usbconsole
echo   compile.bat mcuboot hsi usbconsole
echo   compile.bat version 1.2.0+7
echo   compile.bat mcuboot hsi version 1.2.0+7
echo   compile.bat mcuboot hsi version 1.2.0+7 flash
echo   compile.bat mcuboot hsi version 1.2.0+7 flash-all
echo   compile.bat stlink 003600253234510237333934 flash-only
echo   PowerShell: $env:FLASH_DEV_ID="003600253234510237333934"
echo   cmd.exe: set FLASH_DEV_ID=003600253234510237333934
echo   compile.bat flash-only
echo   set FW_VERSION=1.2.0+7
echo   compile.bat mcuboot
echo   set PRISTINE=auto
echo   compile.bat hse
exit /b 0

:parse_args
if "%ARG1%"=="" goto :after_parse_args
if /I "%ARG1%"=="help" goto :usage
if /I "%ARG1%"=="/?" goto :usage
if /I "%ARG1%"=="-h" goto :usage
if /I "%ARG1%"=="--help" goto :usage
if /I "%ARG1%"=="mcuboot" (
	set "BUILD_MODE=mcuboot"
	set "SYSBUILD_ARGS=--sysbuild"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="hsi" (
	set "CLOCK_SOURCE=hsi"
	set "CLOCK_SNIPPET_ARGS=-S argisense-u575-hsi"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="hse" (
	set "CLOCK_SOURCE=hse"
	set "CLOCK_SNIPPET_ARGS="
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="usbconsole" (
	set "USB_CONSOLE=yes"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="usb-console" (
	set "USB_CONSOLE=yes"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="flash" (
	set "FLASH_AFTER_BUILD=yes"
	set "FLASH_TARGET=app"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="flash-only" (
	set "FLASH_ONLY=yes"
	set "FLASH_TARGET=app"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="flash-all" (
	set "FLASH_AFTER_BUILD=yes"
	set "FLASH_TARGET=all"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="flash-all-only" (
	set "FLASH_ONLY=yes"
	set "FLASH_TARGET=all"
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="stlink" (
	if "%ARG2%"=="" (
		echo ERROR: Missing ST-LINK serial after "stlink".
		exit /b 1
	)
	set "FLASH_DEV_ID=%ARG2%"
	call :shift_args
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="dev-id" (
	if "%ARG2%"=="" (
		echo ERROR: Missing ST-LINK serial after "dev-id".
		exit /b 1
	)
	set "FLASH_DEV_ID=%ARG2%"
	call :shift_args
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="--dev-id" (
	if "%ARG2%"=="" (
		echo ERROR: Missing ST-LINK serial after "--dev-id".
		exit /b 1
	)
	set "FLASH_DEV_ID=%ARG2%"
	call :shift_args
	call :shift_args
	goto :parse_args
)
if /I "%ARG1%"=="version" (
	if "%ARG2%"=="" (
		echo ERROR: Missing firmware version after "version".
		echo Expected format: major.minor.patch+build, for example 1.2.0+7
		exit /b 1
	)
	set "FIRMWARE_VERSION=%ARG2%"
	call :shift_args
	call :shift_args
	goto :parse_args
)
set "BOARD=%ARG1%"
call :shift_args
goto :parse_args

:shift_args
set "ARG1=%ARG2%"
set "ARG2=%ARG3%"
set "ARG3=%ARG4%"
set "ARG4=%ARG5%"
set "ARG5=%ARG6%"
set "ARG6=%ARG7%"
set "ARG7=%ARG8%"
set "ARG8=%ARG9%"
set "ARG9="
exit /b 0

:write_version
set "VERSION_INPUT=%~1"
set "VERSION_BASE="
set "VERSION_BUILD="
set "VERSION_MAJOR="
set "VERSION_MINOR="
set "VERSION_PATCH="
set "VERSION_EXTRA_TOKEN="

for /f "tokens=1,2 delims=+" %%A in ("%VERSION_INPUT%") do (
	set "VERSION_BASE=%%A"
	set "VERSION_BUILD=%%B"
)

if not defined VERSION_BASE goto :version_error
if not defined VERSION_BUILD goto :version_error

for /f "tokens=1,2,3,4 delims=." %%A in ("%VERSION_BASE%") do (
	set "VERSION_MAJOR=%%A"
	set "VERSION_MINOR=%%B"
	set "VERSION_PATCH=%%C"
	set "VERSION_EXTRA_TOKEN=%%D"
)

if not defined VERSION_MAJOR goto :version_error
if not defined VERSION_MINOR goto :version_error
if not defined VERSION_PATCH goto :version_error
if defined VERSION_EXTRA_TOKEN goto :version_error

call :validate_version_part "%VERSION_MAJOR%" "major"
if errorlevel 1 exit /b 1
call :validate_version_part "%VERSION_MINOR%" "minor"
if errorlevel 1 exit /b 1
call :validate_version_part "%VERSION_PATCH%" "patch"
if errorlevel 1 exit /b 1
call :validate_version_part "%VERSION_BUILD%" "build"
if errorlevel 1 exit /b 1

set "FIRMWARE_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH%+%VERSION_BUILD%"

call :read_current_version >nul 2>nul
if not errorlevel 1 (
	if "%CURRENT_VERSION_MAJOR%"=="%VERSION_MAJOR%" if "%CURRENT_VERSION_MINOR%"=="%VERSION_MINOR%" if "%CURRENT_VERSION_PATCH%"=="%VERSION_PATCH%" if "%CURRENT_VERSION_BUILD%"=="%VERSION_BUILD%" (
		echo VERSION already set to %FIRMWARE_VERSION%; not rewriting %VERSION_FILE%.
		exit /b 0
	)
)

> "%VERSION_FILE%" echo VERSION_MAJOR = %VERSION_MAJOR%
>> "%VERSION_FILE%" echo VERSION_MINOR = %VERSION_MINOR%
>> "%VERSION_FILE%" echo PATCHLEVEL = %VERSION_PATCH%
>> "%VERSION_FILE%" echo VERSION_TWEAK = %VERSION_BUILD%
>> "%VERSION_FILE%" echo EXTRAVERSION =

exit /b 0

:validate_version_part
set "VERSION_PART=%~1"
set "VERSION_PART_NAME=%~2"
if "%VERSION_PART%"=="" (
	echo ERROR: Empty firmware version %VERSION_PART_NAME% value.
	exit /b 1
)
for /f "delims=0123456789" %%A in ("%VERSION_PART%") do (
	echo ERROR: Firmware version %VERSION_PART_NAME% must be numeric: %VERSION_PART%
	exit /b 1
)
set /a VERSION_PART_NUM=%VERSION_PART% >nul 2>nul
if errorlevel 1 (
	echo ERROR: Firmware version %VERSION_PART_NAME% must be numeric: %VERSION_PART%
	exit /b 1
)
if %VERSION_PART_NUM% LSS 0 (
	echo ERROR: Firmware version %VERSION_PART_NAME% must be >= 0.
	exit /b 1
)
if %VERSION_PART_NUM% GTR 255 (
	echo ERROR: Firmware version %VERSION_PART_NAME% must be <= 255 for Zephyr VERSION fields.
	exit /b 1
)
exit /b 0

:read_version
if not exist "%VERSION_FILE%" (
	echo ERROR: VERSION file not found: %VERSION_FILE%
	echo Run "compile.bat version 0.1.0+0" to create it.
	exit /b 1
)
for /f "tokens=1,2,3" %%A in ('findstr /B "VERSION_MAJOR VERSION_MINOR PATCHLEVEL VERSION_TWEAK" "%VERSION_FILE%"') do (
	if "%%A"=="VERSION_MAJOR" set "VERSION_MAJOR=%%C"
	if "%%A"=="VERSION_MINOR" set "VERSION_MINOR=%%C"
	if "%%A"=="PATCHLEVEL" set "VERSION_PATCH=%%C"
	if "%%A"=="VERSION_TWEAK" set "VERSION_BUILD=%%C"
)
if not defined VERSION_MAJOR goto :read_version_error
if not defined VERSION_MINOR goto :read_version_error
if not defined VERSION_PATCH goto :read_version_error
if not defined VERSION_BUILD goto :read_version_error
set "FIRMWARE_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH%+%VERSION_BUILD%"
exit /b 0

:read_current_version
set "CURRENT_VERSION_MAJOR="
set "CURRENT_VERSION_MINOR="
set "CURRENT_VERSION_PATCH="
set "CURRENT_VERSION_BUILD="
if not exist "%VERSION_FILE%" exit /b 1
for /f "tokens=1,2,3" %%A in ('findstr /B "VERSION_MAJOR VERSION_MINOR PATCHLEVEL VERSION_TWEAK" "%VERSION_FILE%"') do (
	if "%%A"=="VERSION_MAJOR" set "CURRENT_VERSION_MAJOR=%%C"
	if "%%A"=="VERSION_MINOR" set "CURRENT_VERSION_MINOR=%%C"
	if "%%A"=="PATCHLEVEL" set "CURRENT_VERSION_PATCH=%%C"
	if "%%A"=="VERSION_TWEAK" set "CURRENT_VERSION_BUILD=%%C"
)
if not defined CURRENT_VERSION_MAJOR exit /b 1
if not defined CURRENT_VERSION_MINOR exit /b 1
if not defined CURRENT_VERSION_PATCH exit /b 1
if not defined CURRENT_VERSION_BUILD exit /b 1
exit /b 0

:version_error
echo ERROR: Invalid firmware version "%VERSION_INPUT%".
echo Expected format: major.minor.patch+build, for example 1.2.0+7
exit /b 1

:read_version_error
echo ERROR: Failed to read firmware version from "%VERSION_FILE%".
exit /b 1
