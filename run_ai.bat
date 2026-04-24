@echo off
rem Launch OpenXcom with the AI Bridge TCP server enabled.
rem Extra args are forwarded to openxcom.exe.

set PORT=12345
set EXE=build\bin\Release\openxcom.exe
if not exist "%EXE%" set EXE=build\bin\Debug\openxcom.exe
if not exist "%EXE%" set EXE=bin\x64\Release\OpenXcom.exe
if not exist "%EXE%" (
  echo ERROR: openxcom.exe not found under build\bin\{Release,Debug} or bin\x64\Release.
  echo Build the game first ^(see README^), or edit EXE= in this script.
  exit /b 1
)

"%EXE%" -ai-server %PORT% %*
