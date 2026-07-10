@echo off
rem ==========================================================================
rem Build the WinSquish installer (build\winsquish-setup.exe) with Inno Setup.
rem
rem Builds build\winsquish.exe first if it is missing, then compiles
rem installer\winsquish.iss. Requires Inno Setup 6.3 or later: it locates
rem ISCC.exe on PATH or under Program Files. Get it from:
rem   https://jrsoftware.org/isdl.php
rem ==========================================================================

setlocal
cd /d "%~dp0"

rem --- ensure the application binary exists -------------------------------
if not exist "..\build\winsquish.exe" (
    echo winsquish.exe not found -- building it first...
    call "..\build.bat"
    if errorlevel 1 exit /b 1
)

rem --- locate the Inno Setup compiler ------------------------------------
set "ISCC=ISCC.exe"
where ISCC.exe >nul 2>nul
if not errorlevel 1 goto :compile

set "ISCC="
for %%v in (7 6) do (
    if not defined ISCC if exist "%ProgramFiles%\Inno Setup %%v\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup %%v\ISCC.exe"
    if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup %%v\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup %%v\ISCC.exe"
)
if not defined ISCC (
    echo error: ISCC.exe ^(Inno Setup 6.3+ / 7^) not found on PATH or in Program Files.
    echo Install Inno Setup from https://jrsoftware.org/isdl.php
    exit /b 1
)

:compile
rem --- optional Authenticode signing (Azure Trusted Signing) -------------
rem Mirrors build.bat: enabled when sign-metadata.json exists. Inno then
rem signs both setup.exe and the uninstaller via the /Swinsquish sign tool.
if not exist "..\sign-metadata.json" goto :unsigned

set "DLIB=%LOCALAPPDATA%\Microsoft.Trusted.Signing.Client\bin\x64\Azure.CodeSigning.Dlib.dll"
if not exist "%DLIB%" (
    echo error: sign-metadata.json present but the Trusted Signing client is
    echo missing. Expected: %DLIB%
    exit /b 1
)

set "SIGNTOOL=signtool.exe"
where signtool.exe >nul 2>nul
if not errorlevel 1 goto :havesigntool
set "SIGNTOOL="
for /d %%d in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do if exist "%%d\x64\signtool.exe" set "SIGNTOOL=%%d\x64\signtool.exe"
if not defined SIGNTOOL (
    echo error: signtool.exe not found ^(install a Windows 10/11 SDK^).
    exit /b 1
)
:havesigntool

rem Absolute path so signtool finds the metadata regardless of its cwd.
for %%i in ("..\sign-metadata.json") do set "SIGNMETA=%%~fi"

rem $q -> a literal quote, $f -> the file to sign (Inno fills both in).
"%ISCC%" /Qp /DSIGN "/Swinsquish=$q%SIGNTOOL%$q sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib $q%DLIB%$q /dmdf $q%SIGNMETA%$q $f" "winsquish.iss"
if errorlevel 1 (
    echo error: Inno Setup compilation/signing failed. Are you logged in? Try: az login
    exit /b 1
)
echo.
echo Built and signed installer: build\winsquish-setup.exe
goto :eof

:unsigned
"%ISCC%" /Qp "winsquish.iss"
if errorlevel 1 (
    echo error: Inno Setup compilation failed.
    exit /b 1
)
echo.
echo Built installer: build\winsquish-setup.exe ^(unsigned; create sign-metadata.json to sign^)
