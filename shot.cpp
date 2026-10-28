#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

#define HOTKEY_ID     1
#define WM_TRAYICON   (WM_APP + 1)
#define MAX_SHAPES    512
#define TOOLBAR_H     60
#define BTN_H         40

enum { TOOL_LINE = 0, TOOL_RECT, TOOL_ELLIPSE, TOOL_ARROW };

struct Shape { int type; POINT a, b; };
struct BtnDef { int x; int w; };
static const BtnDef g_btns[] = {
    {  10,  60 }, // Line
    {  80,  60 }, // Rect
    { 150,  60 }, // Ellipse
    { 220,  60 }, // Arrow
    { 310,  60 }, // Undo
    { 380,  60 }, // Save
    { 450,  60 }, // Cancel
};
#define BTN_COUNT (sizeof(g_btns) / sizeof(g_btns[0]))

static HBITMAP g_screenBmp = NULL;
static HDC     g_screenDC  = NULL;
static int     g_screenW = 0, g_screenH = 0;

static Shape   g_shapes[MAX_SHAPES];
static int     g_shapeCount = 0;
static int     g_tool = TOOL_LINE;
static BOOL    g_dragging = FALSE;
static POINT   g_dragStart = {0,0}, g_dragEnd = {0,0};
static HWND    g_msgWnd  = NULL;
static HWND    g_editWnd = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static BOOL    g_saving = FALSE;

static int     g_mouseX = 0, g_mouseY = 0;
static BOOL    g_showMouse = FALSE;
static NOTIFYICONDATA g_nid = {0};
static HICON   g_trayIcon = NULL;

// 生成瞄准镜图标 (代码纯手工绘制)
HICON CreateTargetIcon(int size, COLORREF color) {
    HDC hdc = GetDC(NULL);
    HDC hMem = CreateCompatibleDC(hdc);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    SelectObject(hMem, hBmp);
    memset(pBits, 0, size * size * 4);
    
    HPEN hPen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hMem, hPen);
    HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
    
    int c = size / 2;
    int r = size / 4;
    Ellipse(hMem, c - r, c - r, c + r, c + r);
    MoveToEx(hMem, c, 0, NULL); LineTo(hMem, c, c - r - 2);
    MoveToEx(hMem, c, c + r + 2, NULL); LineTo(hMem, c, size);
    MoveToEx(hMem, 0, c, NULL); LineTo(hMem, c - r - 2, c);
    MoveToEx(hMem, c + r + 2, c, NULL); LineTo(hMem, size, c);
    
    SelectObject(hMem, oldPen);
    SelectObject(hMem, oldBrush);
    DeleteObject(hPen);

    DWORD* pixels = (DWORD*)pBits;
    for (int i = 0; i < size * size; i++) {
        if ((pixels[i] & 0x00FFFFFF) != 0) pixels[i] |= 0xFF000000;
    }
    
    HBITMAP hMask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hBmp;
    HICON hIcon = CreateIconIndirect(&ii);
    
    DeleteObject(hMask);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hdc);
    return hIcon;
}

// 绘制工具栏图标
void draw_icon(HDC hdc, int i, int cx, int cy) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    switch(i) {
    case 0: // Line
        MoveToEx(hdc, cx - 8, cy + 8, NULL); LineTo(hdc, cx + 8, cy - 8); break;
    case 1: // Rect
        Rectangle(hdc, cx - 8, cy - 6, cx + 8, cy + 6); break;
    case 2: // Ellipse
        Ellipse(hdc, cx - 8, cy - 6, cx + 8, cy + 6); break;
    case 3: // Arrow
        MoveToEx(hdc, cx - 8, cy + 8, NULL); LineTo(hdc, cx + 8, cy - 8);
        MoveToEx(hdc, cx + 8, cy - 8, NULL); LineTo(hdc, cx + 2, cy - 8);
        MoveToEx(hdc, cx + 8, cy - 8, NULL); LineTo(hdc, cx + 8, cy - 2); break;
    case 4: // Undo
        MoveToEx(hdc, cx + 6, cy - 4, NULL); LineTo(hdc, cx - 6, cy - 4); LineTo(hdc, cx - 2, cy - 8); break;
    case 5: // Save
        Rectangle(hdc, cx - 8, cy - 8, cx + 8, cy + 8);
        Rectangle(hdc, cx - 4, cy - 8, cx + 4, cy - 2);
        Rectangle(hdc, cx - 5, cy + 2, cx + 5, cy + 6); break;
    case 6: // Cancel
        MoveToEx(hdc, cx - 6, cy - 6, NULL); LineTo(hdc, cx + 6, cy + 6);
        MoveToEx(hdc, cx + 6, cy - 6, NULL); LineTo(hdc, cx - 6, cy + 6); break;
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void draw_toolbar(HDC hdc) {
    RECT rc = { 0, 0, g_screenW, TOOLBAR_H };
    HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    for (int i = 0; i < (int)BTN_COUNT; i++) {
        RECT br = { g_btns[i].x, 10, g_btns[i].x + g_btns[i].w, 10 + BTN_H };
        BOOL active = (i < 4 && i == g_tool);
        HBRUSH bb = CreateSolidBrush(active ? RGB(0, 122, 204) : RGB(70, 70, 70));
        FillRect(hdc, &br, bb);
        DeleteObject(bb);
        draw_icon(hdc, i, g_btns[i].x + g_btns[i].w / 2, 10 + BTN_H / 2);
    }
    RECT info = { 520, 10, g_screenW - 10, 10 + BTN_H };
    SetTextColor(hdc, RGB(200, 200, 200));
    DrawTextA(hdc, "1-4=Tool  Ctrl+Z=Undo  Enter=Save  Esc=Cancel", -1, &info, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void draw_shape(HDC hdc, const Shape* s) {
    switch (s->type) {
    case TOOL_LINE: MoveToEx(hdc, s->a.x, s->a.y, NULL); LineTo(hdc, s->b.x, s->b.y); break;
    case TOOL_RECT: Rectangle(hdc, s->a.x, s->a.y, s->b.x, s->b.y); break;
    case TOOL_ELLIPSE: Ellipse(hdc, s->a.x, s->a.y, s->b.x, s->b.y); break;
    case TOOL_ARROW:
        MoveToEx(hdc, s->a.x, s->a.y, NULL); LineTo(hdc, s->b.x, s->b.y);
        double ang = atan2((double)(s->b.y - s->a.y), (double)(s->b.x - s->a.x));
        for (int i = -1; i <= 1; i += 2) {
            double t = ang + i * 0.45;
            MoveToEx(hdc, s->b.x, s->b.y, NULL);
            LineTo(hdc, s->b.x - (int)(16 * cos(t)), s->b.y - (int)(16 * sin(t)));
        } break;
    }
}

int get_encoder_clsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(size);
    if (!pInfo) return -1;
    GetImageEncoders(num, size, pInfo);
    int result = -1;
    for (UINT j = 0; j < num; j++) {
        if (wcscmp(pInfo[j].MimeType, format) == 0) {
            *pClsid = pInfo[j].Clsid; result = (int)j; break;
        }
    }
    free(pInfo);
    return result;
}

void save_png(HBITMAP hBmp) {
    char desktop[MAX_PATH] = "C:\\";
    if (!SHGetSpecialFolderPathA(NULL, desktop, CSIDL_DESKTOPDIRECTORY, FALSE))
        lstrcpynA(desktop, "C:\\", MAX_PATH);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    wsprintfA(path, "%s\\Screenshot_%04d%02d%02d_%02d%02d%02d.png",
              desktop, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    Bitmap bmp(hBmp, NULL);
    CLSID clsid;
    if (get_encoder_clsid(L"image/png", &clsid) >= 0)
        bmp.Save(wpath, &clsid, NULL);
}

void save_and_copy() {
    if (!g_screenDC) return;
    HDC hdc   = GetDC(NULL);
    HDC hMem  = CreateCompatibleDC(hdc);
    HBITMAP hOut = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
    HGDIOBJ old = SelectObject(hMem, hOut);

    BitBlt(hMem, 0, 0, g_screenW, g_screenH, g_screenDC, 0, 0, SRCCOPY);

    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 0, 0));
    HGDIOBJ oldPen   = SelectObject(hMem, pen);
    HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
    for (int i = 0; i < g_shapeCount; i++) draw_shape(hMem, &g_shapes[i]);
    SelectObject(hMem, oldPen); SelectObject(hMem, oldBrush); DeleteObject(pen);

    save_png(hOut);

    if (OpenClipboard(g_editWnd)) {
        EmptyClipboard();
        HBITMAP hCopy = (HBITMAP)CopyImage(hOut, IMAGE_BITMAP, 0, 0, 0);
        if (hCopy) SetClipboardData(CF_BITMAP, hCopy);
        CloseClipboard();
    }

    SelectObject(hMem, old);
    DeleteObject(hOut);
    DeleteDC(hMem);
    ReleaseDC(NULL, hdc);
}

void close_editor() {
    if (g_editWnd) { DestroyWindow(g_editWnd); g_editWnd = NULL; }
    if (g_screenBmp) { DeleteObject(g_screenBmp); g_screenBmp = NULL; }
    if (g_screenDC)  { DeleteDC(g_screenDC);     g_screenDC  = NULL; }
    if (g_showMouse) { g_showMouse = FALSE; while(ShowCursor(TRUE) < 0); }
}

void save_and_close() {
    if (g_saving) return;
    g_saving = TRUE;
    save_and_copy();
    close_editor();
    g_saving = FALSE;
}

void paint_edit(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!g_screenDC) { EndPaint(hwnd, &ps); return; }

    HDC hMem = CreateCompatibleDC(hdc);
    HBITMAP hBuf = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
    HGDIOBJ oldBuf = SelectObject(hMem, hBuf);

    BitBlt(hMem, 0, 0, g_screenW, g_screenH, g_screenDC, 0, 0, SRCCOPY);

    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 0, 0));
    HGDIOBJ oldPen   = SelectObject(hMem, pen);
    HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
    for (int i = 0; i < g_shapeCount; i++) draw_shape(hMem, &g_shapes[i]);
    if (g_dragging) {
        Shape tmp = { g_tool, g_dragStart, g_dragEnd };
        draw_shape(hMem, &tmp);
    }
    SelectObject(hMem, oldPen); SelectObject(hMem, oldBrush); DeleteObject(pen);

    draw_toolbar(hMem);

    // 放大镜与 1 像素准星
    if (g_showMouse) {
        int magSize = 120;
        int magX = g_mouseX + 20;
        int magY = g_mouseY + 20;
        if (magX + magSize > g_screenW) magX = g_mouseX - 20 - magSize;
        if (magY + magSize > g_screenH) magY = g_mouseY - 20 - magSize;
        
        int srcSize = magSize / 4;
        int srcX = g_mouseX - srcSize / 2;
        int srcY = g_mouseY - srcSize / 2;

        SetStretchBltMode(hMem, COLORONCOLOR);
        StretchBlt(hMem, magX, magY, magSize, magSize, g_screenDC, srcX, srcY, srcSize, srcSize, SRCCOPY);
        
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HPEN crossPen  = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
        oldPen = SelectObject(hMem, borderPen);
        oldBrush = SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hMem, magX, magY, magX+magSize, magY+magSize);
        
        SelectObject(hMem, crossPen);
        int cx = magX + magSize / 2, cy = magY + magSize / 2;
        MoveToEx(hMem, cx - 15, cy, NULL); LineTo(hMem, cx + 15, cy);
        MoveToEx(hMem, cx, cy - 15, NULL); LineTo(hMem, cx, cy + 15);
        
        SelectObject(hMem, oldPen); SelectObject(hMem, oldBrush);
        DeleteObject(borderPen); DeleteObject(crossPen);
        
        HPEN thinPen = CreatePen(PS_SOLID, 1, RGB(0, 255, 0));
        SelectObject(hMem, thinPen);
        MoveToEx(hMem, g_mouseX, 0, NULL); LineTo(hMem, g_mouseX, g_screenH);
        MoveToEx(hMem, 0, g_mouseY, NULL); LineTo(hMem, g_screenW, g_mouseY);
        SelectObject(hMem, oldPen);
        DeleteObject(thinPen);
    }

    BitBlt(hdc, 0, 0, g_screenW, g_screenH, hMem, 0, 0, SRCCOPY);

    SelectObject(hMem, oldBuf);
    DeleteObject(hBuf);
    DeleteDC(hMem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK EditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:      paint_edit(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (y < TOOLBAR_H) {
            for (int i = 0; i < (int)BTN_COUNT; i++) {
                if (x >= g_btns[i].x && x <= g_btns[i].x + g_btns[i].w) {
                    if (i < 4) { g_tool = i; InvalidateRect(hwnd, NULL, FALSE); }
                    else if (i == 4) { if (g_shapeCount > 0) g_shapeCount--; InvalidateRect(hwnd, NULL, FALSE); }
                    else if (i == 5) { save_and_close(); }
                    else if (i == 6) { close_editor(); }
                    return 0;
                }
            }
            return 0;
        }
        SetCapture(hwnd);
        g_dragging = TRUE;
        g_dragStart.x = x; g_dragStart.y = y;
        g_dragEnd = g_dragStart;
        return 0;
    }
    case WM_MOUSEMOVE: {
        g_mouseX = GET_X_LPARAM(lParam);
        g_mouseY = GET_Y_LPARAM(lParam);
        if (g_dragging) {
            g_dragEnd.x = g_mouseX; g_dragEnd.y = g_mouseY;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_dragging) {
            ReleaseCapture();
            g_dragging = FALSE;
            if (abs(g_dragEnd.x - g_dragStart.x) > 3 || abs(g_dragEnd.y - g_dragStart.y) > 3) {
                if (g_shapeCount < MAX_SHAPES) {
                    g_shapes[g_shapeCount].type = g_tool;
                    g_shapes[g_shapeCount].a = g_dragStart;
                    g_shapes[g_shapeCount].b = g_dragEnd;
                    g_shapeCount++;
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_RETURN) { save_and_close(); return 0; }
        if (wParam == VK_ESCAPE) { close_editor();   return 0; }
        if (wParam >= '1' && wParam <= '4') { g_tool = wParam - '1'; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if ((wParam == 'Z' || wParam == 'z') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (g_shapeCount > 0) g_shapeCount--;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: close_editor(); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void begin_capture() {
    if (g_editWnd) return;

    HDC hScreen = GetDC(NULL);
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);
    g_screenDC  = CreateCompatibleDC(hScreen);
    g_screenBmp = CreateCompatibleBitmap(hScreen, g_screenW, g_screenH);
    SelectObject(g_screenDC, g_screenBmp);
    BitBlt(g_screenDC, 0, 0, g_screenW, g_screenH, hScreen, 0, 0, SRCCOPY);
    ReleaseDC(NULL, hScreen);

    g_shapeCount = 0;
    g_dragging = FALSE;
    g_tool = TOOL_LINE;
    g_showMouse = TRUE;
    while(ShowCursor(FALSE) >= 0);

    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA(NULL);
    g_editWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "ShotEditCls", "Screenshot", WS_POPUP,
        0, 0, g_screenW, g_screenH, NULL, NULL, hInst, NULL);
    if (!g_editWnd) { close_editor(); return; }

    ShowWindow(g_editWnd, SW_SHOW);
    SetForegroundWindow(g_editWnd);
    SetFocus(g_editWnd);
    InvalidateRect(g_editWnd, NULL, FALSE);
}

void init_tray(HWND hwnd) {
    g_trayIcon = CreateTargetIcon(16, RGB(255, 50, 50));
    
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_trayIcon;
    lstrcpy(g_nid.szTip, "极简截图 (Ctrl+Shift+4)");
    
    g_nid.dwInfoFlags = NIIF_INFO;
    lstrcpy(g_nid.szInfoTitle, "极简截图已启动");
    lstrcpy(g_nid.szInfo, "快捷键: Ctrl+Shift+4\n右键托盘图标可以退出");
    
    Shell_NotifyIcon(NIM_ADD, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && wParam == HOTKEY_ID) { begin_capture(); return 0; }
    
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, 1001, "关于");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hMenu, MF_STRING, 1002, "退出");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            
            if (cmd == 1001) {
                MessageBoxA(NULL, "极简截图 by zsDili", "关于", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == 1002) {
                HANDLE hEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, "TinyShotQuitEvent");
                if (hEvent) { SetEvent(hEvent); CloseHandle(hEvent); }
                Shell_NotifyIcon(NIM_DELETE, &g_nid);
                DestroyIcon(g_trayIcon);
                PostQuitMessage(0);
            }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void set_auto_start() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        RegSetValueExA(hKey, "TinyShot", 0, REG_SZ, (const BYTE*)path, (DWORD)(lstrlenA(path) + 1));
        RegCloseKey(hKey);
    }
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* p) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    lstrcatA(path, "tinyshot_crash.log");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        const char* msg = "TinyShot crashed. Watchdog will restart.\r\n";
        WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
        CloseHandle(h);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int watchdog_main() {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    
    HANDLE hQuitEvent = CreateEventA(NULL, TRUE, FALSE, "TinyShotQuitEvent");
    
    for (;;) {
        char cmd[MAX_PATH + 32];
        wsprintfA(cmd, "\"%s\" --noguard", self);
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
            if (WaitForSingleObject(hQuitEvent, 0) == WAIT_OBJECT_0) break;
            Sleep(1500);
        } else {
            Sleep(5000);
        }
    }
    CloseHandle(hQuitEvent);
    return 0;
}

void start_watchdog() {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    char cmd[MAX_PATH + 32];
    wsprintfA(cmd, "\"%s\" --watchdog", self);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static int run_message_loop() {
    MSG msg;
    __try {
        while (GetMessageA(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    if (cmdLine && strstr(cmdLine, "--watchdog")) return watchdog_main();

    SetUnhandledExceptionFilter(TopLevelFilter);

    HANDLE hMutex = CreateMutexA(NULL, TRUE, "TinyShotMutex_v3");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    if (!cmdLine || !strstr(cmdLine, "--noguard")) start_watchdog();

    GdiplusStartupInput gdiSI;
    if (GdiplusStartup(&g_gdiplusToken, &gdiSI, NULL) != Ok) return 1;

    set_auto_start();

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "TinyShotMsgCls";
    RegisterClassA(&wc);

    WNDCLASSA we = {0};
    we.lpfnWndProc = EditWndProc;
    we.hInstance = hInst;
    we.hCursor = LoadCursor(NULL, IDC_ARROW);
    we.lpszClassName = "ShotEditCls";
    we.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&we);

    g_msgWnd = CreateWindowExA(0, "TinyShotMsgCls", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_msgWnd) { GdiplusShutdown(g_gdiplusToken); return 1; }

    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, '4')) {
        MessageBoxA(NULL, "Ctrl+Shift+4 register failed.", "TinyShot", MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    init_tray(g_msgWnd);

    run_message_loop();

    UnregisterHotKey(g_msgWnd, HOTKEY_ID);
    GdiplusShutdown(g_gdiplusToken);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
