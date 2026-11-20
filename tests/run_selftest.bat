@echo off
rem Usage: run_selftest.bat [path\to\YangTreeView.dll] [path\to\fakehost.exe]
rem Loads the plugin through the real Notepad++ plugin ABI with a fake host.
setlocal
set HERE=%~dp0
set DLL=%~1
if "%DLL%"=="" set DLL=%HERE%..\build\Release\YangTreeView.dll
set HOST=%~2
if "%HOST%"=="" set HOST=%HERE%..\build\Release\fakehost.exe
set CFG=%TEMP%\yangtree-selftest
if exist "%CFG%" rmdir /s /q "%CFG%"
mkdir "%CFG%"
"%HOST%" "%DLL%" "%HERE%sample\acme-olt.yang" "%CFG%"
exit /b %ERRORLEVEL%
