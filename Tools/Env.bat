@echo off
rem Shared environment for the Lambda Engine helper scripts.
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
set "PROJECT=%PROJECT_DIR%\LambdaEngine.uproject"
rem The game files live in their own repository; GameDir.txt beside the .uproject says where they are.
rem A GAME_DIR already set in the environment wins, so one build can be pointed elsewhere without
rem editing the file.
if not defined GAME_DIR if exist "%PROJECT_DIR%\GameDir.txt" (
	for /f "usebackq eol=# tokens=* delims=" %%L in ("%PROJECT_DIR%\GameDir.txt") do (
		if not defined GAME_DIR if not "%%~L"=="" set "GAME_DIR=%%~L"
	)
)
if defined GAME_DIR for %%I in ("%GAME_DIR%") do set "GAME_DIR=%%~fI"
rem The mod folder inside it, which is what -gamedir wants.
if defined GAME_DIR set "MOD_DIR=%GAME_DIR%\Mods\lambda"

if not exist "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" (
	echo [LambdaEngine] Unreal Engine not found at "%UE_ROOT%". Set UE_ROOT to your UE 5.8 folder.
	exit /b 1
)
