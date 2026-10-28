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

#define HOTKEY_ID   1
#define MAX_SHAPES  512
#define TOOLBAR_H   60
#define BTN_H       40

enum { TOOL_LINE = 0, TOOL_RECT, TOOL_ELLIPSE, TOOL_ARROW };

struct Shape { int type; POINT a, b; };

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

struct BtnDef { int x; int w; const char* text; };
static const BtnDef g_btns[] = {
    {  10,  70, "Line" },
    {  90,  70, "Rect" },
    { 170,  70, "Ellipse" },
    { 250,  70, "Arrow" },
    { 340,  80, "Undo" },
    { 430,  80, "Save" },
    { 520,  80, "Cancel" },
};
#define BTN_COUNT (sizeof(g_btns) / sizeof(g_btns[0]))

// ---------- 绘制 ----------
void draw_arrow(HDC hdc, POINT a, POINT b) {
    MoveToEx(hdc, a.x, a.y, NULL);
    LineTo(hdc, b.x, b.y);
    double ang = atan2((double)(b.y - a.y), (double)(b.x - a.x));
    int len = 16;
    double spread = 0.45;
    for (int i = -1; i <= 1; i += 2) {
        double t = ang + i * spread;
        int x = b.x - (int)(len * cos(t));
        int y = b.y - (int)(len * sin(t));
        MoveToEx(hdc, b.x, b.y, NULL);
        LineTo(hdc, x, y);
    }
}

void draw_shape(HDC hdc, const Shape* s) {
    switch (s->type) {
    case TOOL_LINE:
        MoveToEx(hdc, s->a.x, s->a.y, NULL);
        LineTo(hdc, s->b.x, s->b.y);
        break;
    case TOOL_RECT:
        Rectangle(hdc, s->a.x, s->a.y, s->b.x, s->b.y);
        break;
    case TOOL_ELLIPSE:
        Ellipse(hdc, s->a.x, s->a.y, s->b.x, s->b.y);
        break;
    case TOOL_ARROW:
        draw_arrow(hdc, s->a, s->b);
        break;
    }
}

void draw_toolbar(HDC hdc) {
    RECT rc = { 0, 0, g_screenW, TOOLBAR_H };
    HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        RECT br = { g_btns[i].x, 10, g_btns[i].x + g_btns[i].w, 10 + BTN_H };
        BOOL active = (i < 4 && i == g_tool);
        HBRUSH bb = CreateSolidBrush(active ? RGB(0, 122, 204) : RGB(70, 70, 70));
        FillRect(hdc, &br, bb);
        DeleteObject(bb);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextA(hdc, g_btns[i].text, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT info = { 620, 10, g_screenW - 10, 10 + BTN_H };
    SetTextColor(hdc, RGB(200, 200, 200));
    DrawTextA(hdc, "1-4=Tool  Ctrl+Z=Undo  Enter=Save  Esc=Cancel",
              -1, &info, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ---------- PNG 编码 ----------
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
            *pClsid = pInfo[j].Clsid;
            result = (int)j;
            break;
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
              desktop, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);

    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    Bitmap bmp(hBmp, NULL);
    CLSID clsid;
    if (get_encoder_clsid(L"image/png", &clsid) >= 0)
        bmp.Save(wpath, &clsid, NULL);
}

// ---------- 合成 + 保存 + 剪贴板 ----------
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
    SelectObject(hMem, oldPen);
    SelectObject(hMem, oldBrush);
    DeleteObject(pen);

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
}

void save_and_close() {
    if (g_saving) return;
    g_saving = TRUE;
    save_and_copy();
    close_editor();
    g_saving = FALSE;
}

// ---------- 编辑窗口绘制 ----------
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
    SelectObject(hMem, oldPen);
    SelectObject(hMem, oldBrush);
    DeleteObject(pen);

    draw_toolbar(hMem);
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
        if (g_dragging) {
            g_dragEnd.x = GET_X_LPARAM(lParam);
            g_dragEnd.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_dragging) {
            ReleaseCapture();
            g_dragging = FALSE;
            if (abs(g_dragEnd.x - g_dragStart.x) > 3 ||
                abs(g_dragEnd.y - g_dragStart.y) > 3) {
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
        if (wParam == '1') { g_tool = TOOL_LINE;    InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wParam == '2') { g_tool = TOOL_RECT;    InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wParam == '3') { g_tool = TOOL_ELLIPSE; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wParam == '4') { g_tool = TOOL_ARROW;   InvalidateRect(hwnd, NULL, FALSE); return 0; }
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

// ---------- 触发热键：抓屏 + 开编辑窗 ----------
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

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && wParam == HOTKEY_ID) { begin_capture(); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------- 自启动 / 异常 / 看门狗 ----------
void set_auto_start() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        RegSetValueExA(hKey, "TinyShot", 0, REG_SZ,
                       (const BYTE*)path, (DWORD)(lstrlenA(path) + 1));
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
    for (;;) {
        char cmd[MAX_PATH + 32];
        wsprintfA(cmd, "\"%s\" --noguard", self);
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            Sleep(1500);
        } else {
            Sleep(5000);
        }
    }
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
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    if (cmdLine && strstr(cmdLine, "--watchdog")) return watchdog_main();

    SetUnhandledExceptionFilter(TopLevelFilter);

    // 单实例：已有主进程则直接退出，避免看门狗互相拉扯
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "TinyShotMutex_v2");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    // 只有正常启动才派生看门狗
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
    we.hCursor = LoadCursor(NULL, IDC_CROSS);
    we.lpszClassName = "ShotEditCls";
    we.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&we);

    g_msgWnd = CreateWindowExA(0, "TinyShotMsgCls", "", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_msgWnd) { GdiplusShutdown(g_gdiplusToken); return 1; }

    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, '4')) {
        MessageBoxA(NULL, "Ctrl+Shift+4 register failed.", "TinyShot", MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    run_message_loop();   // 崩溃自动落回看门狗重启

    UnregisterHotKey(g_msgWnd, HOTKEY_ID);
    GdiplusShutdown(g_gdiplusToken);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
