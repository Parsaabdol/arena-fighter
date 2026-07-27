@echo off
REM Headless tests for the simulation.
REM
REM src\fighter.c depends on nothing but game.h and math.h -- no raylib, no
REM window -- so the whole combat state machine can be tested from a console.
REM Keep it that way: it is the reason these tests take a second to run.
setlocal
set ROOT=%~dp0..\

if not defined VCINSTALLDIR (
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
)

if not exist "%ROOT%build\tests" mkdir "%ROOT%build\tests"
pushd "%ROOT%build\tests"

cl /nologo /std:c17 /W4 /MD ^
   /I"%ROOT%src" ^
   "%ROOT%tests\test_attack.c" "%ROOT%src\fighter.c" ^
   /Fe:tests.exe

set ERR=%ERRORLEVEL%
if not %ERR%==0 goto done

echo.
REM Explicit .\ -- some environments drop the current directory from the
REM executable search path, and then a freshly built exe "does not exist".
.\tests.exe
set ERR=%ERRORLEVEL%

:done
popd
if %ERR%==0 (echo. & echo Tests OK) else (echo. & echo TESTS FAILED)
exit /b %ERR%
