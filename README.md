# snip-lite

Minimalistische snip-tool voor Windows: **hotkey → selectie → preview → save/edit/clipboard**.  
Geen “grote GUI”; alles gebeurt via een overlay + een kleine preview + popup-menu’s.

## Wat het doet

- **Hotkey:** `Ctrl + Alt + S` toggelt de overlay.
- **Overlay:**
  - Crosshair cursor
  - Mode-tekst linksboven
  - **Rechtsklik**: mode-menu
  - `ESC`: sluiten / annuleren
- **Modes:**
  - **Region**: klik + sleep → rechthoek → capture
  - **Window**: hover highlight → klik → capture van venster
  - **Monitor**: hover highlight → klik → capture van monitor
  - **Freestyle (Lasso)**: teken vorm → loslaten → capture met transparantie buiten de lasso
- **Na capture:**
  - Direct naar **clipboard**
  - **Preview window** met knoppen: **Save / Edit / Dismiss**
  - Preview venster kun je slepen door overal te klikken (behalve op knoppen)

## Save (formaat + folder)

- **Default formaat:** PNG
- **Linksklik op Save**
  - Region/Window/Monitor: bewaart in het **laatst gekozen formaat** (PNG/JPEG/BMP)
  - Freestyle: bewaart **altijd PNG** (vanwege transparantie)
- **Rechtsklik op Save** (geen dialoog)
  - **Save format → PNG / JPEG / BMP**
  - **Open capture folder**
  - **Choose capture folder…**
  - **Auto-dismiss after Save** (preview sluit automatisch na opslaan)

## Edit

- **Linksklik Edit:** opent de huidige capture via een **temp-bestand** (dus nooit per ongeluk een oude file).
- **Rechtsklik Edit:**
  - **Open in (last program)**
  - **Choose program…** (kies vaste editor)

## Settings (persistent)

Bestand:
- `%LOCALAPPDATA%\snip-lite\settings.ini`

Belangrijkste keys:

`[General]`
- `SaveDir=...`
- `SaveFormat=0/1/2`  (0=PNG, 1=JPEG, 2=BMP)
- `AutoDismiss=0/1`
- `EditorExe=...`
- `LastSavedFile=...`
- `Mode=0/1/2/3` (0=Region, 1=Window, 2=Monitor, 3=Freestyle)

`[Preview]`
- `X=`, `Y=`, `W=`, `H=` (positie/grootte preview)

## Build & run

### Vereisten
- Windows 10/11
- Visual Studio met “Desktop development with C++”
- CMake 3.23+

### Build (PowerShell)
```powershell
cmake --preset vs2026-x64
cmake --build --preset debug

