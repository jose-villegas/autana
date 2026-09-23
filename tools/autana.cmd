@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana,
rem which resolves the same interpreter and says why it has to be ESP-IDF's.
setlocal
set "AUTANA_TOOLS=%IDF_TOOLS_PATH%"
if not defined AUTANA_TOOLS set "AUTANA_TOOLS=%USERPROFILE%\.espressif"
set "AUTANA_PYTHON="
if defined IDF_PYTHON_ENV_PATH if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" set "AUTANA_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
if not defined AUTANA_PYTHON for /f "delims=" %%e in ('dir /b /ad /o-n "%AUTANA_TOOLS%\python_env\idf*_env" 2^>nul') do if not defined AUTANA_PYTHON if exist "%AUTANA_TOOLS%\python_env\%%e\Scripts\python.exe" set "AUTANA_PYTHON=%AUTANA_TOOLS%\python_env\%%e\Scripts\python.exe"
if not defined AUTANA_PYTHON set "AUTANA_PYTHON=python"
"%AUTANA_PYTHON%" "%~dp0..\scripts\autana\autana.py" %*
