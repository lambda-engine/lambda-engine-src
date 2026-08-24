@echo off
setlocal enabledelayedexpansion
rem =====================================================================================================
rem  Packages the game into ..\..\game so that folder is self-contained and runnable:
rem
rem      game\LambdaEngine.exe          <- launcher
rem      game\Engine\ , game\LambdaEngine\   <- cooked engine + game data
rem      game\lambda\                   <- mod content (gameinfo.txt, maps, materials) - NOT overwritten
rem      <map>.bat                      <- convenience launcher, named for the map it runs
rem
rem  Usage: Package.bat [Configuration=Development]     (Development or Shipping)
rem =====================================================================================================
call "%~dp0Env.bat" || exit /b 1

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Development"

if not defined GAME_DIR (
	echo [LambdaEngine] No game directory. Put the path to your game repository in "%PROJECT_DIR%\GameDir.txt".
	exit /b 1
)
if not exist "%GAME_DIR%" (
	echo [LambdaEngine] GameDir.txt points at "%GAME_DIR%", which does not exist.
	exit /b 1
)
set "STAGE_DIR=%PROJECT_DIR%\Saved\Packaged"

echo [LambdaEngine] Packaging %CONFIG% into "%GAME_DIR%"
echo [LambdaEngine] Staging to "%STAGE_DIR%" first...

call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
	-project="%PROJECT%" ^
	-noP4 -utf8output -nop4 ^
	-platform=Win64 ^
	-clientconfig=%CONFIG% ^
	-build -cook -stage -pak -archive ^
	-archivedirectory="%STAGE_DIR%" ^
	-prereqs=false
if errorlevel 1 (
	echo [LambdaEngine] Packaging FAILED.
	exit /b 1
)

rem UAT archives to <STAGE_DIR>\Windows. Flatten that into game\ so the exe sits beside lambda\.
set "ARCHIVED=%STAGE_DIR%\Windows"
if not exist "%ARCHIVED%" (
	echo [LambdaEngine] Expected archived build at "%ARCHIVED%" but it does not exist.
	exit /b 1
)

echo [LambdaEngine] Copying build into "%GAME_DIR%" (lambda\ is preserved)...
robocopy "%ARCHIVED%" "%GAME_DIR%" /E /NFL /NDL /NJH /NJS /NP /XD "%GAME_DIR%\Game" >nul
if errorlevel 8 (
	echo [LambdaEngine] Copy FAILED.
	exit /b 1
)

rem The staged launcher is built from BootstrapPackagedGame and keeps Epic's icon alongside ours, so strip it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0FixLauncherIcon.ps1" -Exe "%GAME_DIR%\LambdaEngine.exe"
ie4uinit.exe -show >nul 2>&1

echo.
echo [LambdaEngine] Done. Run the game with:
echo     "%GAME_DIR%\startup.bat"
endlocal
