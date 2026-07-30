@echo off
REM Double-click this to build a Dota hero into a skin. No typing beyond the
REM hero's name; export_hero.py does the work and explains itself as it goes.
setlocal
cd /d "%~dp0.."

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

echo.
echo   Arena Fighter -- Dota hero export
echo   ---------------------------------
echo.
echo   Type a hero's name, or LIST to see them all.
echo   Names are Valve's internal ones: witchdoctor, ogre_magi, queenofpain.
echo.

:ask
set "HERO="
set /p "HERO=  hero: "
if /i "%HERO%"=="" goto ask
if /i "%HERO%"=="list" (
    python tools\export_hero.py --list
    echo.
    goto ask
)

echo.
python tools\export_hero.py %HERO%
if errorlevel 1 (
    echo.
    echo   That did not work. If the name was wrong, type LIST to see them all.
    echo.
    goto ask
)

echo.
echo   Start ArenaFighter.exe and pick it under Customize / Mod Skins.
echo.
pause
