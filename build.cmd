@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "LOGDIR=%CD%\build-logs"
set "MASTERLOG=%LOGDIR%\build-latest.log"
set "CONFIGLOG=%LOGDIR%\configure-latest.log"
set "COMPILELOG=%LOGDIR%\compile-latest.log"
set "DISTDIR=%CD%\dist"
set "BUILDDIR=%CD%\build"

if not exist "%LOGDIR%" mkdir "%LOGDIR%" >nul 2>&1
>"%MASTERLOG%" echo Entropy Backup build log
>>"%MASTERLOG%" echo Started: %date% %time%
>>"%MASTERLOG%" echo Working directory: %CD%
>>"%MASTERLOG%" echo Computer: %COMPUTERNAME%
>>"%MASTERLOG%" echo Processor architecture: %PROCESSOR_ARCHITECTURE%
>>"%MASTERLOG%" echo.

echo.
echo ============================================================
echo   Entropy Backup - Windows 11 x64 Builder v1.9.0
echo   Compile verification + persistent diagnostic logs
echo ============================================================
echo.

call :log "[0/6] Checking prerequisites..."
where cmake >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :no_cmake
cmake --version >>"%MASTERLOG%" 2>&1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL goto :no_msvc

call :log "[1/6] Loading Visual Studio C++ x64 environment..."
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :vsdev_failed
where cl >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :no_msvc
cl 2>>"%MASTERLOG" 1>>"%MASTERLOG%"

call :log "[2/6] Cleaning previous generated build output..."
if exist "%BUILDDIR%" rmdir /s /q "%BUILDDIR%" >>"%MASTERLOG%" 2>&1
if exist "%DISTDIR%\EntropyBackup.exe" del /q "%DISTDIR%\EntropyBackup.exe" >>"%MASTERLOG%" 2>&1
mkdir "%BUILDDIR%" >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :clean_failed

call :log "[3/6] Configuring CMake for Visual Studio 2022 x64..."
>"%CONFIGLOG%" echo Entropy Backup CMake configure output - %date% %time%
cmake -S "%CD%" -B "%BUILDDIR%" -G "Visual Studio 17 2022" -A x64 >>"%CONFIGLOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%CONFIGLOG%" >>"%MASTERLOG%"
if not "%RC%"=="0" goto :configure_failed
call :log "      Configure completed successfully."

call :log "[4/6] Compiling and linking Release x64..."
>"%COMPILELOG%" echo Entropy Backup compiler/linker output - %date% %time%
cmake --build "%BUILDDIR%" --config Release --parallel >>"%COMPILELOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%COMPILELOG%" >>"%MASTERLOG%"
if not "%RC%"=="0" goto :compile_failed
call :log "      Compiler returned exit code 0."

call :log "[5/6] Verifying compiled executable..."
set "BUILT_EXE=%BUILDDIR%\Release\EntropyBackup.exe"
set "BUILT_CLI=%BUILDDIR%\Release\EntropyBackupCLI.exe"
if not exist "%BUILT_EXE%" goto :missing_exe
if not exist "%BUILT_CLI%" goto :missing_exe
for %%F in ("%BUILT_EXE%") do set "EXESIZE=%%~zF"
if "%EXESIZE%"=="0" goto :missing_exe
call :log "      Verified: %BUILT_EXE%"
call :log "      Executable size: %EXESIZE% bytes"

call :log "[6/6] Creating distribution output..."
if not exist "%DISTDIR%" mkdir "%DISTDIR%" >>"%MASTERLOG%" 2>&1
copy /y "%BUILT_EXE%" "%DISTDIR%\EntropyBackup.exe" >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :copy_failed
if not exist "%DISTDIR%\EntropyBackup.exe" goto :copy_failed
copy /y "%BUILT_CLI%" "%DISTDIR%\EntropyBackupCLI.exe" >>"%MASTERLOG%" 2>&1
if errorlevel 1 goto :copy_failed

call :log ""
call :log "================ BUILD SUCCESS ================"
call :log "Verified EXE: %DISTDIR%\EntropyBackup.exe"
call :log "Command line: %DISTDIR%\EntropyBackupCLI.exe"
call :log "Finished: %date% %time%"
call :log "==============================================="
copy /y "%MASTERLOG%" "%LOGDIR%\build-success.log" >nul 2>&1

echo.
echo ============================================================
echo   BUILD SUCCESS
 echo ============================================================
echo.
echo   %DISTDIR%\EntropyBackup.exe
echo   %DISTDIR%\EntropyBackupCLI.exe
echo.
echo The EXE was verified to exist after the compiler finished.
echo Full log: %MASTERLOG%
echo.
start "" explorer.exe /select,"%DISTDIR%\EntropyBackup.exe"
pause
exit /b 0

:no_cmake
set "FAILSTAGE=Prerequisite check - CMake not found"
goto :fail
:no_msvc
set "FAILSTAGE=Prerequisite check - MSVC C++ x64 tools not found"
goto :fail
:vsdev_failed
set "FAILSTAGE=Visual Studio developer environment initialization"
goto :fail
:clean_failed
set "FAILSTAGE=Cleaning or creating build directory"
goto :fail
:configure_failed
set "FAILSTAGE=CMake configuration"
goto :fail
:compile_failed
set "FAILSTAGE=C++ compilation or linking"
goto :fail
:missing_exe
set "FAILSTAGE=Output verification - compiler returned success but EXE is missing or empty"
goto :fail
:copy_failed
set "FAILSTAGE=Copying verified executable to dist"
goto :fail

:fail
call :log ""
call :log "================ BUILD FAILED ================="
call :log "Failed stage: %FAILSTAGE%"
call :log "Exit code: %RC%"
call :log "Time: %date% %time%"
call :log "==============================================="
copy /y "%MASTERLOG%" "%LOGDIR%\build-failed.log" >nul 2>&1
echo.
echo ============================================================
echo   BUILD FAILED
 echo ============================================================
echo   Stage: %FAILSTAGE%
echo.
echo   Master log:   %MASTERLOG%
echo   Configure:    %CONFIGLOG%
echo   Compiler:     %COMPILELOG%
echo.
start "" explorer.exe "%LOGDIR%"
pause
exit /b 1

:log
echo %~1
>>"%MASTERLOG%" echo %~1
exit /b 0
