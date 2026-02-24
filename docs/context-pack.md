# Snip-Lite — Context Pack (Snapshot)

## 0) Repo & exact snapshot
- Repo: https://github.com/WillemStapper/snip-lite
- Branch: main
- Commit: 8f15709
- Snapshot date: 2026-02-24

## 1) One-line goal
A fast snipping tool without a “big GUI”: hotkey/tray → selection overlay → preview → save/edit/clipboard.

## 2) Current UX flow
- App runs in the background (no main window).
- Start capture:
  - Hotkey: `Ctrl+Alt+S` toggles the overlay
  - Tray:
    - Double-click → capture using **last mode**
    - Right-click → choose mode and other settings, or exit
- In overlay:
  - Right-click → mode menu
  - `Esc` → cancel / close overlay
- After capture:
  - Copy to clipboard
  - Show preview window with **Save / Edit / Dismiss**

## 3) Implemented modes
- Region (drag rectangle)
- Window (hover highlight + click)
- Monitor (hover highlight + click)
- Freestyle / Lasso (draw shape; outside becomes transparent)
- Polygon (click points; double-click or click first to close; outside becomes transparent)
- 
## 4) Save & Edit behavior
- Save:
  - Formats: PNG / JPEG / BMP
  - Freestyle always saves PNG (transparency)
  - Right-click Save: format, open folder, choose folder, auto-dismiss toggle
- Edit:
  - Always opens the **current capture** using a **temp file**
  - Right-click Edit: open in last program / choose program

## 5) Persistence (INI)
- `%LOCALAPPDATA%\snip-lite\settings.ini`
- Stores: save folder, format, auto-dismiss, editor path, last saved file, last mode

## 6) Key code areas (main.cpp)
- Message-only window + hotkey: `MsgProc`, `wWinMain`
- Tray icon + tray menu: `TrayAdd`, `TrayShowMenu`, `TryModeFromTrayCmd`
- Overlay window: `CreateOverlay`, `OverlayProc`
- Hover picking: window/monitor hit-testing + hover drawing
- Capture:
  - Region/window/monitor: capture rect → clipboard → preview
  - Freestyle: lasso point collection → alpha mask → clipboard → preview
- Clipboard:
  - Opaque capture
  - Alpha (freestyle) capture
- Preview window: create/layout/buttons/context menus
- Save:
  - BMP writer
  - PNG/JPEG via WIC (Windows Imaging Component)
- Settings:
  - `LoadSettings`, `SaveSettings` + INI helpers

## 7) Build
- CMake preset: `vs2026-x64`
- Build presets: `debug`, `release`
- Output EXE in `build/vs2026-x64/<Debug|Release>/snip_lite.exe`

## 8) Known issues / TODOs
- Packaging not done yet:
  - Portable ZIP release
  - Installer (setup) release

## 9) Next milestone: v1.0.0 release work
- Produce Release build artifacts:
  - ZIP (portable)
  - Installer
- Create a simple release webpage (later hosted)
