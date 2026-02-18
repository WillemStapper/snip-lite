// snip-lite main.cpp
// ===>> 3: Preview (Save/Edit/Dismiss) na knipsel
//
// Filosofie:
// - Geen hoofdvenster, alleen overlay + kleine popup menu’s.
// - Hotkey start capture, muis doet de rest.
// - Instellingen zijn runtime (blijven zo tot je ze via RMB verandert).


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM
#include <sal.h>          // _In_ annotations
#include <shellapi.h>     // ShellExecuteW
#include <cstring>        // memcpy / memset
#include <string>

// -----------------------------
// Hotkey
// -----------------------------
static constexpr int  HOTKEY_ID  = 1;
static constexpr UINT HOTKEY_MOD = MOD_CONTROL | MOD_ALT;
static constexpr UINT HOTKEY_VK  = 'S';            // Ctrl+Alt+S

// -----------------------------
// Modes
// -----------------------------
enum class Mode { Region = 0, Window = 1, Monitor = 2 };
static Mode g_mode = Mode::Region;

static const wchar_t* ModeText(Mode m) {
  switch (m) {
    case Mode::Region:  return L"Mode: Region";
    case Mode::Window:  return L"Mode: Window";
    case Mode::Monitor: return L"Mode: Monitor";
    default:            return L"Mode: ?";
  }
}

// -----------------------------
// Globals (windows)
// -----------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMsg     = nullptr;   // message-only window (hotkey)
static HWND g_hwndOverlay = nullptr;   // capture overlay
static HWND g_hwndPreview = nullptr;   // preview window

// -----------------------------
// Selectie state (overlay)
// -----------------------------
static bool  g_selecting = false;
static bool  g_hasSelection = false;
static POINT g_selStart{};
static POINT g_selCur{};
static RECT  g_selRectClient{};

// -----------------------------
// Capture state (bitmap)
// -----------------------------
static HBITMAP g_captureBmp = nullptr;
static int     g_captureW = 0;
static int     g_captureH = 0;

// -----------------------------
// Preview UI state
// -----------------------------
static RECT g_rcImage{};
static RECT g_btnSave{};
static RECT g_btnEdit{};
static RECT g_btnDismiss{};
static std::wstring g_statusText;      // korte status onderin (Saved/Failed)
static bool g_autoDismissAfterSave = false;

static std::wstring g_saveDir;         // runtime instelling
static std::wstring g_lastSavedFile;   // runtime instelling

static constexpr UINT_PTR TIMER_STATUS_CLEAR = 1;

// -----------------------------
// Kleine helpers
// -----------------------------
static RECT VirtualScreenRect() {
  RECT r{};
  r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
  r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
  r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  return r;
}

static RECT MakeNormalizedRect(POINT a, POINT b) {
  RECT r{};
  r.left   = (a.x < b.x) ? a.x : b.x;
  r.right  = (a.x > b.x) ? a.x : b.x;
  r.top    = (a.y < b.y) ? a.y : b.y;
  r.bottom = (a.y > b.y) ? a.y : b.y;

  if (r.right == r.left)   r.right++;
  if (r.bottom == r.top)   r.bottom++;
  return r;
}

static bool PtInRectEx(const RECT& r, POINT p) {
  return (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom);
}

static void FreeCapture() {
  if (g_captureBmp) {
    DeleteObject(g_captureBmp);
    g_captureBmp = nullptr;
  }
  g_captureW = 0;
  g_captureH = 0;
}

static std::wstring GetEnvW(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return L"";
  std::wstring s;
  s.resize(n);
  GetEnvironmentVariableW(name, s.data(), n);
  if (!s.empty() && s.back() == L'\0') s.pop_back();
  return s;
}

// simpele “maak map”-functie (maakt ook tussenmappen)
static bool EnsureDirectoryRecursive(const std::wstring& path) {
  if (path.size() < 3) return false; // minimaal "C:\"
  std::wstring cur;
  cur.reserve(path.size());

  for (size_t i = 0; i < path.size(); ++i) {
    wchar_t c = path[i];
    cur.push_back(c);

    if (c == L'\\' || c == L'/') {
      // sla "C:\" over
      if (cur.size() <= 3) continue;
      CreateDirectoryW(cur.c_str(), nullptr);
    }
  }

  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  DWORD e = GetLastError();
  return (e == ERROR_ALREADY_EXISTS);
}

static std::wstring DefaultSaveDir() {
  // REMARK: geen shell API’s nodig; we gebruiken USERPROFILE.
  // Meestal bestaat Pictures al.
  std::wstring up = GetEnvW(L"USERPROFILE");
  if (up.empty()) return L".\\captures";

  std::wstring pictures = up + L"\\Pictures";
  std::wstring dir = pictures + L"\\snip-lite";

  // maak Pictures (als nodig) + snip-lite
  EnsureDirectoryRecursive(pictures + L"\\");
  EnsureDirectoryRecursive(dir + L"\\");

  return dir;
}

static std::wstring TimestampedFileNameBmp() {
  SYSTEMTIME st{};
  GetLocalTime(&st);

  wchar_t buf[128]{};
  swprintf_s(buf, L"snip_%04u%02u%02u_%02u%02u%02u_%03u.bmp",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

// -----------------------------
// Capture: screen rect -> 32bpp DIBSection
// IMPORTANT: biHeight = +h (bottom-up) om vertical flip issues te voorkomen (o.a. Photoshop CS6).
// -----------------------------
static bool CaptureRectToBitmap(const RECT& screenRect, HBITMAP& outBmp, int& outW, int& outH) {
  const int w = screenRect.right - screenRect.left;
  const int h = screenRect.bottom - screenRect.top;
  if (w <= 0 || h <= 0) return false;

  HDC hdcScreen = GetDC(nullptr);
  if (!hdcScreen) return false;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = h;          // bottom-up (compatibel)
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP hbmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!hbmp) {
    ReleaseDC(nullptr, hdcScreen);
    return false;
  }

  HDC hdcMem = CreateCompatibleDC(hdcScreen);
  HGDIOBJ old = SelectObject(hdcMem, hbmp);

  const DWORD rop = SRCCOPY | CAPTUREBLT;
  const BOOL ok = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, screenRect.left, screenRect.top, rop);

  SelectObject(hdcMem, old);
  DeleteDC(hdcMem);
  ReleaseDC(nullptr, hdcScreen);

  if (!ok) {
    DeleteObject(hbmp);
    return false;
  }

  outBmp = hbmp;
  outW = w;
  outH = h;
  return true;
}

// -----------------------------
// Clipboard: CF_DIB (bottom-up) + CF_BITMAP
// -----------------------------
static bool CopyBitmapToClipboard(HBITMAP hbmp) {
  if (!hbmp) return false;

  DIBSECTION ds{};
  if (GetObjectW(hbmp, sizeof(ds), &ds) == 0 || ds.dsBm.bmBits == nullptr) {
    return false;
  }

  const int w = ds.dsBmih.biWidth;
  const int absH = (ds.dsBmih.biHeight < 0) ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;

  const int srcStride = ds.dsBm.bmWidthBytes;
  const int dstStride = w * 4; // 32bpp

  const SIZE_T headerSize = sizeof(BITMAPINFOHEADER);
  const SIZE_T bitsSize   = (SIZE_T)dstStride * (SIZE_T)absH;
  const SIZE_T totalSize  = headerSize + bitsSize;

  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalSize);
  if (!hMem) return false;

  BYTE* p = (BYTE*)GlobalLock(hMem);
  if (!p) { GlobalFree(hMem); return false; }

  BITMAPINFOHEADER bih{};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = w;
  bih.biHeight = absH;               // bottom-up
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;
  bih.biSizeImage = (DWORD)bitsSize;

  std::memcpy(p, &bih, sizeof(bih));

  const BYTE* srcBits = (const BYTE*)ds.dsBm.bmBits;
  BYTE* dstBits = p + headerSize;

  const bool srcTopDown = (ds.dsBmih.biHeight < 0);
  const int copyBytes = (srcStride < dstStride) ? srcStride : dstStride;

  // Schrijf bottom-up: bovenste rij komt onderaan in memory.
  for (int y = 0; y < absH; ++y) {
    const int srcY = srcTopDown ? y : (absH - 1 - y);
    const int dstY = (absH - 1 - y);

    const BYTE* srcRow = srcBits + (SIZE_T)srcY * (SIZE_T)srcStride;
    BYTE* dstRow = dstBits + (SIZE_T)dstY * (SIZE_T)dstStride;

    std::memcpy(dstRow, srcRow, (size_t)copyBytes);
    if (copyBytes < dstStride) {
      std::memset(dstRow + copyBytes, 0, (size_t)(dstStride - copyBytes));
    }
  }

  GlobalUnlock(hMem);

  if (!OpenClipboard(nullptr)) {
    GlobalFree(hMem);
    return false;
  }

  EmptyClipboard();

  bool ok = (SetClipboardData(CF_DIB, hMem) != nullptr);
  if (!ok) {
    CloseClipboard();
    GlobalFree(hMem);
    return false;
  }

  // Extra: CF_BITMAP
  HBITMAP copyBmp = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
  if (copyBmp) {
    if (!SetClipboardData(CF_BITMAP, copyBmp)) {
      DeleteObject(copyBmp);
    }
  }

  CloseClipboard();
  return true;
}

// -----------------------------
// Save BMP (simpel, stabiel)
// -----------------------------
static bool SaveBitmapAsBmpFile(HBITMAP hbmp, const std::wstring& filePath) {
  if (!hbmp) return false;

  DIBSECTION ds{};
  if (GetObjectW(hbmp, sizeof(ds), &ds) == 0 || ds.dsBm.bmBits == nullptr) return false;

  const int w = ds.dsBmih.biWidth;
  const int h = (ds.dsBmih.biHeight < 0) ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;
  const bool srcTopDown = (ds.dsBmih.biHeight < 0);

  const DWORD stride = (DWORD)ds.dsBm.bmWidthBytes;
  const DWORD imageSize = stride * (DWORD)h;

  BITMAPFILEHEADER bfh{};
  BITMAPINFOHEADER bih{};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = w;
  bih.biHeight = h;                 // schrijf als bottom-up BMP
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;
  bih.biSizeImage = imageSize;

  bfh.bfType = 0x4D42;              // 'BM'
  bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  bfh.bfSize = bfh.bfOffBits + imageSize;

  HANDLE hf = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hf == INVALID_HANDLE_VALUE) return false;

  DWORD written = 0;
  bool ok = true;

  ok = ok && WriteFile(hf, &bfh, sizeof(bfh), &written, nullptr);
  ok = ok && WriteFile(hf, &bih, sizeof(bih), &written, nullptr);

  const BYTE* bits = (const BYTE*)ds.dsBm.bmBits;

  // BMP (bottom-up) verwacht: eerst onderste scanline.
  // Als onze bron top-down is, draaien we het tijdens schrijven om.
  if (ok) {
    for (int row = 0; row < h; ++row) {
      const int srcRow = srcTopDown ? (h - 1 - row) : row; // bron aanpassen
      const BYTE* pRow = bits + (SIZE_T)srcRow * stride;
      ok = ok && WriteFile(hf, pRow, stride, &written, nullptr);
      if (!ok) break;
    }
  }

  CloseHandle(hf);
  return ok;
}

// -----------------------------
// Mini acties (open file / open folder)
// -----------------------------
static void OpenPath(const std::wstring& path) {
  if (path.empty()) return;
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void SetStatus(HWND hwndPreview, const std::wstring& s) {
  g_statusText = s;
  InvalidateRect(hwndPreview, nullptr, TRUE);
  SetTimer(hwndPreview, TIMER_STATUS_CLEAR, 1500, nullptr);
}

// -----------------------------
// Overlay menu
// -----------------------------
static void ShowModeMenu(HWND hwndOwner) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | (g_mode == Mode::Region  ? MF_CHECKED : 0), 1001, L"Region");
  AppendMenuW(menu, MF_STRING | (g_mode == Mode::Window  ? MF_CHECKED : 0), 1002, L"Window");
  AppendMenuW(menu, MF_STRING | (g_mode == Mode::Monitor ? MF_CHECKED : 0), 1003, L"Monitor");

  POINT pt{};
  GetCursorPos(&pt);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwndOwner, nullptr);
  DestroyMenu(menu);
}

// -----------------------------
// Preview layout
// -----------------------------
static void LayoutPreview(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);

  const int pad = 12;
  const int barH = 52;     // button bar hoogte
  const int btnW = 110;
  const int btnH = 34;
  const int gap  = 10;

  // image area
  g_rcImage = rc;
  g_rcImage.left   += pad;
  g_rcImage.top    += pad;
  g_rcImage.right  -= pad;
  g_rcImage.bottom -= (barH + pad);

  // buttons bottom center
  int totalW = btnW * 3 + gap * 2;
  int x0 = (rc.right - totalW) / 2;
  int y0 = rc.bottom - barH + (barH - btnH) / 2;

  g_btnSave    = { x0,                 y0, x0 + btnW,           y0 + btnH };
  g_btnEdit    = { x0 + btnW + gap,    y0, x0 + 2*btnW + gap,  y0 + btnH };
  g_btnDismiss = { x0 + 2*(btnW+gap),  y0, x0 + 3*btnW + 2*gap,y0 + btnH };
}

static void DestroyPreview() {
  if (g_hwndPreview) {
    DestroyWindow(g_hwndPreview);
    g_hwndPreview = nullptr;
  }
  FreeCapture();
  g_lastSavedFile.clear();
  g_statusText.clear();
}

// -----------------------------
// Preview menus
// -----------------------------
static void ShowSaveMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 2001, L"Open capture folder");
  AppendMenuW(menu, MF_STRING | (g_autoDismissAfterSave ? MF_CHECKED : 0), 2002, L"Auto-dismiss after Save");

  POINT pt{};
  GetCursorPos(&pt);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

static void ShowEditMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 2101, L"Open last saved file");

  POINT pt{};
  GetCursorPos(&pt);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

// -----------------------------
// Preview window proc
// -----------------------------
static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      LayoutPreview(hwnd);
      return 0;

    case WM_SIZE:
      LayoutPreview(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_TIMER:
      if (wParam == TIMER_STATUS_CLEAR) {
        KillTimer(hwnd, TIMER_STATUS_CLEAR);
        g_statusText.clear();
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;

    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        DestroyPreview();
        return 0;
      }
      return 0;

    case WM_LBUTTONUP: {
      POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

      // Save
      if (PtInRectEx(g_btnSave, p)) {
        if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();

        const std::wstring file = g_saveDir + L"\\" + TimestampedFileNameBmp();
        if (SaveBitmapAsBmpFile(g_captureBmp, file)) {
          g_lastSavedFile = file;
          SetStatus(hwnd, L"Saved");
          if (g_autoDismissAfterSave) DestroyPreview();
        } else {
          SetStatus(hwnd, L"Save failed");
        }
        return 0;
      }

      // Edit (open last saved; if none, first save)
      if (PtInRectEx(g_btnEdit, p)) {
        if (g_lastSavedFile.empty()) {
          if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
          const std::wstring file = g_saveDir + L"\\" + TimestampedFileNameBmp();
          if (SaveBitmapAsBmpFile(g_captureBmp, file)) {
            g_lastSavedFile = file;
          } else {
            SetStatus(hwnd, L"Save failed");
            return 0;
          }
        }
        OpenPath(g_lastSavedFile);
        SetStatus(hwnd, L"Opened");
        return 0;
      }

      // Dismiss
      if (PtInRectEx(g_btnDismiss, p)) {
        DestroyPreview();
        return 0;
      }

      return 0;
    }

    case WM_RBUTTONUP: {
      POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      if (PtInRectEx(g_btnSave, p)) { ShowSaveMenu(hwnd); return 0; }
      if (PtInRectEx(g_btnEdit, p)) { ShowEditMenu(hwnd); return 0; }
      return 0;
    }

    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case 2001: { // open capture folder
          if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
          OpenPath(g_saveDir);
          return 0;
        }
        case 2002: { // toggle auto-dismiss
          g_autoDismissAfterSave = !g_autoDismissAfterSave;
          return 0;
        }
        case 2101: { // open last saved
          if (!g_lastSavedFile.empty()) OpenPath(g_lastSavedFile);
          return 0;
        }
      }
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);

      RECT rc{};
      GetClientRect(hwnd, &rc);

      // background
      HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
      FillRect(hdc, &rc, bg);
      DeleteObject(bg);

      // draw image (scaled to fit)
      if (g_captureBmp) {
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, g_captureBmp);

        BITMAP bm{};
        GetObjectW(g_captureBmp, sizeof(bm), &bm);

        // fit to g_rcImage
        int iw = bm.bmWidth;
        int ih = bm.bmHeight;
        int aw = g_rcImage.right - g_rcImage.left;
        int ah = g_rcImage.bottom - g_rcImage.top;

        double sx = (iw > 0) ? (double)aw / (double)iw : 1.0;
        double sy = (ih > 0) ? (double)ah / (double)ih : 1.0;
        double s = (sx < sy) ? sx : sy;
        if (s > 1.0) s = 1.0;

        int dw = (int)(iw * s);
        int dh = (int)(ih * s);

        int dx = g_rcImage.left + (aw - dw) / 2;
        int dy = g_rcImage.top + (ah - dh) / 2;

        SetStretchBltMode(hdc, HALFTONE);
        StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, iw, ih, SRCCOPY);

        SelectObject(mem, old);
        DeleteDC(mem);

        // border around image area
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, g_rcImage.left, g_rcImage.top, g_rcImage.right, g_rcImage.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
      }

      auto drawBtn = [&](const RECT& r, const wchar_t* text) {
        HBRUSH b = CreateSolidBrush(RGB(50, 50, 50));
        FillRect(hdc, &r, b);
        DeleteObject(b);

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        RECT t = r;
        DrawTextW(hdc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      };

      drawBtn(g_btnSave, L"Save");
      drawBtn(g_btnEdit, L"Edit");
      drawBtn(g_btnDismiss, L"Dismiss");

      // status text (klein)
      if (!g_statusText.empty()) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(180, 180, 180));
        RECT tr = rc;
        tr.left += 12;
        tr.right -= 12;
        tr.bottom -= 6;
        tr.top = tr.bottom - 18;
        DrawTextW(hdc, g_statusText.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      }

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_DESTROY:
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------
// Preview window creation
// -----------------------------
static void CreatePreviewWindow() {
  if (g_hwndPreview) return;

  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = PreviewProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"SnipLitePreview";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    registered = true;
  }

  // preview size: max ~70% van work area
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  int maxW = (int)((wa.right - wa.left) * 0.70);
  int maxH = (int)((wa.bottom - wa.top) * 0.70);

  int w = g_captureW;
  int h = g_captureH;
  if (w <= 0 || h <= 0) { w = 600; h = 400; }

  // plus buttonbar + padding
  int winW = w + 24;
  int winH = h + 24 + 52;

  double sx = (winW > 0) ? (double)maxW / (double)winW : 1.0;
  double sy = (winH > 0) ? (double)maxH / (double)winH : 1.0;
  double s = (sx < sy) ? sx : sy;
  if (s > 1.0) s = 1.0;

  winW = (int)(winW * s);
  winH = (int)(winH * s);

  int x = wa.left + ((wa.right - wa.left) - winW) / 2;
  int y = wa.top  + ((wa.bottom - wa.top) - winH) / 2;

  g_hwndPreview = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    L"SnipLitePreview",
    L"",
    WS_POPUP,
    x, y, winW, winH,
    nullptr, nullptr, g_hInst, nullptr
  );

  ShowWindow(g_hwndPreview, SW_SHOW);
  SetForegroundWindow(g_hwndPreview);
  SetFocus(g_hwndPreview);
}

// -----------------------------
// Overlay lifecycle
// -----------------------------
static void DestroyOverlay() {
  g_selecting = false;
  g_hasSelection = false;
  ZeroMemory(&g_selRectClient, sizeof(g_selRectClient));

  if (g_hwndOverlay) {
    DestroyWindow(g_hwndOverlay);
    g_hwndOverlay = nullptr;
  }
}

// -----------------------------
// Overlay proc
// -----------------------------
static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_CROSS));
      return TRUE;

    case WM_RBUTTONUP:
      if (g_selecting) return 0;
      ShowModeMenu(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      g_selecting = true;
      g_hasSelection = false;

      g_selStart = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selCur   = g_selStart;
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_MOUSEMOVE:
      if (!g_selecting) return 0;
      g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_LBUTTONUP: {
      if (!g_selecting) return 0;

      g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);

      g_selecting = false;
      ReleaseCapture();

      const int rw = g_selRectClient.right  - g_selRectClient.left;
      const int rh = g_selRectClient.bottom - g_selRectClient.top;
      g_hasSelection = (rw >= 5 && rh >= 5);

      if (!g_hasSelection) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }

      if (g_mode != Mode::Region) {
        MessageBeep(MB_ICONWARNING);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }

      // client -> screen coords
      RECT wr{};
      GetWindowRect(hwnd, &wr);

      RECT sr{};
      sr.left   = wr.left + g_selRectClient.left;
      sr.top    = wr.top  + g_selRectClient.top;
      sr.right  = wr.left + g_selRectClient.right;
      sr.bottom = wr.top  + g_selRectClient.bottom;

      // overlay verbergen zodat je hem niet mee-captured
      ShowWindow(hwnd, SW_HIDE);
      Sleep(20);
      GdiFlush();

      FreeCapture();
      const bool capOk = CaptureRectToBitmap(sr, g_captureBmp, g_captureW, g_captureH);
      const bool clipOk = capOk ? CopyBitmapToClipboard(g_captureBmp) : false;

      if (clipOk) {
        DestroyOverlay();
        CreatePreviewWindow();   // ===>> 3: hier komt preview
      } else {
        MessageBeep(MB_ICONERROR);
        ShowWindow(hwnd, SW_SHOW);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }

    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        if (g_selecting || g_hasSelection) {
          g_selecting = false;
          g_hasSelection = false;
          ReleaseCapture();
          InvalidateRect(hwnd, nullptr, TRUE);
        } else {
          DestroyOverlay();
        }
        return 0;
      }
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case 1001: g_mode = Mode::Region;  break;
        case 1002: g_mode = Mode::Window;  break;
        case 1003: g_mode = Mode::Monitor; break;
        default: break;
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);

      RECT r{};
      GetClientRect(hwnd, &r);
      HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(hdc, &r, bg);
      DeleteObject(bg);

      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(255, 255, 255));

      RECT tr = r;
      tr.left += 20;
      tr.top  += 20;
      DrawTextW(hdc, ModeText(g_mode), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);

      if (g_selecting || g_hasSelection) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        Rectangle(hdc,
                  g_selRectClient.left, g_selRectClient.top,
                  g_selRectClient.right, g_selRectClient.bottom);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
      }

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_DESTROY:
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------
// Overlay creation
// -----------------------------
static void CreateOverlay() {
  if (g_hwndOverlay) return;

  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"SnipLiteOverlay";
    wc.hCursor       = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    registered = true;
  }

  const RECT vr = VirtualScreenRect();

  g_hwndOverlay = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
    L"SnipLiteOverlay",
    L"",
    WS_POPUP,
    vr.left, vr.top,
    vr.right - vr.left, vr.bottom - vr.top,
    nullptr, nullptr, g_hInst, nullptr
  );

  SetLayeredWindowAttributes(g_hwndOverlay, 0, (BYTE)120, LWA_ALPHA);

  ShowWindow(g_hwndOverlay, SW_SHOW);
  SetForegroundWindow(g_hwndOverlay);
  SetFocus(g_hwndOverlay);
}

// -----------------------------
// Hotkey handler window proc
// -----------------------------
static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_HOTKEY:
      if (wParam == HOTKEY_ID) {
        // hotkey toggles: als preview open is -> sluit preview en start capture
        if (g_hwndPreview) {
          DestroyPreview();
          CreateOverlay();
          return 0;
        }

        // overlay aan/uit
        if (g_hwndOverlay) DestroyOverlay();
        else CreateOverlay();
      }
      return 0;

    case WM_DESTROY:
      UnregisterHotKey(hwnd, HOTKEY_ID);
      DestroyPreview();
      DestroyOverlay();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------
// Entry point
// -----------------------------
int WINAPI wWinMain(_In_ HINSTANCE hInst,
                    _In_opt_ HINSTANCE hPrevInst,
                    _In_ PWSTR pCmdLine,
                    _In_ int nCmdShow) {
  g_hInst = hInst;
  UNREFERENCED_PARAMETER(hPrevInst);
  UNREFERENCED_PARAMETER(pCmdLine);
  UNREFERENCED_PARAMETER(nCmdShow);

  // default save dir (kan later via RMB menu aangepast worden)
  g_saveDir = DefaultSaveDir();

  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = MsgProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"SnipLiteMsgWindow";
    RegisterClassW(&wc);
    registered = true;
  }

  g_hwndMsg = CreateWindowExW(
    0, L"SnipLiteMsgWindow", L"", 0,
    0, 0, 0, 0,
    HWND_MESSAGE, nullptr, hInst, nullptr
  );

  if (!RegisterHotKey(g_hwndMsg, HOTKEY_ID, HOTKEY_MOD, HOTKEY_VK)) {
    MessageBoxW(nullptr, L"Hotkey (Ctrl+Alt+S) is al in gebruik.", L"snip-lite", MB_ICONERROR);
    return 1;
  }

  MSG m{};
  while (GetMessageW(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
