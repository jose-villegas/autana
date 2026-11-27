@echo off
rem autana - the terminal command, for cmd.exe and PowerShell. See ./autana.
setlocal
set /p python_min_version=<"%~dp0..\scripts\lib\python-min-version"
set "tools_root=%IDF_TOOLS_PATH%"
if not defined tools_root set "tools_root=%USERPROFILE%\.espressif"
set "python_command="
rem The minor pinned in .python-version, found on PATH by name: the name says
rem its version, so it is taken without being started (each start costs a
rem second on a loaded machine).
set "python_pin="
set "python_minor="
if exist "%~dp0..\.python-version" set /p python_pin=<"%~dp0..\.python-version"
if defined python_pin for /f "tokens=1,2 delims=." %%a in ("%python_pin%") do set "python_minor=%%a.%%b"
if defined python_minor for %%p in ("python%python_minor%.exe") do set "python_command=%%~$PATH:p"
if defined python_command set python_command="%python_command%"
if not defined python_command call :probe python
if not defined python_command for /d %%e in ("%tools_root%\python_env\idf*_env") do (
    if not defined python_command if exist "%%e\Scripts\python.exe" call :probe "%%e\Scripts\python.exe"
)
if not defined python_command call :probe py -3
if not defined python_command (
    echo autana: no Python ^>= %python_min_version% found. Install Python ^>= %python_min_version%, the py launcher or ESP-IDF. Found: 1>&2
    call :report python
    for /d %%e in ("%tools_root%\python_env\idf*_env") do (
        if exist "%%e\Scripts\python.exe" call :report "%%e\Scripts\python.exe"
    )
    call :report py -3
    exit /b 1
)
%python_command% "%~dp0..\scripts\autana\autana.py" %*
exit /b %errorlevel%

:probe
%* -c "import sys; m=tuple(map(int, sys.argv[1].split('.'))); v=sys.version_info[:2]; sys.exit(v < m)" %python_min_version% >nul 2>&1
if not errorlevel 1 set "python_command=%*"
exit /b

:report
echo [%*] 1>&2
%* -c "import sys; print('.'.join(map(str, sys.version_info[:2])))" 1>&2 2>nul
exit /b
