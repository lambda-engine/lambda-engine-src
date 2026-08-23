@echo off
call "%~dp0Env.bat" || exit /b 1
"%UE_ROOT%\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" -projectfiles -project="%PROJECT%" -game -engine -progress
