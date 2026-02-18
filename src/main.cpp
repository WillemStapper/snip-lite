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

// -----------------------------
// Hotkey
// -----------------------------
static constexpr int  HOTKEY_ID = 1;
static constexpr UINT HOTKEY_MOD = MOD_CONTROL | MOD_ALT;
static constexpr UINT HOTKEY_VK = 'S';            // Ctrl+Alt+S

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
static std::wstring g_statusText;
static bool g_autoDismissAfterSave = false;

static std::wstring g_saveDir;         // persistent
static std::wstring g_lastSavedFile;   // persistent
static std::wstring g_editorExe;       // persistent: "Open in..." program

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

static void LoadSettings() {
    g_saveDir = IniReadStr(L"General", L"SaveDir", DefaultSaveDir());
    g_autoDismissAfterSave = (IniReadInt(L"General", L"AutoDismiss", 0) != 0);
    g_editorExe = IniReadStr(L"General", L"EditorExe", L"");
    g_lastSavedFile = IniReadStr(L"General", L"LastSavedFile", L"");

    int m = IniReadInt(L"General", L"Mode", 0);
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    g_mode = (Mode)m;
}

static void SaveSettings() {
    IniWriteStr(L"General", L"SaveDir", g_saveDir);
    IniWriteInt(L"General", L"AutoDismiss", g_autoDismissAfterSave ? 1 : 0);
    IniWriteStr(L"General", L"EditorExe", g_editorExe);
    IniWriteStr(L"General", L"LastSavedFile", g_lastSavedFile);
    IniWriteInt(L"General", L"Mode", (int)g_mode);
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

static std::wstring TimestampedFileNameBmp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t buf[128]{};
    swprintf_s(buf, L"snip_%04u%02u%02u_%02u%02u%02u_%03u.bmp",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
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
            if (g_lastSavedFile.empty()) {
                // eerst save zodat er iets te openen is
                if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
                EnsureDirectoryRecursive(g_saveDir + L"\\");

                std::wstring filePath = g_saveDir + L"\\" + TimestampedFileNameBmp();
                if (!SaveBitmapAsBmpFile(g_captureBmp, filePath)) {
                    SetStatus(hwnd, L"Save failed");
                    return 0;
                }
                g_lastSavedFile = filePath;
                SaveSettings();
            }

            if (g_editorExe.empty()) {
                std::wstring exe;
                if (!PickExe(hwnd, exe)) { SetStatus(hwnd, L"Canceled"); return 0; }
                g_editorExe = exe;
                SaveSettings();
            }

            if (OpenInEditor(g_editorExe, g_lastSavedFile)) SetStatus(hwnd, L"Opened");
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
            if (g_saveDir.empty()) g_saveDir = DefaultSaveDir();
            OpenPath(g_saveDir);
            return 0;
        }
        case 2003: { // choose capture folder
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
            g_autoDismissAfterSave = !g_autoDismissAfterSave;
            SaveSettings();
            return 0;
        }
        case 2102: { // choose program
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
        case 2101: { // open in last program
            if (g_lastSavedFile.empty()) { SetStatus(hwnd, L"No saved file"); return 0; }
            if (g_editorExe.empty()) {
                std::wstring exe;
                if (!PickExe(hwnd, exe)) { SetStatus(hwnd, L"Canceled"); return 0; }
                g_editorExe = exe;
                SaveSettings();
            }
            if (OpenInEditor(g_editorExe, g_lastSavedFile)) SetStatus(hwnd, L"Opened");
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

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, iw, ih, SRCCOPY);

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
static void DestroyOverlay() {
    g_selecting = false;
    g_hasSelection = false;
    ZeroMemory(&g_selRectClient, sizeof(g_selRectClient));

    if (g_hwndOverlay) {
        DestroyWindow(g_hwndOverlay);
        g_hwndOverlay = nullptr;
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
        SetCapture(hwnd);
        g_selecting = true;
        g_hasSelection = false;
        g_selStart = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_selCur = g_selStart;
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

        const int rw = g_selRectClient.right - g_selRectClient.left;
        const int rh = g_selRectClient.bottom - g_selRectClient.top;
        g_hasSelection = (rw >= 5 && rh >= 5);

        if (!g_hasSelection) { InvalidateRect(hwnd, nullptr, TRUE); return 0; }

        if (g_mode != Mode::Region) {
            MessageBeep(MB_ICONWARNING);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        RECT wr{};
        GetWindowRect(hwnd, &wr);

        RECT sr{};
        sr.left = wr.left + g_selRectClient.left;
        sr.top = wr.top + g_selRectClient.top;
        sr.right = wr.left + g_selRectClient.right;
        sr.bottom = wr.top + g_selRectClient.bottom;

        ShowWindow(hwnd, SW_HIDE);
        Sleep(20);
        GdiFlush();

        FreeCapture();
        const bool capOk = CaptureRectToBitmap(sr, g_captureBmp, g_captureW, g_captureH);
        const bool clipOk = capOk ? CopyBitmapToClipboard(g_captureBmp) : false;

        if (clipOk) {
            DestroyOverlay();
            CreatePreviewWindow();
        }
        else {
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
            }
            else {
                DestroyOverlay();
            }
            return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: g_mode = Mode::Region;  SaveSettings(); break;
        case 1002: g_mode = Mode::Window;  SaveSettings(); break;
        case 1003: g_mode = Mode::Monitor; SaveSettings(); break;
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

        if (g_selecting || g_hasSelection) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, g_selRectClient.left, g_selRectClient.top, g_selRectClient.right, g_selRectClient.bottom);
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
