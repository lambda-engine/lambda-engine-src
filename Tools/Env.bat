@echo off
rem Shared environment for the Lambda Engine helper scripts.
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
set "PROJECT=%PROJECT_DIR%\LambdaEngine.uproject"
if not exist "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" (
	echo [LambdaEngine] Unreal Engine not found at "%UE_ROOT%". Set UE_ROOT to your UE 5.8 folder.
	exit /b 1
)
