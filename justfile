set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-Command"]

# Build the Windows x64 release and package it in dist/.
dist:
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./scripts/package.ps1
