@echo off
chcp 65001 >nul
title S3 Sense Studio - Browser Flasher
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1"
