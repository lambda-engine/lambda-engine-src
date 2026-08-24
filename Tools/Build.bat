@echo off
rem =====================================================================================================
rem  Compiles a target. Defaults to the EDITOR target, which is what you want for iterating in-editor.
rem
rem  Usage: Build.bat [Target=LambdaEngineEditor] [Configuration=Development]
rem
rem  NOTE: this does NOT update the packaged game in ..\game. Building the editor target only produces
rem        the editor DLLs. To refresh the packaged game, run Release.bat.
rem        To compile and immediately play from source, use PlayDev.bat.
rem =====================================================================================================
call "%~dp0Env.bat" || exit /b 1
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=LambdaEngineEditor"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Development"

call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" %TARGET% Win64 %CONFIG% -Project="%PROJECT%" -WaitMutex -NoHotReload
if errorlevel 1 exit /b 1

rem If a packaged build exists and is now older than what we just compiled, say so - otherwise it is easy
rem to run game\Run.bat and wonder why the change is missing.
set "PACKAGED=%PROJECT_DIR%\..\game\LambdaEngine\Binaries\Win64\LambdaEngine.exe"
if exist "%PACKAGED%" (
	echo.
	echo [LambdaEngine] Reminder: the packaged build in ..\game was NOT updated by this command.
	for %%I in ("%PACKAGED%") do echo                game\LambdaEngine.exe was packaged %%~tI
	echo                Run Release.bat to refresh it, or Debug.bat to play the code you just built.
)
