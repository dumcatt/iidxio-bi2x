@echo off
cd /d "%~dp0"
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
C:\msys64\usr\bin\bash.exe -lc "./build_msys2.sh"
