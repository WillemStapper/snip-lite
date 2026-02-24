
# Snip-Lite
A minimal snipping tool for Windows: **hotkey → select → preview → save / edit / clipboard**.
No “big GUI”: just a full-screen overlay, a small preview window, and context menus.

## What it does
- **Hotkey:** `Ctrl + Alt + S` toggles the capture overlay.
  - If the hotkey is already taken, Snip-Lite still works via the **tray icon**.
- **System tray:**
  - **Double-click** the tray icon → start capture with the **last used mode**
  - **Right-click** the tray icon → pick a capture mode or exit
- **Overlay:**
  - Crosshair cursor (drawn by Snip-Lite)
  - Current mode text (top-left)
  - **Right-click** → mode menu
  - `Esc` → cancel / close

## Capture modes
- **Region**: click + drag → rectangle → capture
- **Window**: hover highlights a window → click → capture that window
- **Monitor**: hover highlights a monitor → click → capture that monitor
- **Freestyle (Lasso)**: hold left mouse button and draw a shape → release → capture
  - Outside the lasso becomes **transparent** (alpha)
- **Polygon**: click to create points → double-click or click the first point to close → capture
  - Outside the polygon becomes **transparent** (alpha)

## After capture
- Capture is copied to the **clipboard**
- A **preview window** opens with buttons:
  - **Save**
  - **Edit**
  - **Dismiss**
- You can drag the preview window by clicking and dragging anywhere (except the buttons)

## Save (format + folder)
- **Default format:** PNG
- **Left-click Save**
  - Region / Window / Monitor: saves using the **last selected format** (PNG / JPEG / BMP)
  - Freestyle / Polygon: always saves **PNG** (because it can contain transparency)
- **Right-click Save** (no dialog)
  - **Save format → PNG / JPEG / BMP**
  - **Open capture folder**
  - **Choose capture folder…**
  - **Auto-dismiss after Save** (closes the preview after saving)

## Edit
- **Left-click Edit:** opens the **current capture** via a **temp file** (so you never edit an older file by accident).
- **Right-click Edit:**
  - **Open in (last program)**
  - **Choose program…** (pick a fixed editor EXE)

## Settings (persistent)
File:
- `%LOCALAPPDATA%\snip-lite\settings.ini`
Keys:
`[General]`
- `SaveDir=...`
- `SaveFormat=0/1/2`  (0=PNG, 1=JPEG, 2=BMP)
- `AutoDismiss=0/1`
- `EditorExe=...`
- `LastSavedFile=...`
- `Mode=0/1/2/3` (0=Region, 1=Window, 2=Monitor, 3=Freestyle)
Temp files:
- `%LOCALAPPDATA%\snip-lite\tmp\` (used for “Edit”)
- Settings are loaded at startup and saved on changes (e.g. when you change the mode or save a capture).


## Build & run
### Requirements
- Windows 10/11
- Visual Studio with **Desktop development with C++**
- CMake **3.23+**

### Build (PowerShell)
```powershell
# Configure
cmake --preset vs2026-x64

# Build Debug
cmake --build --preset debug

# Build Release
cmake --build --preset release
```

### Output
- `build/vs2026-x64/Debug/snip_lite.exe`
- `build/vs2026-x64/Release/snip_lite.exe`

### Run
- Start `snip_lite.exe`
- You should see a tray icon
- Exit: **right-click tray icon → Exit**
