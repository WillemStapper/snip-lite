// snip-lite main.cpp
// Doel (MVP tot en met ===>> 2):
// - Ctrl+Alt+S: overlay aan/uit
// - Crosshair cursor + mode-tekst
// - Rechtermuisknop: klein menu om mode te kiezen
// - Linkermuisknop slepen: selectie-rechthoek tekenen
// - Linkermuisknop loslaten: knipsel capturen en in Clipboard zetten (Photoshop CS6 compatibel)
// - ESC: eerst selectie weg, daarna overlay weg
//
// REMARK: Dit is bewust "geen grote GUI". Alleen overlay + popup menu’s.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <sal.h>        // _In_ annotations voor wWinMain (voorkomt "inconsistent annotation")
#include <cstring>      // memcpy / memset

// -----------------------------
// Hotkey
// -----------------------------
static constexpr int  HOTKEY_ID  = 1;                 // intern id
static constexpr UINT HOTKEY_MOD = MOD_CONTROL | MOD_ALT;
static constexpr UINT HOTKEY_VK  = 'S';               // Ctrl+Alt+S

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
// Globals (app / windows)
// -----------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMsg = nullptr;        // message-only window (voor hotkey)
static HWND g_hwndOverlay = nullptr;    // overlay window

// -----------------------------
// Selectie state
// -----------------------------
static bool  g_selecting = false;
static bool  g_hasSelection = false;
static POINT g_selStart{};
static POINT g_selCur{};
static RECT  g_selRectClient{};         // selectie in client coords van overlay

// -----------------------------
// Capture state (bitmap)
// -----------------------------
static HBITMAP g_captureBmp = nullptr;
static int     g_captureW = 0;
static int     g_captureH = 0;

static void FreeCapture() {
  if (g_captureBmp) {
    DeleteObject(g_captureBmp);
    g_captureBmp = nullptr;
  }
  g_captureW = 0;
  g_captureH = 0;
}

// -----------------------------
// Helpers
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

  // REMARK: voorkom 0x0 rect (handig voor "klik zonder slepen")
  if (r.right == r.left)   r.right++;
  if (r.bottom == r.top)   r.bottom++;
  return r;
}

// REMARK: Capture het schermgedeelte (screen coords) naar een 32bpp DIBSection.
// We maken intern top-down (biHeight = -h), omdat dat praktisch is.
// Later bij clipboard zetten we het om naar bottom-up voor Photoshop CS6.
static bool CaptureRectToBitmap(const RECT& screenRect, HBITMAP& outBmp, int& outW, int& outH) {
  const int w = screenRect.right - screenRect.left;
  const int h = screenRect.bottom - screenRect.top;
  if (w <= 0 || h <= 0) return false;

  HDC hdcScreen = GetDC(nullptr);
  if (!hdcScreen) return false;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = h;          // bottom-up (Photoshop CS6 vriendelijker)
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

  // REMARK: CAPTUREBLT kan sommige "layered" dingen meenemen. Omdat we onze overlay verbergen,
  // is het doorgaans oké. Je kunt CAPTUREBLT ook weghalen als je rare effecten ziet.
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

// REMARK: Photoshop CS6 is kieskeurig. CF_DIB bottom-up werkt vaak het best.
// We zetten CF_DIB (bottom-up) en daarnaast ook CF_BITMAP (sommige apps gebruiken die).
static bool CopyBitmapToClipboard(HWND /*hwndOwner*/, HBITMAP hbmp) {
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
  bih.biHeight = absH;               // bottom-up (positief)
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;
  bih.biSizeImage = (DWORD)bitsSize;

  std::memcpy(p, &bih, sizeof(bih));

  const BYTE* srcBits = (const BYTE*)ds.dsBm.bmBits;
  BYTE* dstBits = p + headerSize;

  const bool srcTopDown = (ds.dsBmih.biHeight < 0);
  const int copyBytes = (srcStride < dstStride) ? srcStride : dstStride;

  // We schrijven bottom-up: bovenste rij komt onderaan in memory.
  for (int y = 0; y < absH; ++y) {
    const int srcY = srcTopDown ? y : (absH - 1 - y); // lees "van boven naar beneden" uit bron
    const int dstY = (absH - 1 - y);                  // schrijf naar bottom-up bestemming

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

  // Ook CF_BITMAP aanbieden (extra compatibiliteit)
  HBITMAP copyBmp = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
  if (copyBmp) {
    if (!SetClipboardData(CF_BITMAP, copyBmp)) {
      DeleteObject(copyBmp);
    }
  }

  CloseClipboard();
  // REMARK: ownership van hMem is nu van het Clipboard; niet zelf vrijgeven.
  return true;
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

  // TrackPopupMenu geeft via WM_COMMAND het gekozen item terug
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwndOwner, nullptr);

  DestroyMenu(menu);
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
// Overlay window proc
// -----------------------------
static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SETCURSOR: {
      SetCursor(LoadCursorW(nullptr, IDC_CROSS));
      return TRUE;
    }

    case WM_RBUTTONUP: {
      if (g_selecting) return 0;  // tijdens slepen geen menu
      ShowModeMenu(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      SetCapture(hwnd);
      g_selecting = true;
      g_hasSelection = false;

      g_selStart = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selCur   = g_selStart;
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);

      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_MOUSEMOVE: {
      if (!g_selecting) return 0;
      g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_LBUTTONUP: {
      if (!g_selecting) return 0;

      g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);

      g_selecting = false;
      ReleaseCapture();

      const int w = g_selRectClient.right  - g_selRectClient.left;
      const int h = g_selRectClient.bottom - g_selRectClient.top;
      g_hasSelection = (w >= 5 && h >= 5);

      if (!g_hasSelection) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }

      if (g_mode != Mode::Region) {
        // REMARK: Window/Monitor doen we later. Nu alleen Region.
        MessageBeep(MB_ICONWARNING);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }

      // client coords -> screen coords
      RECT wr{};
      GetWindowRect(hwnd, &wr);

      RECT sr{};
      sr.left   = wr.left + g_selRectClient.left;
      sr.top    = wr.top  + g_selRectClient.top;
      sr.right  = wr.left + g_selRectClient.right;
      sr.bottom = wr.top  + g_selRectClient.bottom;

      // REMARK: overlay verbergen zodat hij niet mee-captured
      ShowWindow(hwnd, SW_HIDE);

      // REMARK: geef Windows heel kort de kans om het scherm echt te updaten
      // (dit kan helpen tegen "zwarte waas" of het meecapturen van de overlay)
      Sleep(20);
      GdiFlush();

      FreeCapture();
      const bool capOk = CaptureRectToBitmap(sr, g_captureBmp, g_captureW, g_captureH);
      const bool clipOk = capOk ? CopyBitmapToClipboard(hwnd, g_captureBmp) : false;

      if (clipOk) {
        // REMARK: voorlopig sluiten we de overlay meteen.
        // Later vervangen we dit door een Preview met Save/Edit/Dismiss.
        DestroyOverlay();
      } else {
        MessageBeep(MB_ICONERROR);
        // Overlay terug tonen zodat je niet "kwijt" bent
        ShowWindow(hwnd, SW_SHOW);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }

    case WM_KEYDOWN: {
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
    }

    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case 1001: g_mode = Mode::Region;  break;
        case 1002: g_mode = Mode::Window;  break;
        case 1003: g_mode = Mode::Monitor; break;
        default: break;
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);

      // Achtergrond (simpel: donker transparant)
      RECT r{};
      GetClientRect(hwnd, &r);
      HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(hdc, &r, bg);
      DeleteObject(bg);

      // Mode tekst
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(255, 255, 255));
      const wchar_t* txt = ModeText(g_mode);

      RECT tr = r;
      tr.left += 20;
      tr.top  += 20;
      DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);

      // Selectie rechthoek
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

  // REMARK: hele overlay semi-transparant (ook tekst)
  SetLayeredWindowAttributes(g_hwndOverlay, 0, (BYTE)120, LWA_ALPHA);

  ShowWindow(g_hwndOverlay, SW_SHOW);
  SetForegroundWindow(g_hwndOverlay);
  SetFocus(g_hwndOverlay);
}

// -----------------------------
// Message-only window proc (hotkeys)
// -----------------------------
static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_HOTKEY:
      if (wParam == HOTKEY_ID) {
        if (g_hwndOverlay) DestroyOverlay();
        else CreateOverlay();
      }
      return 0;

    case WM_DESTROY:
      UnregisterHotKey(hwnd, HOTKEY_ID);
      FreeCapture();
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

  // REMARK: deze parameters gebruiken we nu nog niet, maar we willen geen warnings.
  UNREFERENCED_PARAMETER(hPrevInst);
  UNREFERENCED_PARAMETER(pCmdLine);
  UNREFERENCED_PARAMETER(nCmdShow);

  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = MsgProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"SnipLiteMsgWindow";
    RegisterClassW(&wc);
    registered = true;
  }

  // Message-only window voor hotkeys (geen GUI)
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
