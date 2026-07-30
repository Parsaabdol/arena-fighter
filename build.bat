@echo off
REM Build Arena Fighter with MSVC. Run this from anywhere; paths are absolute.
setlocal
set ROOT=%~dp0
set RAYLIB=%ROOT%vendor\raylib-6.0_win64_msvc16

REM Load the MSVC environment only once per shell (it is slow).
if not defined VCINSTALLDIR (
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
)

REM Build from inside build\ so every intermediate (.obj, linker leftovers)
REM lands there, but link the executable straight out to the source root. There
REM is exactly one ArenaFighter.exe and it is the one you run -- an extra copy
REM in build\ would get its own settings.dat next to it, because the game saves
REM beside whichever executable is running.
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"

REM /O2  maximum speed        /GL + /LTCG  whole-program optimisation
REM /fp:fast relaxed float    /W4 high warning level
REM /MD  dynamic CRT -- MUST match the prebuilt raylib.lib, which was compiled
REM      with /DEFAULTLIB:MSVCRT (verified with `dumpbin /directives raylib.lib`).
REM      Mismatching this is the classic "unresolved __imp_*" link error.
cl /nologo /std:c17 /O2 /GL /fp:fast /MD /W4 /wd4996 ^
   /I"%RAYLIB%\include" /I"%ROOT%vendor\cgltf" ^
   "%ROOT%src\*.c" ^
   /Fe:"%ROOT%ArenaFighter.exe" ^
   /link /LTCG /INCREMENTAL:NO /LIBPATH:"%RAYLIB%\lib" ^
   /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
   raylib.lib opengl32.lib gdi32.lib winmm.lib user32.lib shell32.lib kernel32.lib

set ERR=%ERRORLEVEL%
if not %ERR%==0 goto :done

REM SkinPreview.exe -- the importer in tools/ shells out to this to show you a
REM skin without starting the game. It lives in src\preview\ rather than src\
REM so the wildcard above does not pick up its main(), and it deliberately
REM links the SAME model.c and anim_gltf.c: the whole value of it is that the
REM loader deciding how a skin looks is the game's, not a copy of the game's.
REM fighter.c comes along because the clip pacing asks it when a swing lands.
cl /nologo /std:c17 /O2 /fp:fast /MD /W4 /wd4996 ^
   /I"%RAYLIB%\include" /I"%ROOT%vendor\cgltf" /I"%ROOT%src" ^
   "%ROOT%src\preview\preview_main.c" "%ROOT%src\model.c" ^
   "%ROOT%src\anim_gltf.c" "%ROOT%src\fighter.c" ^
   /Fe:"%ROOT%SkinPreview.exe" ^
   /link /INCREMENTAL:NO /LIBPATH:"%RAYLIB%\lib" ^
   /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
   raylib.lib opengl32.lib gdi32.lib winmm.lib user32.lib shell32.lib kernel32.lib

set ERR=%ERRORLEVEL%

:done
popd

if %ERR%==0 (
  echo.
  echo Build OK  -^>  %ROOT%ArenaFighter.exe
  echo           -^>  %ROOT%SkinPreview.exe
) else (echo. & echo BUILD FAILED)
exit /b %ERR%
