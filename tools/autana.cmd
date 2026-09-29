@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana.
rem The Store stub answers on PATH but exits instead of running, so each
rem candidate has to prove it runs.
setlocal
set "tools_root=%IDF_TOOLS_PATH%"
if not defined tools_root set "tools_root=%USERPROFILE%\.espressif"
set "python_exe="
python -c "" >nul 2>&1 && set "python_exe=python"
if not defined python_exe for /d %%e in ("%tools_root%\python_env\idf*_env") do (
    if exist "%%e\Scripts\python.exe" set "python_exe=%%e\Scripts\python.exe"
)
if not defined python_exe (
    echo autana: no Python found - install ESP-IDF, or see docs/tools/Autana-CLI.md 1>&2
    exit /b 1
)
"%python_exe%" "%~dp0..\scripts\autana\autana.py" %*
