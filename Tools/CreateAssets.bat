@echo off
rem Creates the editor-side assets the runtime expects (master material + empty startup level) via editor Python.
call "%~dp0Env.bat" || exit /b 1
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -run=pythonscript -script="%PROJECT_DIR%\Tools\create_assets.py" -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
