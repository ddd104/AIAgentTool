@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "NODE_EXE=%SCRIPT_DIR%..\.runtime\node\node.exe"

if not exist "%NODE_EXE%" (
  echo Missing local Node runtime: %NODE_EXE% 1>&2
  exit /b 1
)

cd /d "%SCRIPT_DIR%"
"%NODE_EXE%" "%SCRIPT_DIR%server.js"
