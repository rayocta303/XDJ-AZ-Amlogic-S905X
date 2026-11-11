@echo off
rem snap.cmd — Fast 1-click snapshot & log collector for XDJ-AZ
setlocal

set SCRIPT_DIR=%~dp0
python "%SCRIPT_DIR%capture_sample.py" %*

endlocal

