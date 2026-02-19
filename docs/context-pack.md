# Snip-Lite — Context Pack

## 0) Repo & exacte versie
- Repo: https://github.com/WillemStapper/snip-lite
- Branch: main
## commit invullen met: git rev-parse --short HEAD
- Commit: da34124

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
  - Mode=0/1/2/3
- Sectie `[Preview]`
  - X=, Y=, W=, H=

## 8) Bekende quirks / bugs
- (optioneel) Als Edit soms een oud bestand opent: maak “Edit” altijd gebaseerd op **huidige capture** (temp-bestand), niet op `LastSavedFile`.

## 9) Volgende stap (exact)
- ===>> 5 (voorstel)
  - [X] “Edit” altijd huidige selectie openen (temp-file) + optioneel temp cleanup
  - [X] PNG export (WIC) als alternatief voor BMP
  - [X] Window/Monitor mode implementeren
- ===>> 7
	- [ ] Freestyle smoothing
	- [ ] Window picking verfijnen
	- [ ] Hotkeys + instellingen UI ??
	- [ ] Overlay in kleur aanpassen
	- [ ] Pointer duidelijker in beeld brengen

## 10) Open vragen
- Welke standaard editor wil je als fallback (als EditorExe leeg is)?

## 11) Mini-log (laatste wijzigingen)
- <19-02-2026>: Window/Monitor mode implementeren
- <19-02-2026>: Freestyle (Lasso) toegevoegd
- <19-02-2026>: PNG export via WIC toegevoegd plus keuze in JPG en BMP
- <19-02-2026>: Edit opent nu altijd huidige selectie (temp-file)
- 
- <18-02-2026>: Preview is niet meer “topmost” na menu-acties
- <18-02-2026>: Cursor verandert in “handje” in preview (knoppen + sleepgebied)
- <18-02-2026>: Preview positie/grootte wordt nu onthouden tussen sessies
- <18-02-2026>: Program picker start nu standaard in Program Files (x86)
