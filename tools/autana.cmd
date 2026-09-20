@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana.
set "AUTANA_PYTHON=%USERPROFILE%\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
if not exist "%AUTANA_PYTHON%" set "AUTANA_PYTHON=python"
"%AUTANA_PYTHON%" "%~dp0..\scripts\autana\autana.py" %*
