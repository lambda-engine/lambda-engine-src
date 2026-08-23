@echo off
rem Opens the project in the Unreal Editor.
call "%~dp0Env.bat" || exit /b 1
start "" "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%PROJECT%" %*
