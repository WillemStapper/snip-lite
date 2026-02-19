// snip-lite main.cpp
// ===>> 4: settings persistence + "Open in..." + preview window position persistence
//
// Opslag:
//   %LOCALAPPDATA%\snip-lite\settings.ini
//
// Filosofie:
// - Geen grote GUI. Alleen overlay + kleine preview + popup menu’s.
// - Instellingen worden runtime aangepast en blijven bewaard tussen sessies.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM
#include <sal.h>          // _In_ annotations
#include <shellapi.h>     // ShellExecuteW / ShellExecuteExW
#include <shobjidl.h>     // IFileDialog (folder picker / exe picker)
#include <cstring>        // memcpy / memset
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// -----------------------------
// Hotkey
// -----------------------------
static constexpr int  HOTKEY_ID = 1;
static constexpr UINT HOTKEY_MOD = MOD_CONTROL | MOD_ALT;
static constexpr UINT HOTKEY_VK = 'S';            // Ctrl+Alt+S

// -----------------------------
// Modes
// -----------------------------
enum class Mode { Region = 0, Window = 1, Monitor = 2, Freestyle = 3 };
static Mode g_mode = Mode::Region;

static const wchar_t* ModeText(Mode m) {
    switch (m) {
    case Mode::Region:  return L"Mode: Region";
    case Mode::Window:  return L"Mode: Window";
    case Mode::Monitor: return L"Mode: Monitor";
	case Mode::Freestyle: return L"Mode: Freestyle";
    default:            return L"Mode: ?";
    }
}

// -----------------------------
// Globals (windows)
// -----------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMsg = nullptr;   // message-only window (hotkey)
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
// Hover state (Window/Monitor)
// -----------------------------
static bool  g_hoverValid = false;
static HWND  g_hoverHwnd = nullptr;
static RECT  g_hoverRectScreen{};
static RECT  g_hoverRectClient{};

// -----------------------------
// Capture state (bitmap)
// -----------------------------
static HBITMAP g_captureBmp = nullptr;
static int     g_captureW = 0;
static int     g_captureH = 0;

// -----------------------------
// Freestyle (Lasso) state
// -----------------------------
static bool g_lassoSelecting = false;
static std::vector<POINT> g_lassoPtsClient;
static RECT g_lassoBoundsClient{};
static bool g_captureHasAlpha = false;   // straks voor preview + save

// -----------------------------
// Preview UI state
// -----------------------------
static RECT g_rcImage{};
static RECT g_btnSave{};
static RECT g_btnEdit{};
static RECT g_btnDismiss{};
static std::wstring g_statusText;
static bool g_autoDismissAfterSave = false;

static std::wstring g_saveDir;         // persistent
static std::wstring g_lastSavedFile;   // persistent
static std::wstring g_editorExe;       // persistent: "Open in..." program
static std::wstring g_tempEditFile;   // alleen voor huidige preview/capture

static constexpr UINT_PTR TIMER_STATUS_CLEAR = 1;

// =========================================================
// Helpers: strings, directories, INI settings
// =========================================================
static std::wstring GetEnvW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return L"";
    std::wstring s;
    s.resize(n);
    GetEnvironmentVariableW(name, s.data(), n);
    if (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

// simpele “mkdir -p” voor \-paden
static bool EnsureDirectoryRecursive(const std::wstring& path) {
    if (path.size() < 3) return false; // minimaal "C:\"
    std::wstring cur;
    cur.reserve(path.size());

    for (size_t i = 0; i < path.size(); ++i) {
        wchar_t c = path[i];
        cur.push_back(c);

        if (c == L'\\' || c == L'/') {
            if (cur.size() <= 3) continue; // "C:\"
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    return (e == ERROR_ALREADY_EXISTS);
}

static std::wstring SettingsDir() {
    std::wstring lad = GetEnvW(L"LOCALAPPDATA");
    if (lad.empty()) return L".\\snip-lite";
    return lad + L"\\snip-lite";
}

static std::wstring SettingsFile() {
    return SettingsDir() + L"\\settings.ini";
}

static std::wstring IniReadStr(const wchar_t* section, const wchar_t* key, const std::wstring& defVal) {
    wchar_t buf[4096]{};
    GetPrivateProfileStringW(section, key, defVal.c_str(), buf, (DWORD)_countof(buf), SettingsFile().c_str());
    return buf;
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

static int IniReadInt(const wchar_t* section, const wchar_t* key, int defVal) {
    return GetPrivateProfileIntW(section, key, defVal, SettingsFile().c_str());
}

static void IniWriteStr(const wchar_t* section, const wchar_t* key, const std::wstring& val) {
    EnsureDirectoryRecursive(SettingsDir() + L"\\");
    WritePrivateProfileStringW(section, key, val.c_str(), SettingsFile().c_str());
}

static void IniWriteInt(const wchar_t* section, const wchar_t* key, int val) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"%d", val);
    IniWriteStr(section, key, buf);
}

static std::wstring DefaultSaveDir() {
    // Eenvoudig: USERPROFILE\Pictures\snip-lite (werkt meestal)
    std::wstring up = GetEnvW(L"USERPROFILE");
    if (up.empty()) return L".\\captures";

    std::wstring pictures = up + L"\\Pictures";
    std::wstring dir = pictures + L"\\snip-lite";

    EnsureDirectoryRecursive(pictures + L"\\");
    EnsureDirectoryRecursive(dir + L"\\");

    return dir;
}

static std::wstring TempDir() {
    std::wstring lad = GetEnvW(L"LOCALAPPDATA");
    if (lad.empty()) return L".\\snip-lite-tmp";
    return lad + L"\\snip-lite\\tmp";
}

static std::wstring MakeTempEditPath() {
    std::wstring dir = TempDir();
    EnsureDirectoryRecursive(dir + L"\\");
    return dir + L"\\" + TimestampedFileNameBmp();  // bv snip_...bmp
}

static void LoadSettings() {
    g_saveDir = IniReadStr(L"General", L"SaveDir", DefaultSaveDir());
    g_autoDismissAfterSave = (IniReadInt(L"General", L"AutoDismiss", 0) != 0);
    g_editorExe = IniReadStr(L"General", L"EditorExe", L"");
    g_lastSavedFile = IniReadStr(L"General", L"LastSavedFile", L"");

    int m = IniReadInt(L"General", L"Mode", 0);
    if (m < 0) m = 0;
    if (m > 3) m = 3;
    g_mode = (Mode)m;
}

static void SaveSettings() {
    IniWriteStr(L"General", L"SaveDir", g_saveDir);
    IniWriteInt(L"General", L"AutoDismiss", g_autoDismissAfterSave ? 1 : 0);
    IniWriteStr(L"General", L"EditorExe", g_editorExe);
    IniWriteStr(L"General", L"LastSavedFile", g_lastSavedFile);
    IniWriteInt(L"General", L"Mode", (int)g_mode);
}

static std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return path.substr(0, pos);
}

static void PreviewDropTopmost(HWND hwnd) {
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// =========================================================
// Helpers: geometry, UI
// =========================================================
static RECT VirtualScreenRect() {
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

static RECT MakeNormalizedRect(POINT a, POINT b) {
    RECT r{};
    r.left = (a.x < b.x) ? a.x : b.x;
    r.right = (a.x > b.x) ? a.x : b.x;
    r.top = (a.y < b.y) ? a.y : b.y;
    r.bottom = (a.y > b.y) ? a.y : b.y;

    if (r.right == r.left)   r.right++;
    if (r.bottom == r.top)   r.bottom++;
    return r;
}

static bool PtInRectEx(const RECT& r, POINT p) {
    return (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom);
}

// -----------------------------
// Helpers: Window/Monitor pick
// -----------------------------
static void ClearHover() {
    g_hoverValid = false;
    g_hoverHwnd = nullptr;
    ZeroMemory(&g_hoverRectScreen, sizeof(g_hoverRectScreen));
    ZeroMemory(&g_hoverRectClient, sizeof(g_hoverRectClient));
}

static bool IsSnipLiteWindow(HWND h) {
    return (h == g_hwndOverlay) || (h == g_hwndPreview) || (h == g_hwndMsg);
}

static bool GetWindowRectSafe(HWND h, RECT& out) {
    if (!h) return false;
    if (!GetWindowRect(h, &out)) return false;
    return (out.right > out.left) && (out.bottom > out.top);
}

static bool IsCandidateCaptureWindow(HWND h) {
    if (!h) return false;
    if (IsSnipLiteWindow(h)) return false;
    if (!IsWindowVisible(h)) return false;
    if (IsIconic(h)) return false;

    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (style & WS_CHILD) return false;

    RECT rc{};
    if (!GetWindowRectSafe(h, rc)) return false;
    if ((rc.right - rc.left) < 10 || (rc.bottom - rc.top) < 10) return false;

    return true;
}

static HWND PickTopWindowAtPoint(POINT ptScreen, RECT& outRectScreen) {
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsCandidateCaptureWindow(h)) continue;
        RECT rc{};
        if (!GetWindowRectSafe(h, rc)) continue;
        if (PtInRectEx(rc, ptScreen)) {
            outRectScreen = rc;
            return h;
        }
    }
    return nullptr;
}

static bool GetMonitorRectAtPoint(POINT ptScreen, RECT& outRectScreen) {
    HMONITOR mon = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONEAREST);
    if (!mon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    outRectScreen = mi.rcMonitor;
    return (outRectScreen.right > outRectScreen.left) && (outRectScreen.bottom > outRectScreen.top);
}

static void SetHoverFromScreenRect(HWND hwndOverlay, HWND hoverHwnd, const RECT& screenRect) {
    g_hoverHwnd = hoverHwnd;
    g_hoverRectScreen = screenRect;

    RECT ow{};
    GetWindowRect(hwndOverlay, &ow);
    g_hoverRectClient = {
        screenRect.left - ow.left,
        screenRect.top - ow.top,
        screenRect.right - ow.left,
        screenRect.bottom - ow.top
    };

    g_hoverValid = true;
}

static void FreeCapture() {
    if (g_captureBmp) {
        DeleteObject(g_captureBmp);
        g_captureBmp = nullptr;
    }
    g_captureW = 0;
    g_captureH = 0;
}

static void SetStatus(HWND hwndPreview, const std::wstring& s) {
    g_statusText = s;
    InvalidateRect(hwndPreview, nullptr, TRUE);
    SetTimer(hwndPreview, TIMER_STATUS_CLEAR, 1500, nullptr);
}

// =========================================================
// Dialog helpers: Pick folder / Pick exe
// =========================================================
static void CoInitForDialog(bool& outNeedUninit) {
    outNeedUninit = false;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) outNeedUninit = true;
    // RPC_E_CHANGED_MODE: COM is al init met andere mode; dan mogen we niet CoUninitialize doen.
}

static bool PickFolder(HWND owner, const std::wstring& startDir, std::wstring& outDir) {
    outDir.clear();

    bool needUninit = false;
    CoInitForDialog(needUninit);

    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr) || !pfd) {
        if (needUninit) CoUninitialize();
        return false;
    }

    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);

    if (!startDir.empty()) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr, IID_PPV_ARGS(&psi))) && psi) {
            pfd->SetFolder(psi);
            psi->Release();
        }
    }

    hr = pfd->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(pfd->GetResult(&item)) && item) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                outDir = psz;
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }

    pfd->Release();
    if (needUninit) CoUninitialize();

    return !outDir.empty();
}

static bool PickExe(HWND owner, std::wstring& outExe) {
    outExe.clear();

    bool needUninit = false;
    CoInitForDialog(needUninit);

    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr) || !pfd) {
        if (needUninit) CoUninitialize();
        return false;
    }

    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);

    COMDLG_FILTERSPEC spec[] = {
      { L"Programs (*.exe)", L"*.exe" },
      { L"All files (*.*)",  L"*.*" }
    };
    pfd->SetFileTypes((UINT)_countof(spec), spec);
    pfd->SetFileTypeIndex(1);
    // Startfolder voor de EXE-picker:
    // 1) Als er al een editor gekozen is: open in die map
    // 2) Anders: Program Files (x86) (jouw wens), fallback naar Program Files
    std::wstring start = DirName(g_editorExe);
    if (start.empty()) start = GetEnvW(L"ProgramFiles(x86)");
    if (start.empty()) start = GetEnvW(L"ProgramFiles"); // fallback

    if (!start.empty()) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&psi))) && psi) {
            pfd->SetFolder(psi);          // of: pfd->SetDefaultFolder(psi);
            psi->Release();
        }
    }

    hr = pfd->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(pfd->GetResult(&item)) && item) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                outExe = psz;
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }

    pfd->Release();
    if (needUninit) CoUninitialize();

    return !outExe.empty();
}

// =========================================================
// Capture + Clipboard + Save
// =========================================================
static bool CaptureRectToBitmap(const RECT& screenRect, HBITMAP& outBmp, int& outW, int& outH) {
    const int w = screenRect.right - screenRect.left;
    const int h = screenRect.bottom - screenRect.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h;          // bottom-up: Photoshop CS6 plakt anders verticaal geflipt
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

static bool CopyBitmapToClipboard(HBITMAP hbmp) {
    if (!hbmp) return false;

    DIBSECTION ds{};
    if (GetObjectW(hbmp, sizeof(ds), &ds) == 0 || ds.dsBm.bmBits == nullptr) {
        return false;
    }

    const int w = ds.dsBmih.biWidth;
    const int absH = (ds.dsBmih.biHeight < 0) ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;

    const int srcStride = ds.dsBm.bmWidthBytes;
    const int dstStride = w * 4;

    const SIZE_T headerSize = sizeof(BITMAPINFOHEADER);
    const SIZE_T bitsSize = (SIZE_T)dstStride * (SIZE_T)absH;
    const SIZE_T totalSize = headerSize + bitsSize;

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

    // schrijf bottom-up: bovenste rij komt onderaan in memory
    for (int y = 0; y < absH; ++y) {
        const int srcY = srcTopDown ? y : (absH - 1 - y); // lees "van boven naar beneden"
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

    // Extra compatibiliteit: CF_BITMAP ook aanbieden
    HBITMAP copyBmp = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (copyBmp) {
        if (!SetClipboardData(CF_BITMAP, copyBmp)) {
            DeleteObject(copyBmp);
        }
    }

    CloseClipboard();
    return true;
}

#pragma comment(lib, "Msimg32.lib")

static bool CopyBitmapToClipboardAlphaV5(HBITMAP hbmp) {
    if (!hbmp) return false;

    DIBSECTION ds{};
    if (GetObjectW(hbmp, sizeof(ds), &ds) == 0 || ds.dsBm.bmBits == nullptr) return false;

    const int w = ds.dsBmih.biWidth;
    const int absH = (ds.dsBmih.biHeight < 0) ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;

    const int srcStride = ds.dsBm.bmWidthBytes;
    const int dstStride = w * 4;

    const SIZE_T headerSize = sizeof(BITMAPV5HEADER);
    const SIZE_T bitsSize = (SIZE_T)dstStride * (SIZE_T)absH;
    const SIZE_T totalSize = headerSize + bitsSize;

    HGLOBAL hMemV5 = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hMemV5) return false;

    BYTE* p = (BYTE*)GlobalLock(hMemV5);
    if (!p) { GlobalFree(hMemV5); return false; }

    BITMAPV5HEADER bvh{};
    bvh.bV5Size = sizeof(BITMAPV5HEADER);
    bvh.bV5Width = w;
    bvh.bV5Height = absH; // bottom-up
    bvh.bV5Planes = 1;
    bvh.bV5BitCount = 32;
    bvh.bV5Compression = BI_BITFIELDS;
    bvh.bV5RedMask = 0x00FF0000;
    bvh.bV5GreenMask = 0x0000FF00;
    bvh.bV5BlueMask = 0x000000FF;
    bvh.bV5AlphaMask = 0xFF000000;
    bvh.bV5CSType = LCS_sRGB;

    std::memcpy(p, &bvh, sizeof(bvh));

    const BYTE* srcBits = (const BYTE*)ds.dsBm.bmBits;
    BYTE* dstBits = p + headerSize;

    const bool srcTopDown = (ds.dsBmih.biHeight < 0);
    const int copyBytes = (srcStride < dstStride) ? srcStride : dstStride;

    for (int y = 0; y < absH; ++y) {
        const int srcY = srcTopDown ? y : (absH - 1 - y);
        const int dstY = (absH - 1 - y);

        const BYTE* srcRow = srcBits + (SIZE_T)srcY * (SIZE_T)srcStride;
        BYTE* dstRow = dstBits + (SIZE_T)dstY * (SIZE_T)dstStride;

        std::memcpy(dstRow, srcRow, (size_t)copyBytes);
        if (copyBytes < dstStride) std::memset(dstRow + copyBytes, 0, (size_t)(dstStride - copyBytes));
    }

    GlobalUnlock(hMemV5);

    if (!OpenClipboard(nullptr)) { GlobalFree(hMemV5); return false; }
    EmptyClipboard();

    bool ok = (SetClipboardData(CF_DIBV5, hMemV5) != nullptr);

    // fallback: ook CF_BITMAP aanbieden
    HBITMAP copyBmp = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (copyBmp) {
        if (!SetClipboardData(CF_BITMAP, copyBmp)) DeleteObject(copyBmp);
    }

    CloseClipboard();
    if (!ok) { GlobalFree(hMemV5); return false; }
    return true;
}

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
    bih.biHeight = h;                 // BMP schrijven als bottom-up
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

    // BMP (bottom-up) verwacht eerst onderste scanline.
    if (ok) {
        for (int row = 0; row < h; ++row) {
            const int srcRow = srcTopDown ? (h - 1 - row) : row;
            const BYTE* pRow = bits + (SIZE_T)srcRow * stride;
            ok = ok && WriteFile(hf, pRow, stride, &written, nullptr);
            if (!ok) break;
        }
    }

    CloseHandle(hf);
    return ok;
}

static bool ApplyLassoAlphaMask(HBITMAP hbmp, const std::vector<POINT>& ptsClient, const RECT& boundsClient) {
    if (!hbmp || ptsClient.size() < 3) return false;

    DIBSECTION ds{};
    if (GetObjectW(hbmp, sizeof(ds), &ds) == 0 || !ds.dsBm.bmBits) return false;

    const int w = ds.dsBmih.biWidth;
    const int h = (ds.dsBmih.biHeight < 0) ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;
    const int stride = ds.dsBm.bmWidthBytes;

    // polygon naar "local" coords (0..w/h)
    std::vector<POINT> poly;
    poly.reserve(ptsClient.size());
    for (auto p : ptsClient) {
        poly.push_back({ p.x - boundsClient.left, p.y - boundsClient.top });
    }

    BYTE* bits = (BYTE*)ds.dsBm.bmBits;
    std::vector<double> xs;
    xs.reserve(poly.size());

    for (int y = 0; y < h; ++y) {
        // DIB is bottom-up: y=0 (top) zit in memory op rij (h-1)
        BYTE* row = bits + (size_t)(h - 1 - y) * (size_t)stride;

        // bewaar originele rij (we gaan buitengebied leegmaken)
        std::vector<BYTE> backup((size_t)w * 4);
        std::memcpy(backup.data(), row, (size_t)w * 4);

        // maak hele rij transparant
        std::memset(row, 0, (size_t)w * 4);

        xs.clear();

        // snijpunten met scanline (even-odd rule)
        const int n = (int)poly.size();
        for (int i = 0; i < n; ++i) {
            POINT a = poly[i];
            POINT b = poly[(i + 1) % n];
            if (a.y == b.y) continue;

            int ymin = (a.y < b.y) ? a.y : b.y;
            int ymax = (a.y < b.y) ? b.y : a.y;

            if (y < ymin || y >= ymax) continue;

            double t = (double)(y - a.y) / (double)(b.y - a.y);
            double x = (double)a.x + t * (double)(b.x - a.x);
            xs.push_back(x);
        }

        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());

        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int x0 = (int)std::ceil(xs[k]);
            int x1 = (int)std::floor(xs[k + 1]);

            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;

            for (int x = x0; x < x1; ++x) {
                // BGRA
                row[x * 4 + 0] = backup[x * 4 + 0];
                row[x * 4 + 1] = backup[x * 4 + 1];
                row[x * 4 + 2] = backup[x * 4 + 2];
                row[x * 4 + 3] = 255; // alpha
            }
        }
    }

    return true;
}

static bool OpenInEditor(const std::wstring& editorExe, const std::wstring& filePath) {
    if (editorExe.empty() || filePath.empty()) return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = editorExe.c_str();

    std::wstring params = L"\"" + filePath + L"\"";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

static void OpenPath(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// =========================================================
// Menus
// =========================================================
static void ShowModeMenu(HWND hwndOwner) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_mode == Mode::Region ? MF_CHECKED : 0), 1001, L"Region");
    AppendMenuW(menu, MF_STRING | (g_mode == Mode::Window ? MF_CHECKED : 0), 1002, L"Window");
    AppendMenuW(menu, MF_STRING | (g_mode == Mode::Monitor ? MF_CHECKED : 0), 1003, L"Monitor");
    AppendMenuW(menu, MF_STRING | (g_mode == Mode::Freestyle ? MF_CHECKED : 0), 1004, L"Freestyle (Lasso)");

    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwndOwner, nullptr);
    DestroyMenu(menu);
}

static void ShowSaveMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 2001, L"Open capture folder");
    AppendMenuW(menu, MF_STRING, 2003, L"Choose capture folder...");
    AppendMenuW(menu, MF_STRING | (g_autoDismissAfterSave ? MF_CHECKED : 0), 2002, L"Auto-dismiss after Save");

    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void ShowEditMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 2101, L"Open in (last program)");
    AppendMenuW(menu, MF_STRING, 2102, L"Choose program...");

    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NOANIMATION, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// =========================================================
// Preview layout + lifecycle
// =========================================================
static void LayoutPreview(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int pad = 12;
    const int barH = 52;
    const int btnW = 110;
    const int btnH = 34;
    const int gap = 10;

    g_rcImage = rc;
    g_rcImage.left += pad;
    g_rcImage.top += pad;
    g_rcImage.right -= pad;
    g_rcImage.bottom -= (barH + pad);

    int totalW = btnW * 3 + gap * 2;
    int x0 = (rc.right - totalW) / 2;
    int y0 = rc.bottom - barH + (barH - btnH) / 2;

    g_btnSave = { x0,                 y0, x0 + btnW,            y0 + btnH };
    g_btnEdit = { x0 + btnW + gap,    y0, x0 + 2 * btnW + gap,   y0 + btnH };
    g_btnDismiss = { x0 + 2 * (btnW + gap),  y0, x0 + 3 * btnW + 2 * gap, y0 + btnH };
}

static void DestroyOverlay();

static void DestroyPreview() {
    if (g_hwndPreview) {
        DestroyWindow(g_hwndPreview);
        g_hwndPreview = nullptr;
    }
    FreeCapture();
    g_statusText.clear();
}

// =========================================================
// Preview WindowProc
// =========================================================
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
        if (wParam == VK_ESCAPE) { DestroyPreview(); return 0; }
        return 0;

    case WM_SETCURSOR: {
        // Windows geeft in lParam de hit-test code mee (HTCLIENT / HTCAPTION / etc.)
        const WORD ht = LOWORD(lParam);

        // Als wij sleepgebied teruggeven als HTCAPTION -> maak cursor een handje
        if (ht == HTCAPTION) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }

        // Als het client-area is (buttons), ook handje
        if (ht == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            if (PtInRectEx(g_btnSave, pt) || PtInRectEx(g_btnEdit, pt) || PtInRectEx(g_btnDismiss, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }

            // Overige client-area: normale pijl
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // === drag window by clicking anywhere except buttons
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT) return hit;

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        if (PtInRectEx(g_btnSave, pt) || PtInRectEx(g_btnEdit, pt) || PtInRectEx(g_btnDismiss, pt)) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }

    // === remember position/size after move/resize
    case WM_EXITSIZEMOVE: {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        IniWriteInt(L"Preview", L"X", wr.left);
        IniWriteInt(L"Preview", L"Y", wr.top);
        IniWriteInt(L"Preview", L"W", wr.right - wr.left);
        IniWriteInt(L"Preview", L"H", wr.bottom - wr.top);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // Save
        if (PtInRectEx(g_btnSave, p)) {
            if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
            EnsureDirectoryRecursive(g_saveDir + L"\\");

            std::wstring filePath = g_saveDir + L"\\" + TimestampedFileNameBmp();
            if (SaveBitmapAsBmpFile(g_captureBmp, filePath)) {
                g_lastSavedFile = filePath;
                SaveSettings();
                SetStatus(hwnd, L"Saved");
                if (g_autoDismissAfterSave) DestroyPreview();
            }
            else {
                SetStatus(hwnd, L"Save failed");
            }
            return 0;
        }

        // Edit (open in chosen program)
        if (PtInRectEx(g_btnEdit, p)) {
            // 1) Zorg dat we een editor hebben
            if (g_editorExe.empty()) {
                std::wstring exe;
                if (!PickExe(hwnd, exe)) { SetStatus(hwnd, L"Canceled"); return 0; }
                g_editorExe = exe;
                SaveSettings();
            }

            // 2) Schrijf HUIDIGE capture naar een temp bestand
            std::wstring tempPath = MakeTempEditPath();
            if (!SaveBitmapAsBmpFile(g_captureBmp, tempPath)) {
                SetStatus(hwnd, L"Save failed");
                return 0;
            }
            g_tempEditFile = tempPath;

            // 3) Open temp in editor
            PreviewDropTopmost(hwnd);   // (als je die helper al hebt)
            if (OpenInEditor(g_editorExe, g_tempEditFile)) SetStatus(hwnd, L"Opened");
            else SetStatus(hwnd, L"Open failed");

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
            PreviewDropTopmost(hwnd);
            if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
            OpenPath(g_saveDir);
            return 0;
        }
        case 2003: { // choose capture folder
            PreviewDropTopmost(hwnd);
            std::wstring picked;
            std::wstring start = g_saveDir.empty() ? DefaultSaveDir() : g_saveDir;
            if (PickFolder(hwnd, start, picked)) {
                g_saveDir = picked;
                EnsureDirectoryRecursive(g_saveDir + L"\\");
                SaveSettings();
                SetStatus(hwnd, L"Folder set");
            }
            else {
                SetStatus(hwnd, L"Canceled");
            }
            return 0;
        }
        case 2002: { // toggle auto-dismiss
            PreviewDropTopmost(hwnd);
            g_autoDismissAfterSave = !g_autoDismissAfterSave;
            SaveSettings();
            return 0;
        }
        case 2102: { // choose program
            PreviewDropTopmost(hwnd);
            std::wstring exe;
            if (PickExe(hwnd, exe)) {
                g_editorExe = exe;
                SaveSettings();
                SetStatus(hwnd, L"Program set");
            }
            else {
                SetStatus(hwnd, L"Canceled");
            }
            return 0;
        }
        case 2101: { // Open in (last program) - voorkeur: huidige capture
            // 1) Zorg dat er een editor gekozen is
            if (g_editorExe.empty()) {
                std::wstring exe;
                if (!PickExe(hwnd, exe)) { SetStatus(hwnd, L"Canceled"); return 0; }
                g_editorExe = exe;
                SaveSettings();
            }
            std::wstring target;

            // 2) Probeer eerst het HUIDIGE knipsel (preview) als temp-bestand
            if (g_captureBmp) {
                if (g_tempEditFile.empty()) {
                    std::wstring tempPath = MakeTempEditPath();
                    if (SaveBitmapAsBmpFile(g_captureBmp, tempPath)) {
                        g_tempEditFile = tempPath;
                    }
                }
                target = g_tempEditFile;
            }
            // 3) Fallback: laatst opgeslagen bestand (als er geen capture/temp is)
            if (target.empty()) target = g_lastSavedFile;
            if (target.empty()) { SetStatus(hwnd, L"No file"); return 0; }
            // 4) Open in editor
            PreviewDropTopmost(hwnd); // als je die helper al gebruikt
            if (OpenInEditor(g_editorExe, target)) SetStatus(hwnd, L"Opened");
            else SetStatus(hwnd, L"Open failed");

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

            if (g_captureHasAlpha) {
                BLENDFUNCTION bf{};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = 255;
                bf.AlphaFormat = AC_SRC_ALPHA;

                AlphaBlend(hdc, dx, dy, dw, dh, mem, 0, 0, iw, ih, bf);
            }
            else {
                SetStretchBltMode(hdc, HALFTONE);
                StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, iw, ih, SRCCOPY);
            }

            SelectObject(mem, old);
            DeleteDC(mem);

            // border
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

// =========================================================
// Preview creation
// =========================================================
static void CreatePreviewWindow() {
    if (g_hwndPreview) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PreviewProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = L"SnipLitePreview";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int maxW = (int)((wa.right - wa.left) * 0.70);
    int maxH = (int)((wa.bottom - wa.top) * 0.70);

    int w = (g_captureW > 0) ? g_captureW : 600;
    int h = (g_captureH > 0) ? g_captureH : 400;

    int winW = w + 24;
    int winH = h + 24 + 52;

    double sx = (winW > 0) ? (double)maxW / (double)winW : 1.0;
    double sy = (winH > 0) ? (double)maxH / (double)winH : 1.0;
    double s = (sx < sy) ? sx : sy;
    if (s > 1.0) s = 1.0;

    winW = (int)(winW * s);
    winH = (int)(winH * s);

    int x = wa.left + ((wa.right - wa.left) - winW) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - winH) / 2;

    // restore vorige positie/grootte (als aanwezig)
    int rx = IniReadInt(L"Preview", L"X", x);
    int ry = IniReadInt(L"Preview", L"Y", y);
    int rw = IniReadInt(L"Preview", L"W", winW);
    int rh = IniReadInt(L"Preview", L"H", winH);

    if (rw < 300) rw = winW;
    if (rh < 200) rh = winH;

    g_hwndPreview = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SnipLitePreview",
        L"",
        WS_POPUP,
        rx, ry, rw, rh,
        nullptr, nullptr, g_hInst, nullptr
    );

    ShowWindow(g_hwndPreview, SW_SHOW);
    SetForegroundWindow(g_hwndPreview);
    SetFocus(g_hwndPreview);
}

// =========================================================
// Overlay
// =========================================================
static void LassoReset() {
    g_lassoSelecting = false;
    g_lassoPtsClient.clear();
    ZeroMemory(&g_lassoBoundsClient, sizeof(g_lassoBoundsClient));
}

static void DestroyOverlay() {
    LassoReset();
    g_selecting = false;
    g_hasSelection = false;
    ZeroMemory(&g_selRectClient, sizeof(g_selRectClient));
	ClearHover();

    if (g_hwndOverlay) {
        DestroyWindow(g_hwndOverlay);
        g_hwndOverlay = nullptr;
    }
}

static bool CaptureScreenRectAndShowPreview(HWND hwndOverlay, const RECT& sr) {
    ShowWindow(hwndOverlay, SW_HIDE);
    Sleep(20);
    GdiFlush();

    FreeCapture();
    const bool capOk = CaptureRectToBitmap(sr, g_captureBmp, g_captureW, g_captureH);
    const bool clipOk = capOk ? CopyBitmapToClipboard(g_captureBmp) : false;

    if (clipOk) {
        DestroyOverlay();
        g_tempEditFile.clear();
        CreatePreviewWindow();
        return true;
    }

    MessageBeep(MB_ICONERROR);
    ShowWindow(hwndOverlay, SW_SHOW);
    InvalidateRect(hwndOverlay, nullptr, TRUE);
    return false;
}

static void LassoAddPoint(POINT p) {
    // punten uitdunnen: scheelt CPU bij masken
    if (!g_lassoPtsClient.empty()) {
        POINT last = g_lassoPtsClient.back();
        int dx = p.x - last.x, dy = p.y - last.y;
        if (dx * dx + dy * dy < 9) return; // <3px: negeren
    }

    g_lassoPtsClient.push_back(p);

    if (g_lassoPtsClient.size() == 1) {
        g_lassoBoundsClient = { p.x, p.y, p.x + 1, p.y + 1 };
    }
    else {
        if (p.x < g_lassoBoundsClient.left)   g_lassoBoundsClient.left = p.x;
        if (p.y < g_lassoBoundsClient.top)    g_lassoBoundsClient.top = p.y;
        if (p.x + 1 > g_lassoBoundsClient.right)  g_lassoBoundsClient.right = p.x + 1;
        if (p.y + 1 > g_lassoBoundsClient.bottom) g_lassoBoundsClient.bottom = p.y + 1;
    }
}

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
        if (g_mode == Mode::Freestyle) {
            SetCapture(hwnd);
            LassoReset();
            g_lassoSelecting = true;

            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            LassoAddPoint(p);

            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        if (g_mode != Mode::Region) return 0;

        ClearHover();
        SetCapture(hwnd);
        g_selecting = true;
        g_hasSelection = false;
        g_selStart = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_selCur = g_selStart;
        g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_MOUSEMOVE: {
        if (g_mode == Mode::Freestyle && g_lassoSelecting) {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            LassoAddPoint(p);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        // Region: rubberband rectangle
        if (g_mode == Mode::Region) {
            if (!g_selecting) return 0;
            g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        // Window/Monitor: hover highlight under cursor
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);

        if (g_mode == Mode::Window) {
            RECT wr{};
            HWND h = PickTopWindowAtPoint(pt, wr);
            if (h) SetHoverFromScreenRect(hwnd, h, wr);
            else ClearHover();
        }
        else if (g_mode == Mode::Monitor) {
            RECT mr{};
            if (GetMonitorRectAtPoint(pt, mr)) SetHoverFromScreenRect(hwnd, nullptr, mr);
            else ClearHover();
        }
        else {
            // Freestyle: later
            ClearHover();
        }

        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_mode == Mode::Freestyle && g_lassoSelecting) {
            ReleaseCapture();
            g_lassoSelecting = false;

            if (g_lassoPtsClient.size() < 3) {
                MessageBeep(MB_ICONWARNING);
                LassoReset();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }

            // bounds iets ruimer
            RECT b = g_lassoBoundsClient;
            b.left -= 2; b.top -= 2; b.right += 2; b.bottom += 2;

            if ((b.right - b.left) < 5 || (b.bottom - b.top) < 5) {
                MessageBeep(MB_ICONWARNING);
                LassoReset();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }

            RECT ow{};
            GetWindowRect(hwnd, &ow);

            RECT sr{
                ow.left + b.left,
                ow.top + b.top,
                ow.left + b.right,
                ow.top + b.bottom
            };

            ShowWindow(hwnd, SW_HIDE);
            Sleep(20);
            GdiFlush();

            FreeCapture();
            g_captureHasAlpha = false;

            bool capOk = CaptureRectToBitmap(sr, g_captureBmp, g_captureW, g_captureH);
            if (!capOk) {
                MessageBeep(MB_ICONERROR);
                ShowWindow(hwnd, SW_SHOW);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }

            // mask: alpha buiten lasso = 0
            if (!ApplyLassoAlphaMask(g_captureBmp, g_lassoPtsClient, b)) {
                MessageBeep(MB_ICONERROR);
                ShowWindow(hwnd, SW_SHOW);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            g_captureHasAlpha = true;

            bool clipOk = CopyBitmapToClipboardAlphaV5(g_captureBmp);

            if (clipOk) {
                DestroyOverlay();
                g_tempEditFile.clear();
                CreatePreviewWindow();
            }
            else {
                MessageBeep(MB_ICONERROR);
                ShowWindow(hwnd, SW_SHOW);
                InvalidateRect(hwnd, nullptr, TRUE);
            }

            LassoReset();
            return 0;
        }

        // Region mode: capture dragged rectangle
        if (g_mode == Mode::Region) {
            if (!g_selecting) return 0;

            g_selCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_selRectClient = MakeNormalizedRect(g_selStart, g_selCur);

            g_selecting = false;
            ReleaseCapture();

            const int rw = g_selRectClient.right - g_selRectClient.left;
            const int rh = g_selRectClient.bottom - g_selRectClient.top;
            g_hasSelection = (rw >= 5 && rh >= 5);

            if (!g_hasSelection) { InvalidateRect(hwnd, nullptr, TRUE); return 0; }

            RECT ow{};
            GetWindowRect(hwnd, &ow);

            RECT sr{};
            sr.left = ow.left + g_selRectClient.left;
            sr.top = ow.top + g_selRectClient.top;
            sr.right = ow.left + g_selRectClient.right;
            sr.bottom = ow.top + g_selRectClient.bottom;

            g_tempEditFile.clear();
            CaptureScreenRectAndShowPreview(hwnd, sr);
            return 0;
        }

        // Window/Monitor/Freestyle: capture on click
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);

        RECT sr{};
        bool have = false;

        if (g_mode == Mode::Window) {
            HWND h = PickTopWindowAtPoint(pt, sr);
            have = (h != nullptr);
        }
        else if (g_mode == Mode::Monitor) {
            have = GetMonitorRectAtPoint(pt, sr);
        }
        else {
            // Freestyle (Lasso) is added to the menu, implementation follows later.
            MessageBeep(MB_ICONWARNING);
            ClearHover();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        if (!have) {
            MessageBeep(MB_ICONWARNING);
            ClearHover();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        g_tempEditFile.clear();
        CaptureScreenRectAndShowPreview(hwnd, sr);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (g_lassoSelecting) {
                LassoReset();
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (g_selecting) {
                g_selecting = false;
                g_hasSelection = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            else {
                DestroyOverlay();
            }
            return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: g_mode = Mode::Region;    ClearHover(); SaveSettings(); break;
        case 1002: g_mode = Mode::Window;    ClearHover(); SaveSettings(); break;
        case 1003: g_mode = Mode::Monitor;   ClearHover(); SaveSettings(); break;
        case 1004: g_mode = Mode::Freestyle; ClearHover(); SaveSettings(); break;
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
        tr.left += 20; tr.top += 20;
        DrawTextW(hdc, ModeText(g_mode), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (g_mode == Mode::Freestyle && (g_lassoSelecting || g_lassoPtsClient.size() >= 2)) {
            if (g_lassoPtsClient.size() >= 2) {
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

                Polyline(hdc, g_lassoPtsClient.data(), (int)g_lassoPtsClient.size());

                // optioneel: bounding box (handig bij debug)
                // Rectangle(hdc, g_lassoBoundsClient.left, g_lassoBoundsClient.top,
                //           g_lassoBoundsClient.right, g_lassoBoundsClient.bottom);

                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }
        }

        if (g_selecting) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, g_selRectClient.left, g_selRectClient.top, g_selRectClient.right, g_selRectClient.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
        else if (g_hoverValid) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(100, 200, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, g_hoverRectClient.left, g_hoverRectClient.top, g_hoverRectClient.right, g_hoverRectClient.bottom);
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
    
static void CreateOverlay() {
    if (g_hwndOverlay) return;
	ClearHover();

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = L"SnipLiteOverlay";
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
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

// =========================================================
// Message-only window (hotkey)
// =========================================================
static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_ID) {
            if (g_hwndPreview) {
                DestroyPreview();
                CreateOverlay();
                return 0;
            }
            if (g_hwndOverlay) DestroyOverlay();
            else CreateOverlay();
        }
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID);
        SaveSettings();       // laatste flush
        DestroyPreview();
        DestroyOverlay();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =========================================================
// Entry point
// =========================================================
int WINAPI wWinMain(_In_ HINSTANCE hInst,
    _In_opt_ HINSTANCE hPrevInst,
    _In_ PWSTR pCmdLine,
    _In_ int nCmdShow) {
    g_hInst = hInst;
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(pCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    LoadSettings();
    if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MsgProc;
        wc.hInstance = hInst;
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
