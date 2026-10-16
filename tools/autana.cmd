@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana.
setlocal
set /p python_min_version=<"%~dp0..\scripts\lib\python-min-version"
for /f "tokens=1,2 delims=." %%a in ("%python_min_version%") do (
    set "minimum_major=%%a"
    set "minimum_minor=%%b"
)
set "tools_root=%IDF_TOOLS_PATH%"
if not defined tools_root set "tools_root=%USERPROFILE%\.espressif"
set "python_command="
set "python_found="
call :probe python
if not defined python_command call :probe py -3
if not defined python_command for /d %%e in ("%tools_root%\python_env\idf*_env") do (
    if not defined python_command if exist "%%e\Scripts\python.exe" call :probe "%%e\Scripts\python.exe"
)
if not defined python_command (
    echo autana: no Python ^>= %python_min_version% found. Install Python ^>= %python_min_version%, the py launcher or ESP-IDF. Found: %python_found% 1>&2
    exit /b 1
)
call %python_command% "%~dp0..\scripts\autana\autana.py" %*
exit /b %errorlevel%

:probe
set "candidate_version="
for /f "delims=" %%v in ('call %* -c "import sys; print('%%s.%%s' %% sys.version_info[:2])" 2^>nul') do set "candidate_version=%%v"
if not defined candidate_version (
    set "python_found=%python_found% [%*: unavailable]"
    exit /b
)
set "python_found=%python_found% [%*: %candidate_version%]"
for /f "tokens=1,2 delims=." %%a in ("%candidate_version%") do (
    if %%a GTR %minimum_major% set "python_command=%*"
    if %%a EQU %minimum_major% if %%b GEQ %minimum_minor% set "python_command=%*"
)
exit /b
