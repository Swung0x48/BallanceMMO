@echo off
set type=%1
copy _deps\yaml-cpp-build\%type%\*.dll %type%\ /Y