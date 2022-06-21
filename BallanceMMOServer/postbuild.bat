@echo off
set type=%1
copy _deps\yaml-cpp-build\%type%\*.dll %type%\ /Y
copy bmmo_loop.ps1 %type%\ /Y
copy start_ballancemmo_loop.bat %type%\ /Y