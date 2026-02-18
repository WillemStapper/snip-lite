# snip-lite (MVP)

Doel (MVP):
- Ctrl+Alt+S: overlay aan/uit
- Crosshair cursor
- Mode-tekst linksboven
- Rechtermuisknop: mode-menu
- ESC sluit overlay

Build (PowerShell):
- cmake --preset vs2026-x64
- cmake --build --preset debug

Run:
- build/vs2026-x64/Debug/snip_lite.exe

Known quirks:
Photoshop CS6: capture bitmap moet bottom-up (biHeight positief), anders vertical flip.

Eerste bruikbare versie: 18 februari 2026

wensen:
aanpassen van overlay in menu (kleur, grootte, ...)
in meerdere formaten kunnen bewaren (png, jpg, bmp, ...)
resize preview
bij keuze edit-programma, daarin direct openen

