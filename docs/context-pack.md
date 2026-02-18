# Snip-Lite — Context Pack

## 0) Repo & exacte versie
- Repo: https://github.com/WillemStapper/snip-lite
- Branch: main
## commit invullen met: git rev-parse --short HEAD
- Commit: 9161de6

## 1) Doel in 1 zin
- Snip-tool zonder “grote GUI”: hotkey → selectie → preview → save/edit/clipboard.

## 2) Wat werkt nu (huidige functionaliteit)
- [x] Hotkey: Ctrl+Alt+S toggles overlay
- [x] Overlay: crosshair + mode-tekst
- [x] RMB overlay: mode menu (Region/Window/Monitor) + onthouden
- [x] Region selectie: slepen → rechthoek → capture
- [x] Clipboard: plakken werkt in Photoshop CS6 (niet verticaal geflipt)
- [x] Preview: toont knipsel + knoppen Save / Edit / Dismiss
- [x] Preview RMB menu’s:
  - Save: open folder, choose folder, auto-dismiss toggle
  - Edit: “Open in (last program)”, “Choose program…”
- [x] Preview slepen: venster verplaatsen door overal te klikken (behalve knoppen)
- [x] Cursor: “handje” in preview (knoppen + sleepgebied)
- [x] Preview positie/grootte wordt onthouden tussen sessies
- [x] Program picker start standaard in Program Files (x86)
- [x] Preview “topmost” wordt losgelaten na menu-acties (niet meer vast op voorgrond)
- [x] Settings persistent via `%LOCALAPPDATA%\snip-lite\settings.ini`

## 3) Belangrijke design-keuzes (waarom zo)
- Capture bitmap is **bottom-up** (`biHeight = +h`) om Photoshop CS6 vertical-flip te voorkomen.
- Settings in INI (1 bestand, makkelijk te debuggen en te back-uppen).
- Geen controls/windows “overal”: alles wordt getekend en bediend via mouse + kleine popup menu’s.

## 4) Waar staan de dingen in de code
- `src/main.cpp`
  - Hotkey/message-only window: `MsgProc`, `wWinMain`
  - Overlay: `CreateOverlay`, `OverlayProc`
  - Capture: `CaptureRectToBitmap`, `CopyBitmapToClipboard`
  - Preview: `CreatePreviewWindow`, `PreviewProc`, `LayoutPreview`
  - Save BMP: `SaveBitmapAsBmpFile`
  - Settings: `LoadSettings`, `SaveSettings`, INI helpers
  - Dialogs: `PickFolder`, `PickExe`
  - “Open in”: `OpenInEditor`
  - Preview topmost toggle: `PreviewDropTopmost` (of equivalent)
  - Temp voor Edit (huidige selectie): `g_tempEditFile` + `MakeTempEditPath` (als geïmplementeerd)

## 5) Build & run
- Configure: `cmake --preset <jouw-preset>`
- Build: `cmake --build --preset <jouw-preset>`
- Run: via Visual Studio (Startup Item = snip_lite.exe) of vanuit build map.

## 6) Dependencies / linken (CMake)
- `target_link_libraries(snip_lite PRIVATE user32 gdi32 shell32 ole32)`

## 7) Settings keys (INI)
- Bestand: `%LOCALAPPDATA%\snip-lite\settings.ini`
- Sectie `[General]`
  - SaveDir=
  - AutoDismiss=0/1
  - EditorExe=
  - LastSavedFile=
  - Mode=0/1/2
- Sectie `[Preview]`
  - X=, Y=, W=, H=

## 8) Bekende quirks / bugs
- (optioneel) Als Edit soms een oud bestand opent: maak “Edit” altijd gebaseerd op **huidige capture** (temp-bestand), niet op `LastSavedFile`.

## 9) Volgende stap (exact)
- ===>> 5 (voorstel)
  - [ ] “Edit” altijd huidige selectie openen (temp-file) + optioneel temp cleanup
  - [ ] PNG export (WIC) als alternatief voor BMP
  - [ ] Window/Monitor mode implementeren

## 10) Open vragen
- Welke standaard editor wil je als fallback (als EditorExe leeg is)?
- Moet Save standaard PNG worden zodra WIC er is?

## 11) Mini-log (laatste wijzigingen)
- <datum>: <korte omschrijving>
- <datum>: <korte omschrijving>
