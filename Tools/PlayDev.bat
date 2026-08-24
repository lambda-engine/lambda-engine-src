@echo off
rem =====================================================================================================
rem  Development loop: compile the game code and run it straight from source - no cooking, no packaging.
rem  This is what you want while iterating on gameplay code.
rem
rem  Usage:  PlayDev.bat [mapname=test] [extra engine args...]
rem
rem  It runs the editor binary in -game mode against src\, reading content from game\Game\lambda.
rem  It does NOT touch game\LambdaEngine.exe - repackage with Package.bat when you want to update that.
rem =====================================================================================================
call "%~dp0Env.bat" || exit /b 1

set "MAP=%~1"
if "%MAP%"=="" set "MAP=test"
if not "%~1"=="" shift

if not defined MOD_DIR (
	echo [LambdaEngine] No game directory. Put the path to your game repository in "%PROJECT_DIR%\GameDir.txt".
	exit /b 1
)
set "GAMEDIR=%MOD_DIR%"

echo [LambdaEngine] Building LambdaEngineEditor...
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" LambdaEngineEditor Win64 Development -Project="%PROJECT%" -WaitMutex -NoHotReload
if errorlevel 1 (
	echo [LambdaEngine] Build FAILED - not launching.
	exit /b 1
)

echo [LambdaEngine] Running map '%MAP%' from source...
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%PROJECT%" -game -windowed -ResX=1280 -ResY=720 -log ^
	-sourcemap=%MAP% -gamedir="%GAMEDIR%" %1 %2 %3 %4 %5 %6 %7 %8 %9
