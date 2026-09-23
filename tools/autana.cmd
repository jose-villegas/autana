@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana.
rem The Store stub answers on PATH but exits instead of running, so each
rem candidate has to prove it runs.
setlocal
set "AUTANA_TOOLS=%IDF_TOOLS_PATH%"
if not defined AUTANA_TOOLS set "AUTANA_TOOLS=%USERPROFILE%\.espressif"
set "AUTANA_PYTHON="
python -c "" >nul 2>&1 && set "AUTANA_PYTHON=python"
if not defined AUTANA_PYTHON for /d %%e in ("%AUTANA_TOOLS%\python_env\idf*_env") do (
    if exist "%%e\Scripts\python.exe" set "AUTANA_PYTHON=%%e\Scripts\python.exe"
)
if not defined AUTANA_PYTHON (
    echo autana: no Python found - install ESP-IDF, or see docs/tools/Autana-CLI.md 1>&2
    exit /b 1
)
"%AUTANA_PYTHON%" "%~dp0..\scripts\autana\autana.py" %*
