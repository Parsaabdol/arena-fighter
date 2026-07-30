@echo off
REM Double-click this to open the hero importer window.
setlocal
cd /d "%~dp0.."

where pythonw >nul 2>nul
if not errorlevel 1 (
    start "" pythonw "tools\hero_importer.py"
    exit /b 0
)

where python >nul 2>nul
if errorlevel 1 (
    echo.
    echo Python was not found.
    echo.
    echo Install it from https://www.python.org/downloads/ and tick
    echo "Add python.exe to PATH" in the installer, then run this again.
    echo.
    pause
    exit /b 1
)

python "tools\hero_importer.py"
