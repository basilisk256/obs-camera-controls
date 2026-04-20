#Requires -RunAsAdministrator
# Copy latest build to Program Files — this OBS install only scans
# `C:\Program Files\obs-studio\obs-plugins\64bit\`, not the per-user
# paths. Verified against this machine's OBS logs 2026-04-20.
$src = "C:\Users\griff\dev\obs-shot-presets\build\Release\obs-shot-presets.dll"
$dst = "C:\Program Files\obs-studio\obs-plugins\64bit\obs-shot-presets.dll"
Copy-Item -Path $src -Destination $dst -Force
Write-Host "Installed: $dst"
