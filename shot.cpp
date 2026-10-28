#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <gdipluspath.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

#define HOTKEY_ID     1
#define WM_TRAYICON   (WM_APP + 1)
#define MAX_SHAPES    512
#define BTN_SIZE      36
#define BTN_GAP       12

enum { TOOL_ARROW = 0, TOOL_RECT, TOOL_ELLIPSE, TOOL_LINE, TOOL_UNDO, TOOL_SAVE, TOOL_CANCEL };

struct Shape { int type; Point a, b; };
static const int g_btns[] = { TOOL_ARROW, TOOL_RECT, TOOL_ELLIPSE, TOOL_LINE, TOOL_UNDO, TOOL_SAVE, TOOL_CANCEL };
#define BTN_COUNT (sizeof(g_btns) / sizeof(g_btns[0]))

static HBITMAP g_screenBmp = NULL;
static HDC     g_screenDC  = NULL;
static HBITMAP g_staticBmp = NULL;
static HDC     g_staticDC  = NULL;
static Bitmap* g_screenGpBmp = NULL; // 缓存用于放大镜的 GDI+ 位图，防泄漏
static int     g_screenW = 0, g_screenH = 0;

static Shape   g_shapes[MAX_SHAPES];
static int     g_shapeCount = 0;
static int     g_tool = TOOL_ARROW;
static BOOL    g_dragging = FALSE;
static Point   g_dragStart, g_dragEnd;
static HWND    g_msgWnd  = NULL;
static HWND    g_editWnd = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static BOOL    g_saving = FALSE;
static int     g_hoverBtn = -1;

static int     g_mouseX = 0, g_mouseY = 0;
static BOOL    g_showMouse = FALSE;
static NOTIFYICONDATA g_nid = {0};
static HICON   g_trayIcon = NULL;

// --- 补全 GDI+ 缺失的圆角矩形绘制函数 ---
void DrawRoundRect(Graphics& g, Pen* pen, RectF rect, float radius) {
    GraphicsPath path;
    float diameter = radius * 2;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    g.DrawPath(pen, &path);
}

void FillRoundRect(Graphics& g, Brush* brush, RectF rect, float radius) {
    GraphicsPath path;
    float diameter = radius * 2;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    g.FillPath(brush, &path);
}
// -----------------------------------------

// 绘制苹果风图标
void DrawMacIcon(Graphics& g, int type, RectF rect, Color color) {
    Pen pen(color, 1.5f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    
    float cx = rect.X + rect.Width / 2.0f;
    float cy = rect.Y + rect.Height / 2.0f;
    float s = 8.0f;

    switch(type) {
    case TOOL_ARROW:
        g.DrawLine(&pen, cx - s, cy + s, cx + s*0.6f, cy - s*0.6f);
        g.DrawLine(&pen, cx + s*0.6f, cy - s*0.6f, cx - s*0.1f, cy - s*0.1f);
        g.DrawLine(&pen, cx + s*0.6f, cy - s*0.6f, cx + s*0.1f, cy + s*0.2f);
        break;
    case TOOL_RECT:
        g.DrawRectangle(&pen, cx - s, cy - s*0.7f, s*2, s*1.4f); 
        break;
    case TOOL_ELLIPSE:
        g.DrawEllipse(&pen, cx - s, cy - s*0.7f, s*2, s*1.4f); 
        break;
    case TOOL_LINE:
        g.DrawLine(&pen, cx - s, cy + s, cx + s, cy - s); 
        break;
    case TOOL_UNDO: {
        g.DrawArc(&pen, cx - s, cy - s*0.6f, s*1.8f, s*1.8f, 180.0f, 270.0f);
        PointF pts[] = { PointF(cx - s, cy + s*0.2f), PointF(cx - s, cy - s*0.4f), PointF(cx - s*0.4f, cy + s*0.2f) };
        g.DrawLines(&pen, pts, 3);
        break;
    }
    case TOOL_SAVE:
        g.DrawLine(&pen, cx - s, cy, cx - s*0.2f, cy + s*0.8f);
        g.DrawLine(&pen, cx - s*0.2f, cy + s*0.8f, cx + s, cy - s*0.8f);
        break;
    case TOOL_CANCEL:
        g.DrawLine(&pen, cx - s*0.8f, cy - s*0.8f, cx + s*0.8f, cy + s*0.8f);
        g.DrawLine(&pen, cx + s*0.8f, cy - s*0.8f, cx - s*0.8f, cy + s*0.8f);
        break;
    }
}

// 底部居中工具栏
void draw_toolbar(Graphics& g) {
    int totalW = BTN_COUNT * BTN_SIZE + (BTN_COUNT - 1) * BTN_GAP + 32;
    int startX = (g_screenW - totalW) / 2;
    int startY = g_screenH - 80;
    
    RectF bgRect((float)startX, (float)startY, (float)totalW, 56.0f);
    
    SolidBrush bgBrush(Color(220, 30, 30, 30));
    FillRoundRect(g, &bgBrush, bgRect, 28.0f); // 修复调用
    Pen borderPen(Color(40, 255, 255, 255), 1.0f);
    DrawRoundRect(g, &borderPen, bgRect, 28.0f); // 修复调用

    for (int i = 0; i < (int)BTN_COUNT; i++) {
        RectF btnRect((float)(startX + 16 + i * (BTN_SIZE + BTN_GAP)), (float)(startY + 10), (float)BTN_SIZE, (float)BTN_SIZE);
        
        if (i == g_hoverBtn) {
            SolidBrush hoverBrush(Color(40, 255, 255, 255));
            FillRoundRect(g, &hoverBrush, btnRect, 8.0f); // 修复调用
        }

        Color iconColor;
        if (i < 4) {
            iconColor = (i == g_tool) ? Color(255, 80, 160, 255) : Color(255, 230, 230, 230);
        } else if (i == 5) {
            iconColor = Color(255, 80, 200, 120);
        } else {
            iconColor = Color(255, 230, 230, 230);
        }
        DrawMacIcon(g, g_btns[i], btnRect, iconColor);
    }
}

void draw_shape(Graphics& g, const Shape* s) {
    Pen pen(Color(255, 255, 60, 60), 2.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    
    switch (s->type) {
    case TOOL_LINE: g.DrawLine(&pen, s->a, s->b); break;
    case TOOL_RECT: g.DrawRectangle(&pen, std::min(s->a.X, s->b.X), std::min(s->a.Y, s->b.Y), abs(s->b.X - s->a.X), abs(s->b.Y - s->a.Y)); break;
    case TOOL_ELLIPSE: g.DrawEllipse(&pen, std::min(s->a.X, s->b.X), std::min(s->a.Y, s->b.Y), abs(s->b.X - s->a.X), abs(s->b.Y - s->a.Y)); break;
    case TOOL_ARROW: {
        g.DrawLine(&pen, s->a, s->b);
        double ang = atan2((double)(s->b.Y - s->a.Y), (double)(s->b.X - s->a.X));
        for (int i = -1; i <= 1; i += 2) {
            double t = ang + i * 0.45;
            g.DrawLine(&pen, s->b.X, s->b.Y, s->b.X - (int)(14 * cos(t)), s->b.Y - (int)(14 * sin(t)));
        }
        break;
    }
    }
}

void update_static_layer() {
    Graphics g(g_staticDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    BitBlt(g_staticDC, 0, 0, g_screenW, g_screenH, g_screenDC, 0, 0, SRCCOPY);
    for (int i = 0; i < g_shapeCount; i++) draw_shape(g, &g_shapes[i]);
}

void save_png(HBITMAP hBmp) {
    char desktop[MAX_PATH] = "C:\\";
    SHGetSpecialFolderPathA(NULL, desktop, CSIDL_DESKTOPDIRECTORY, FALSE);
    SYSTEMTIME st; GetLocalTime(&st);
    char path[MAX_PATH];
    wsprintfA(path, "%s\\Screenshot_%04d%02d%02d_%02d%02d%02d.png", desktop, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    WCHAR wpath[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    
    Bitmap bmp(hBmp, NULL);
    CLSID clsid;
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(size);
    GetImageEncoders(num, size, pInfo);
    for (UINT j = 0; j < num; j++) {
        if (wcscmp(pInfo[j].MimeType, L"image/png") == 0) { clsid = pInfo[j].Clsid; break; }
    }
    free(pInfo);
    bmp.Save(wpath, &clsid, NULL);
}

void save_and_copy() {
    if (!g_staticDC) return;
    HDC hdc = GetDC(NULL);
    HBITMAP hOut = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
    HDC hMem = CreateCompatibleDC(hdc);
    SelectObject(hMem, hOut);
    BitBlt(hMem, 0, 0, g_screenW, g_screenH, g_staticDC, 0, 0, SRCCOPY);
    
    if (g_dragging) {
        Graphics g(hMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Shape tmp = { g_tool, g_dragStart, g_dragEnd };
        draw_shape(g, &tmp);
    }

    save_png(hOut);

    if (OpenClipboard(g_editWnd)) {
        EmptyClipboard();
        HBITMAP hCopy = (HBITMAP)CopyImage(hOut, IMAGE_BITMAP, 0, 0, 0);
        if (hCopy) SetClipboardData(CF_BITMAP, hCopy);
        CloseClipboard();
    }
    SelectObject(hMem, NULL); DeleteObject(hOut); DeleteDC(hMem); ReleaseDC(NULL, hdc);
}

void close_editor() {
    if (g_editWnd) { DestroyWindow(g_editWnd); g_editWnd = NULL; }
    if (g_screenBmp) { DeleteObject(g_screenBmp); g_screenBmp = NULL; }
    if (g_screenDC) { DeleteDC(g_screenDC); g_screenDC = NULL; }
    if (g_staticBmp) { DeleteObject(g_staticBmp); g_staticBmp = NULL; }
    if (g_staticDC) { DeleteDC(g_staticDC); g_staticDC = NULL; }
    if (g_screenGpBmp) { delete g_screenGpBmp; g_screenGpBmp = NULL; }
    if (g_showMouse) { g_showMouse = FALSE; while(ShowCursor(TRUE) < 0); }
}

void save_and_close() {
    if (g_saving) return; g_saving = TRUE;
    save_and_copy(); close_editor(); g_saving = FALSE;
}

void paint_edit(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!g_staticDC) { EndPaint(hwnd, &ps); return; }

    HDC hMem = CreateCompatibleDC(hdc);
    HBITMAP hBuf = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
    HGDIOBJ oldBuf = SelectObject(hMem, hBuf);

    BitBlt(hMem, 0, 0, g_screenW, g_screenH, g_staticDC, 0, 0, SRCCOPY);

    Graphics g(hMem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    if (g_dragging) {
        Shape tmp = { g_tool, g_dragStart, g_dragEnd };
        draw_shape(g, &tmp);
    }

    draw_toolbar(g);

    if (g_showMouse) {
        int magSize = 120;
        int magX = g_mouseX + 24;
        int magY = g_mouseY + 24;
        if (magX + magSize > g_screenW) magX = g_mouseX - 24 - magSize;
        if (magY + magSize > g_screenH) magY = g_mouseY - 24 - magSize;

        int srcSize = magSize / 6;
        int srcX = g_mouseX - srcSize / 2;
        int srcY = g_mouseY - srcSize / 2;

        GraphicsPath magPath;
        magPath.AddEllipse(magX, magY, magSize, magSize);
        g.SetClip(&magPath);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        
        if (g_screenGpBmp) {
            g.DrawImage(g_screenGpBmp, Rect(magX, magY, magSize, magSize), srcX, srcY, srcSize, srcSize, UnitPixel);
        }
        
        g.ResetClip();
        
        Pen borderPen(Color(255, 255, 255, 255), 2.0f);
        g.DrawEllipse(&borderPen, magX, magY, magSize, magSize);
        
        int cx = magX + magSize / 2, cy = magY + magSize / 2;
        Pen crossPen(Color(255, 255, 60, 60), 1.0f);
        g.DrawLine(&crossPen, cx - 10, cy, cx + 10, cy);
        g.DrawLine(&crossPen, cx, cy - 10, cx, cy + 10);

        Pen screenCrossPen(Color(200, 0, 255, 255), 1.0f);
        g.DrawLine(&screenCrossPen, g_mouseX, 0, g_mouseX, g_screenH);
        g.DrawLine(&screenCrossPen, 0, g_mouseY, g_screenW, g_mouseY);
    }

    BitBlt(hdc, 0, 0, g_screenW, g_screenH, hMem, 0, 0, SRCCOPY);

    SelectObject(hMem, oldBuf); DeleteObject(hBuf); DeleteDC(hMem);
    EndPaint(hwnd, &ps);
}

int GetHoverBtn(int x, int y) {
    int totalW = BTN_COUNT * BTN_SIZE + (BTN_COUNT - 1) * BTN_GAP + 32;
    int startX = (g_screenW - totalW) / 2;
    int startY = g_screenH - 80;
    if (y >= startY + 10 && y <= startY + 10 + BTN_SIZE) {
        for (int i = 0; i < (int)BTN_COUNT; i++) {
            int bx = startX + 16 + i * (BTN_SIZE + BTN_GAP);
            if (x >= bx && x <= bx + BTN_SIZE) return i;
        }
    }
    return -1;
}

LRESULT CALLBACK EditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: paint_edit(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;

    case WM_MOUSEMOVE: {
        g_mouseX = GET_X_LPARAM(lParam); g_mouseY = GET_Y_LPARAM(lParam);
        if (g_dragging) { g_dragEnd.X = g_mouseX; g_dragEnd.Y = g_mouseY; }
        int hover = GetHoverBtn(g_mouseX, g_mouseY);
        if (hover != g_hoverBtn) g_hoverBtn = hover;
        InvalidateRect(hwnd, NULL, FALSE); 
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int btn = GetHoverBtn(x, y);
        if (btn != -1) {
            if (btn < 4) { g_tool = btn; }
            else if (btn == 4) { if (g_shapeCount > 0) { g_shapeCount--; update_static_layer(); } }
            else if (btn == 5) { save_and_close(); return 0; }
            else if (btn == 6) { close_editor(); return 0; }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        SetCapture(hwnd); g_dragging = TRUE;
        g_dragStart.X = x; g_dragStart.Y = y; g_dragEnd = g_dragStart;
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_dragging) {
            ReleaseCapture(); g_dragging = FALSE;
            if (abs(g_dragEnd.X - g_dragStart.X) > 3 || abs(g_dragEnd.Y - g_dragStart.Y) > 3) {
                if (g_shapeCount < MAX_SHAPES) {
                    g_shapes[g_shapeCount].type = g_tool;
                    g_shapes[g_shapeCount].a = g_dragStart;
                    g_shapes[g_shapeCount].b = g_dragEnd;
                    g_shapeCount++; update_static_layer();
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_RETURN) { save_and_close(); return 0; }
        if (wParam == VK_ESCAPE) { close_editor(); return 0; }
        if (wParam >= '1' && wParam <= '4') { g_tool = wParam - '1'; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if ((wParam == 'Z' || wParam == 'z') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (g_shapeCount > 0) { g_shapeCount--; update_static_layer(); }
            InvalidateRect(hwnd, NULL, FALSE); return 0;
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
    g_screenW = GetSystemMetrics(SM_CXSCREEN); g_screenH = GetSystemMetrics(SM_CYSCREEN);
    
    g_screenDC = CreateCompatibleDC(hScreen);
    g_screenBmp = CreateCompatibleBitmap(hScreen, g_screenW, g_screenH);
    SelectObject(g_screenDC, g_screenBmp);
    BitBlt(g_screenDC, 0, 0, g_screenW, g_screenH, hScreen, 0, 0, SRCCOPY);

    g_staticDC = CreateCompatibleDC(hScreen);
    g_staticBmp = CreateCompatibleBitmap(hScreen, g_screenW, g_screenH);
    SelectObject(g_staticDC, g_staticBmp);
    
    g_screenGpBmp = Bitmap::FromHBITMAP(g_screenBmp, NULL);

    ReleaseDC(NULL, hScreen);

    g_shapeCount = 0; g_dragging = FALSE; g_tool = TOOL_ARROW; g_showMouse = TRUE; g_hoverBtn = -1;
    while(ShowCursor(FALSE) >= 0);
    update_static_layer();

    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA(NULL);
    g_editWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "ShotEditCls", "Screenshot", WS_POPUP, 0, 0, g_screenW, g_screenH, NULL, NULL, hInst, NULL);
    if (!g_editWnd) { close_editor(); return; }

    ShowWindow(g_editWnd, SW_SHOW); SetForegroundWindow(g_editWnd); SetFocus(g_editWnd);
    InvalidateRect(g_editWnd, NULL, FALSE);
}

HICON CreateTargetIcon(int size, Color color) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    
    Pen pen(color, 2.0f);
    float c = size / 2.0f;
    float r = size / 4.0f;
    g.DrawEllipse(&pen, c - r, c - r, r * 2, r * 2);
    
    pen.SetWidth(1.5f);
    g.DrawLine(&pen, c, 0.0f, c, c - r - 2);
    g.DrawLine(&pen, c, c + r + 2, c, (float)size);
    g.DrawLine(&pen, 0.0f, c, c - r - 2, c);
    g.DrawLine(&pen, c + r + 2, c, (float)size, c);
    
    HICON hIcon;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

void init_tray(HWND hwnd) {
    g_trayIcon = CreateTargetIcon(32, Color(255, 255, 80, 80));
    g_nid.cbSize = sizeof(NOTIFYICONDATA); g_nid.hWnd = hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON; g_nid.hIcon = g_trayIcon;
    lstrcpy(g_nid.szTip, "极简截图 (Ctrl+Shift+4)");
    g_nid.dwInfoFlags = NIIF_INFO;
    lstrcpy(g_nid.szInfoTitle, "极简截图已启动");
    lstrcpy(g_nid.szInfo, "按 Ctrl+Shift+4 截图\n右键图标可退出");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && wParam == HOTKEY_ID) { begin_capture(); return 0; }
    if (msg == WM_TRAYICON && lParam == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_STRING, 1001, "关于");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(hMenu, MF_STRING, 1002, "退出");
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
        if (cmd == 1001) MessageBoxA(NULL, "极简截图 by zsDili", "关于", MB_OK | MB_ICONINFORMATION);
        else if (cmd == 1002) {
            HANDLE hEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, "TinyShotQuitEvent");
            if (hEvent) { SetEvent(hEvent); CloseHandle(hEvent); }
            Shell_NotifyIcon(NIM_DELETE, &g_nid); DestroyIcon(g_trayIcon);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void set_auto_start() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
        RegSetValueExA(hKey, "TinyShot", 0, REG_SZ, (const BYTE*)path, (DWORD)(lstrlenA(path) + 1));
        RegCloseKey(hKey);
    }
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* p) {
    char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); lstrcatA(path, "tinyshot_crash.log");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, "Crash", 5, &wr, NULL); CloseHandle(h); }
    return EXCEPTION_EXECUTE_HANDLER;
}

int watchdog_main() {
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    HANDLE hQuitEvent = CreateEventA(NULL, TRUE, FALSE, "TinyShotQuitEvent");
    for (;;) {
        char cmd[MAX_PATH + 32]; wsprintfA(cmd, "\"%s\" --noguard", self);
        STARTUPINFOA si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            if (WaitForSingleObject(hQuitEvent, 0) == WAIT_OBJECT_0) break;
            Sleep(1500);
        } else Sleep(5000);
    }
    CloseHandle(hQuitEvent); return 0;
}

void start_watchdog() {
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    char cmd[MAX_PATH + 32]; wsprintfA(cmd, "\"%s\" --watchdog", self);
    STARTUPINFOA si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static int run_message_loop() {
    MSG msg;
    __try { while (GetMessageA(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    if (cmdLine && strstr(cmdLine, "--watchdog")) return watchdog_main();
    SetUnhandledExceptionFilter(TopLevelFilter);
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "TinyShotMutex_v5");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(hMutex); return 0; }
    if (!cmdLine || !strstr(cmdLine, "--noguard")) start_watchdog();
    
    GdiplusStartupInput gdiSI;
    if (GdiplusStartup(&g_gdiplusToken, &gdiSI, NULL) != Ok) return 1;
    set_auto_start();

    WNDCLASSA wc = {0}; wc.lpfnWndProc = MsgWndProc; wc.hInstance = hInst; wc.lpszClassName = "TinyShotMsgCls";
    RegisterClassA(&wc);
    WNDCLASSA we = {0}; we.lpfnWndProc = EditWndProc; we.hInstance = hInst; we.hCursor = LoadCursor(NULL, IDC_ARROW); 
    we.lpszClassName = "ShotEditCls"; we.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&we);

    g_msgWnd = CreateWindowExA(0, "TinyShotMsgCls", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_msgWnd) { GdiplusShutdown(g_gdiplusToken); return 1; }

    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, '4')) {
        MessageBoxA(NULL, "Ctrl+Shift+4 register failed.", "TinyShot", MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken); return 1;
    }

    init_tray(g_msgWnd);
    run_message_loop();
    
    UnregisterHotKey(g_msgWnd, HOTKEY_ID); GdiplusShutdown(g_gdiplusToken);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
