@echo off
powershell.exe /c "Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass; .\bmmo_loop.ps1 %*"