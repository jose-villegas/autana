@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana,
rem which resolves the same interpreter and says why it has to be ESP-IDF's.
rem
rem The highest idf version wins, compared as zero-padded numbers: a plain
rem name sort puts idf5.9 after idf5.10 and would pick a different interpreter
rem than ./autana's `sort -V` does. No labels here - this file is stored with
rem LF endings, and cmd cannot reliably find a batch label in one.
setlocal enabledelayedexpansion
set "AUTANA_TOOLS=%IDF_TOOLS_PATH%"
if not defined AUTANA_TOOLS set "AUTANA_TOOLS=%USERPROFILE%\.espressif"
set "AUTANA_PYTHON="
if defined IDF_PYTHON_ENV_PATH if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" set "AUTANA_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
if not defined AUTANA_PYTHON (
    set "AUTANA_BEST="
    for /f "delims=" %%e in ('dir /b /ad "%AUTANA_TOOLS%\python_env\idf*_env" 2^>nul') do (
        if exist "%AUTANA_TOOLS%\python_env\%%e\Scripts\python.exe" (
            for /f "tokens=1,2 delims=._" %%a in ("%%e") do (
                set "AUTANA_MAJ=000%%a"
                set "AUTANA_MAJ=!AUTANA_MAJ:idf=!"
                set "AUTANA_MIN=000%%b"
                set "AUTANA_KEY=!AUTANA_MAJ:~-3!!AUTANA_MIN:~-3!"
                if "!AUTANA_KEY!" gtr "!AUTANA_BEST!" (
                    set "AUTANA_BEST=!AUTANA_KEY!"
                    set "AUTANA_PYTHON=%AUTANA_TOOLS%\python_env\%%e\Scripts\python.exe"
                )
            )
        )
    )
)
rem The Store stub answers on PATH but exits instead of running, so prove it
rem runs. Anything found here is short of pyserial, which device.py reports.
if not defined AUTANA_PYTHON (
    python -c "" >nul 2>&1 && set "AUTANA_PYTHON=python"
)
rem Ends delayed expansion before %* is read, so an argument containing ! survives.
endlocal & set "AUTANA_PYTHON=%AUTANA_PYTHON%"
if not defined AUTANA_PYTHON (
    echo autana: no Python found - install ESP-IDF, or see docs/tools/Autana-CLI.md 1>&2
    exit /b 1
)
"%AUTANA_PYTHON%" "%~dp0..\scripts\autana\autana.py" %*
