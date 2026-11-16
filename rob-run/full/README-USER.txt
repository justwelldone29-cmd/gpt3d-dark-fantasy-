ROB RUN internal + Silent Aim (experimental)
Package: RobRunLauncher.exe, RobRunInternal.dll, README.txt.
Keep EXE and DLL in the same directory.
Start the game, then launch RobRunLauncher.exe.
The launcher auto-detects one ROB RUN.exe; use --pid otherwise.
Insert opens/closes the menu. Open Silent Aim for Enable, FOV, Distance,
runtime status, candidate count, and redirect count.
Silent Aim is disabled by default. Keep the launcher running.
The launcher neither starts the game nor installs a driver.
Process protection may reject attachment. No protection bypass is included.
Target GameAssembly.dll SHA-256:
03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab
A successful build does not confirm in-game behavior. Redirects are not
confirmed hits. Server acceptance and ban avoidance are unverified.
The original project documentation reports server bans.
Existing Aim, Noclip, Speedhack, and Config controls remain placeholders.
Closing the launcher removes its Silent Aim hooks. The menu DLL stays
loaded until the game exits; do not manually unload it.
