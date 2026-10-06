@echo off
rem ============================================================
rem  shya compiler build script (MSVC / cl.exe)
rem  usage:  build.bat            -> build\shya.exe (Release)
rem          build.bat debug      -> build\shya.exe (Debug, no opt)
rem  Requires Visual Studio 2022 with the C++ workload.
rem ============================================================
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not exist "%VCVARS%" (
  echo [build] ERROR: vcvars64.bat not found. Install the "Desktop development with C++" workload.
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 ( echo [build] ERROR: vcvars64.bat failed & exit /b 1 )

set "ROOT=%~dp0"
if not exist "%ROOT%build" mkdir "%ROOT%build"

set "CFGFLAGS=/std:c++20 /EHsc /utf-8 /nologo /W4 /wd4100 /wd4189 /D_CRT_SECURE_NO_WARNINGS"
if /I "%~1"=="debug" (
  set "CFGFLAGS=%CFGFLAGS% /Zi /Od /DDEBUG"
) else (
  set "CFGFLAGS=%CFGFLAGS% /O2 /DNDEBUG"
)

rem /WX would make warnings fatal; kept off so a warning never blocks a build.
cl %CFGFLAGS% /Fo"%ROOT%build\\" /Fd"%ROOT%build\\" /Fe"%ROOT%build\shya.exe" ^
   "%ROOT%src\main.cpp" "%ROOT%src\diag.cpp" "%ROOT%src\lexer.cpp" "%ROOT%src\parser.cpp" ^
   "%ROOT%src\macro.cpp" "%ROOT%src\modules.cpp" "%ROOT%src\typecheck.cpp" ^
   "%ROOT%src\codegen.cpp" "%ROOT%src\stdlib.cpp" ^
   /link /STACK:8388608
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )

echo [build] OK -^> %ROOT%build\shya.exe
endlocal
