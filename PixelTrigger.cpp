// ================================================================
//  PixelTrigger.cpp  —  v2.0  |  Win32-only, C++17, VS-compatible
// ================================================================
//
// BUILD (Developer Command Prompt / PowerShell):
//   cl /O2 /std:c++17 /EHsc PixelTrigger.cpp /link user32.lib gdi32.lib
//
//   Or with MinGW / MSYS2:
//   g++ -O2 -std=c++17 PixelTrigger.cpp -o PixelTrigger.exe -luser32 -lgdi32
//
// USAGE:
//   Run PixelTrigger.exe — a full-screen terminal menu lets you
//   configure every setting live. The trigger loop runs in a
//   background thread while you navigate the menu.
//   Press F12 (or your configured exit key) at any time to quit.
//
// HOW IT WORKS:
//   1. Captures a small centred screen zone each scan cycle
//   2. If any pixel matches the target colour (± tolerance) → trigger
//   3. Waits for the cooldown, then sends a humanised key tap
//      (random pre-delay + variable hold + random repeat gap)
// ================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

// ================================================================
//  DEFAULT CONFIGURATION
// ================================================================

static int   CFG_ZONE_W         = 40;      // capture width  (px)
static int   CFG_ZONE_H         = 40;      // capture height (px)

static BYTE  CFG_R              = 255;     // target Red
static BYTE  CFG_G              = 0;       // target Green
static BYTE  CFG_B              = 0;       // target Blue
static int   CFG_TOLERANCE      = 20;      // per-channel ±

static int   CFG_SCAN_MS        = 1;       // ms between scans (0 = busy)
static int   CFG_COOLDOWN_MS    = 800;     // min ms between triggers
static int   CFG_DELAY_MIN_MS   = 20;      // human pre-press min
static int   CFG_DELAY_MAX_MS   = 60;      // human pre-press max
static int   CFG_HOLD_MIN_MS    = 40;      // key hold min
static int   CFG_HOLD_MAX_MS    = 90;      // key hold max

static WORD  CFG_VK             = VK_SPACE;
static WORD  CFG_EXIT_VK        = VK_F12;

// ================================================================
//  CONSOLE COLOUR CONSTANTS  (Win32 console attributes)
// ================================================================
#define CON_BLACK         0
#define CON_DARKBLUE      1
#define CON_DARKGREEN     2
#define CON_DARKCYAN      3
#define CON_DARKRED       4
#define CON_DARKMAGENTA   5
#define CON_DARKYELLOW    6
#define CON_GREY          7
#define CON_DARKGREY      8
#define CON_BLUE          9
#define CON_GREEN        10
#define CON_CYAN         11
#define CON_RED          12
#define CON_MAGENTA      13
#define CON_YELLOW       14
#define CON_WHITE        15

// ================================================================
//  CONSOLE HELPERS
// ================================================================

static HANDLE g_hOut = INVALID_HANDLE_VALUE;
static HANDLE g_hIn  = INVALID_HANDLE_VALUE;
static int    g_conW = 80;
static int    g_conH = 25;

static void conInit()
{
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);

    // Hide cursor
    CONSOLE_CURSOR_INFO ci{ 1, FALSE };
    SetConsoleCursorInfo(g_hOut, &ci);

    // Disable Quick-Edit so mouse clicks don't freeze stdout
    DWORD mode = 0;
    GetConsoleMode(g_hIn, &mode);
    SetConsoleMode(g_hIn, mode & ~ENABLE_QUICK_EDIT_MODE);

    // Maximise console window, give it a reasonable buffer
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(g_hOut, &csbi))
    {
        g_conW = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        g_conH = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    }
    // Ensure buffer >= window
    COORD sz{ (SHORT)max(g_conW, 120), (SHORT)max(g_conH, 30) };
    SetConsoleScreenBufferSize(g_hOut, sz);

    SetConsoleTextAttribute(g_hOut, CON_GREY);
}

static void conGoto(int x, int y)
{
    COORD c{ (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(g_hOut, c);
}

static void conColor(WORD attr)
{
    SetConsoleTextAttribute(g_hOut, attr);
}

static void conColorFB(int fg, int bg = CON_BLACK)
{
    SetConsoleTextAttribute(g_hOut, (WORD)((bg << 4) | fg));
}

static void conWrite(const char* s)
{
    DWORD w;
    WriteConsoleA(g_hOut, s, (DWORD)strlen(s), &w, nullptr);
}

static void conPrint(int x, int y, int fg, int bg, const char* s)
{
    conGoto(x, y);
    conColorFB(fg, bg);
    conWrite(s);
}

// Fill a rectangle with spaces in given colour
static void conFillRect(int x, int y, int w, int h, int fg, int bg)
{
    char line[256];
    int filled = min(w, 254);
    memset(line, ' ', filled);
    line[filled] = '\0';
    conColorFB(fg, bg);
    for (int row = y; row < y + h; ++row)
    {
        conGoto(x, row);
        conWrite(line);
    }
}

// Draw a single-line box  (ASCII compatible)
static void conBox(int x, int y, int w, int h, int fg, int bg,
                   const char* title = nullptr)
{
    char buf[256];
    conColorFB(fg, bg);

    // Top border
    conGoto(x, y);
    buf[0] = (char)218; // ┌
    for (int i = 1; i < w - 1; ++i) buf[i] = (char)196; // ─
    buf[w-1] = (char)191; // ┐
    buf[w]   = '\0';
    conWrite(buf);

    // Title centred on top border
    if (title)
    {
        int tlen = (int)strlen(title);
        int tx   = x + (w - tlen - 2) / 2 + 1;
        conGoto(tx, y);
        char tbuf[128];
        snprintf(tbuf, sizeof(tbuf), " %s ", title);
        conColorFB(CON_YELLOW, bg);
        conWrite(tbuf);
        conColorFB(fg, bg);
    }

    // Sides
    for (int row = y + 1; row < y + h - 1; ++row)
    {
        conGoto(x,         row); buf[0] = (char)179; buf[1] = '\0'; conWrite(buf); // │
        conGoto(x + w - 1, row); conWrite(buf);
    }

    // Bottom border
    conGoto(x, y + h - 1);
    buf[0] = (char)192; // └
    for (int i = 1; i < w - 1; ++i) buf[i] = (char)196;
    buf[w-1] = (char)217; // ┘
    buf[w]   = '\0';
    conWrite(buf);
}

static void conHLine(int x, int y, int w, int fg, int bg)
{
    conGoto(x, y); conColorFB(fg, bg);
    char buf[256];
    int cw = min(w, 254);
    for (int i = 0; i < cw; ++i) buf[i] = (char)196;
    buf[cw] = '\0';
    conWrite(buf);
}

// Clear the whole screen
static void conClear(int bg = CON_BLACK)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(g_hOut, &csbi);
    DWORD size = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD written;
    COORD origin{0,0};
    FillConsoleOutputCharacterA(g_hOut, ' ', size, origin, &written);
    FillConsoleOutputAttribute(g_hOut, (WORD)((bg << 4)), size, origin, &written);
    conGoto(0, 0);
}

// ================================================================
//  TRIGGER ENGINE
// ================================================================

static std::atomic<bool> g_running   { true };
static std::atomic<bool> g_enabled   { true };
static std::atomic<bool> g_detected  { false }; // pixel match in last frame
static std::atomic<int>  g_trigCount { 0 };     // total triggers fired

static thread_local std::mt19937 tl_rng{ std::random_device{}() };

static int randRange(int lo, int hi)
{
    if (lo >= hi) return lo;
    std::uniform_int_distribution<int> d(lo, hi);
    return d(tl_rng);
}

static void sendKey(WORD vk, bool down)
{
    INPUT inp{};
    inp.type       = INPUT_KEYBOARD;
    inp.ki.wVk     = vk;
    inp.ki.wScan   = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

static void humanTap(WORD vk)
{
    Sleep((DWORD)randRange(CFG_DELAY_MIN_MS, CFG_DELAY_MAX_MS));
    sendKey(vk, true);
    Sleep((DWORD)randRange(CFG_HOLD_MIN_MS, CFG_HOLD_MAX_MS));
    sendKey(vk, false);
}

static inline bool colourMatch(BYTE r, BYTE g, BYTE b)
{
    return (abs((int)r - CFG_R) <= CFG_TOLERANCE) &&
           (abs((int)g - CFG_G) <= CFG_TOLERANCE) &&
           (abs((int)b - CFG_B) <= CFG_TOLERANCE);
}

struct CaptureCtx
{
    HDC        screenDC = nullptr;
    HDC        memDC    = nullptr;
    HBITMAP    hBmp     = nullptr;
    HBITMAP    hOld     = nullptr;
    int        bmpW     = 0, bmpH = 0;
    BITMAPINFO bmi{};
    std::vector<DWORD> buf;

    void init(int w, int h)
    {
        release();
        bmpW = w; bmpH = h;
        screenDC = GetDC(nullptr);
        memDC    = CreateCompatibleDC(screenDC);
        hBmp     = CreateCompatibleBitmap(screenDC, w, h);
        hOld     = (HBITMAP)SelectObject(memDC, hBmp);
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        buf.resize((size_t)w * h);
    }

    void release()
    {
        if (hOld && memDC) SelectObject(memDC, hOld);
        if (hBmp)          DeleteObject(hBmp);
        if (memDC)         DeleteDC(memDC);
        if (screenDC)      ReleaseDC(nullptr, screenDC);
        hOld = nullptr; hBmp = nullptr;
        memDC = nullptr; screenDC = nullptr;
        bmpW = bmpH = 0;
    }

    ~CaptureCtx() { release(); }
};

static void triggerThread()
{
    CaptureCtx ctx;
    int lastW = 0, lastH = 0;

    LARGE_INTEGER freq, lastTrig;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTrig);

    while (g_running.load(std::memory_order_relaxed))
    {
        if (!g_enabled.load(std::memory_order_relaxed))
        {
            g_detected.store(false);
            Sleep(10);
            continue;
        }

        int zw = CFG_ZONE_W, zh = CFG_ZONE_H;
        if (zw != lastW || zh != lastH)
        {
            ctx.init(zw, zh);
            lastW = zw; lastH = zh;
        }

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int ox = (sw - zw) / 2;
        int oy = (sh - zh) / 2;

        BitBlt(ctx.memDC, 0, 0, zw, zh, ctx.screenDC, ox, oy, SRCCOPY);
        GetDIBits(ctx.memDC, ctx.hBmp, 0, zh,
                  ctx.buf.data(), &ctx.bmi, DIB_RGB_COLORS);

        bool found = false;
        for (int i = 0, n = zw * zh; i < n && !found; ++i)
        {
            DWORD px = ctx.buf[i];
            BYTE b = (BYTE)( px        & 0xFF);
            BYTE g = (BYTE)((px >>  8) & 0xFF);
            BYTE r = (BYTE)((px >> 16) & 0xFF);
            if (colourMatch(r, g, b)) found = true;
        }

        g_detected.store(found);

        if (found)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double ms = (double)(now.QuadPart - lastTrig.QuadPart)
                        * 1000.0 / (double)freq.QuadPart;

            if (ms >= CFG_COOLDOWN_MS)
            {
                lastTrig = now;
                g_trigCount.fetch_add(1);
                WORD vk = CFG_VK;
                std::thread([vk]{ humanTap(vk); }).detach();
            }
        }

        if (CFG_SCAN_MS > 0)
            Sleep((DWORD)CFG_SCAN_MS);
    }
}

static void exitWatcher()
{
    while (g_running.load(std::memory_order_relaxed))
    {
        if (GetAsyncKeyState(CFG_EXIT_VK) & 0x8000)
        {
            g_running.store(false);
            return;
        }
        Sleep(50);
    }
}

// ================================================================
//  VIRTUAL-KEY NAME HELPER
// ================================================================

static std::string vkName(WORD vk)
{
    // Handle common special cases cleanly
    switch (vk)
    {
        case VK_SPACE:  return "Space";
        case VK_RETURN: return "Enter";
        case VK_SHIFT:  return "Shift";
        case VK_CONTROL:return "Ctrl";
        case VK_MENU:   return "Alt";
        case VK_ESCAPE: return "Escape";
        case VK_TAB:    return "Tab";
        case VK_BACK:   return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "PageUp";
        case VK_NEXT:   return "PageDown";
        case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";
        case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);

    char buf[64]{};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)sc << 16;
    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0)
        return buf;

    snprintf(buf, sizeof(buf), "VK(0x%02X)", vk);
    return buf;
}

// ================================================================
//  COLOUR PREVIEW BLOCK  (swatch in terminal)
// ================================================================

static void drawColourSwatch(int x, int y, BYTE r, BYTE g, BYTE b)
{
    // We approximate RGB using the nearest Win32 console palette entry
    // Dark/bright split on each channel
    int ri = r > 127 ? 1 : 0;
    int gi = g > 127 ? 1 : 0;
    int bi = b > 127 ? 1 : 0;
    bool bright = (r > 200 || g > 200 || b > 200);
    int attr = (bright ? 8 : 0) | (ri << 2) | (gi << 1) | bi;
    // Map to Win32 console colour index
    // R=4, G=2, B=1, bright=8
    int conIdx = (bi) | (gi << 1) | (ri << 2);
    if (bright) conIdx += 8;
    if (r > 220 && g > 220 && b > 220) conIdx = CON_WHITE;
    if (r < 30  && g < 30  && b < 30 ) conIdx = CON_BLACK;

    conGoto(x, y);
    conColorFB(conIdx, conIdx);
    conWrite("        ");
    conGoto(x, y + 1);
    conWrite("        ");
    conGoto(x, y + 2);
    conWrite("        ");
}

// ================================================================
//  WAIT FOR A KEY PRESS (returns VK code)
// ================================================================

static WORD captureVK(int promptX, int promptY)
{
    conPrint(promptX, promptY, CON_YELLOW, CON_BLACK,
             "  Press any key...           ");
    FlushConsoleInputBuffer(g_hIn);
    while (true)
    {
        for (int vk = 1; vk <= 254; ++vk)
        {
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                while (GetAsyncKeyState(vk) & 0x8000) Sleep(5);
                return (WORD)vk;
            }
        }
        Sleep(5);
    }
}

// ================================================================
//  INPUT HELPERS  (line editor in the console)
// ================================================================

static void showCursor(bool on)
{
    CONSOLE_CURSOR_INFO ci{ 1, (BOOL)on };
    SetConsoleCursorInfo(g_hOut, &ci);
}

// Read an integer at a given console position, returns current if cancelled
static int readIntAt(int x, int y, int current, int lo, int hi)
{
    char buf[16]{};
    int pos = 0;

    showCursor(true);
    conColorFB(CON_WHITE, CON_DARKBLUE);
    // Clear the field
    conGoto(x, y);
    conWrite("        ");

    conGoto(x, y);
    while (true)
    {
        INPUT_RECORD ir{};
        DWORD read = 0;
        ReadConsoleInputA(g_hIn, &ir, 1, &read);
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        char ch = ir.Event.KeyEvent.uChar.AsciiChar;

        if (vk == VK_RETURN) break;
        if (vk == VK_ESCAPE) { showCursor(false); return current; }
        if (vk == VK_BACK && pos > 0)
        {
            buf[--pos] = '\0';
            conGoto(x, y);
            conColorFB(CON_WHITE, CON_DARKBLUE);
            conWrite("        ");
            conGoto(x, y);
            conWrite(buf);
        }
        else if ((ch >= '0' && ch <= '9') && pos < 7)
        {
            buf[pos++] = ch;
            buf[pos]   = '\0';
            char tmp[2] = {ch, '\0'};
            conWrite(tmp);
        }
    }
    showCursor(false);

    if (pos == 0) return current;
    int v = atoi(buf);
    return max(lo, min(hi, v));
}

// ================================================================
//  MAIN MENU  (draws full UI, handles navigation)
// ================================================================

// Menu pages
enum class Page { MAIN, COLOUR, ZONE, TIMING, KEYS };
static Page g_page = Page::MAIN;
static int  g_sel  = 0;  // selected item on MAIN page

// ---- MAIN PAGE ----

static const int MAIN_ITEMS = 6;
static const char* MAIN_LABELS[MAIN_ITEMS] = {
    "  Colour Settings",
    "  Capture Zone",
    "  Timing & Humanisation",
    "  Key Bindings",
    "  Toggle Trigger",
    "  Exit",
};

static void drawHeader()
{
    // Top banner
    conFillRect(0, 0, g_conW, 3, CON_BLACK, CON_DARKBLUE);
    conPrint(2, 0, CON_WHITE,  CON_DARKBLUE, " PixelTrigger  v2.0 ");
    conPrint(2, 1, CON_CYAN,   CON_DARKBLUE, " Win32 screen-colour key trigger");
    // Status pill
    bool en = g_enabled.load();
    bool det= g_detected.load();
    int  cnt= g_trigCount.load();
    char stat[80];
    snprintf(stat, sizeof(stat),
        " Status: %-7s | Detected: %-3s | Triggers: %d  ",
        en  ? "RUNNING" : "PAUSED",
        det ? "YES"     : "no",
        cnt);
    conPrint(2, 2, en ? CON_GREEN : CON_DARKGREY, CON_DARKBLUE, stat);
}

static void drawFooter()
{
    int y = g_conH - 1;
    conFillRect(0, y, g_conW, 1, CON_BLACK, CON_DARKBLUE);
    conPrint(2, y, CON_CYAN, CON_DARKBLUE,
        " \x18\x19 Navigate   Enter Select   F12 Quit ");
}

static void drawMainPage()
{
    // Background
    conFillRect(0, 3, g_conW, g_conH - 4, CON_GREY, CON_BLACK);

    // Left panel: menu
    int mx = 2, my = 5, mw = 30, mh = MAIN_ITEMS + 4;
    conBox(mx, my, mw, mh, CON_CYAN, CON_BLACK, "MENU");

    for (int i = 0; i < MAIN_ITEMS; ++i)
    {
        bool sel = (i == g_sel);
        int  row = my + 2 + i;
        int  fg  = sel ? CON_BLACK : CON_GREY;
        int  bg  = sel ? CON_CYAN  : CON_BLACK;
        char line[64];
        snprintf(line, sizeof(line), "%-28s", MAIN_LABELS[i]);
        conPrint(mx + 1, row, fg, bg, line);
    }

    // Right panel: live settings summary
    int rx = 36, ry = 5, rw = g_conW - 38;
    conBox(rx, ry, rw, 18, CON_DARKGREY, CON_BLACK, "CURRENT CONFIG");

    char buf[128];
    int row = ry + 2;
    auto row_label = [&](const char* lbl, const char* val) {
        conPrint(rx + 2, row, CON_DARKGREY, CON_BLACK, lbl);
        conPrint(rx + 18, row, CON_WHITE, CON_BLACK, val);
        ++row;
    };

    snprintf(buf, sizeof(buf), "%dx%d px", CFG_ZONE_W, CFG_ZONE_H);
    row_label("Capture zone :", buf);

    snprintf(buf, sizeof(buf), "RGB(%d, %d, %d)", CFG_R, CFG_G, CFG_B);
    row_label("Target colour:", buf);

    // mini swatch
    drawColourSwatch(rx + 36, ry + 2, CFG_R, CFG_G, CFG_B);

    snprintf(buf, sizeof(buf), "+/- %d", CFG_TOLERANCE);
    row_label("Tolerance    :", buf);

    ++row; // spacer
    snprintf(buf, sizeof(buf), "%d ms", CFG_SCAN_MS);
    row_label("Scan interval:", buf);

    snprintf(buf, sizeof(buf), "%d ms", CFG_COOLDOWN_MS);
    row_label("Cooldown     :", buf);

    snprintf(buf, sizeof(buf), "%d – %d ms", CFG_DELAY_MIN_MS, CFG_DELAY_MAX_MS);
    row_label("Pre-delay    :", buf);

    snprintf(buf, sizeof(buf), "%d – %d ms", CFG_HOLD_MIN_MS, CFG_HOLD_MAX_MS);
    row_label("Hold time    :", buf);

    ++row; // spacer
    snprintf(buf, sizeof(buf), "%-12s", vkName(CFG_VK).c_str());
    row_label("Trigger key  :", buf);

    snprintf(buf, sizeof(buf), "%-12s", vkName(CFG_EXIT_VK).c_str());
    row_label("Exit key     :", buf);

    bool en = g_enabled.load();
    conPrint(rx + 2, row + 1, en ? CON_GREEN : CON_RED, CON_BLACK,
             en ? " Trigger: ACTIVE  " : " Trigger: PAUSED  ");
}

// ---- COLOUR PAGE ----

static int  col_sel    = 0;  // 0=R 1=G 2=B 3=Tolerance 4=Back
static const int COL_ITEMS = 5;

static void drawColourPage()
{
    conFillRect(0, 3, g_conW, g_conH - 4, CON_GREY, CON_BLACK);

    int bx = (g_conW - 50) / 2, by = 5;
    conBox(bx, by, 50, 20, CON_CYAN, CON_BLACK, "COLOUR SETTINGS");

    struct Row { const char* lbl; int* val; int lo; int hi; };
    int r = CFG_R, g2 = CFG_G, b = CFG_B, t = CFG_TOLERANCE;
    Row rows[4] = {
        { "  Red   (0-255) : ", &CFG_R, 0, 255 },
        { "  Green (0-255) : ", &CFG_G, 0, 255 },
        { "  Blue  (0-255) : ", &CFG_B, 0, 255 },
        { "  Tolerance ±   : ", &CFG_TOLERANCE, 0, 255 },
    };

    for (int i = 0; i < 4; ++i)
    {
        bool sel = (col_sel == i);
        int  row = by + 3 + i * 2;
        conPrint(bx + 1, row,
                 sel ? CON_YELLOW : CON_GREY, CON_BLACK, rows[i].lbl);
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "%3d  ", *rows[i].val);
        conPrint(bx + 20, row,
                 sel ? CON_WHITE : CON_CYAN, sel ? CON_DARKBLUE : CON_BLACK, vbuf);
    }

    // Live swatch
    conPrint(bx + 1, by + 12, CON_DARKGREY, CON_BLACK, "  Preview swatch:");
    drawColourSwatch(bx + 20, by + 12, (BYTE)CFG_R, (BYTE)CFG_G, (BYTE)CFG_B);

    // Back button
    bool bkSel = (col_sel == 4);
    conPrint(bx + 16, by + 17,
             bkSel ? CON_BLACK : CON_GREY,
             bkSel ? CON_CYAN  : CON_BLACK,
             "  < Back  ");

    conPrint(bx + 1, by + 18, CON_DARKGREY, CON_BLACK,
             " Enter=edit  Esc=cancel  \x18\x19=navigate ");
}

// ---- ZONE PAGE ----

static int zone_sel = 0;  // 0=W 1=H 2=Back

static void drawZonePage()
{
    conFillRect(0, 3, g_conW, g_conH - 4, CON_GREY, CON_BLACK);

    int bx = (g_conW - 50) / 2, by = 5;
    conBox(bx, by, 50, 16, CON_CYAN, CON_BLACK, "CAPTURE ZONE");

    conPrint(bx + 2, by + 2, CON_DARKGREY, CON_BLACK,
        " The zone is centred on the primary monitor.");

    struct Row { const char* lbl; int* val; int lo; int hi; };
    Row rows[2] = {
        { "  Width  (px)  : ", &CFG_ZONE_W, 1, 500 },
        { "  Height (px)  : ", &CFG_ZONE_H, 1, 500 },
    };

    for (int i = 0; i < 2; ++i)
    {
        bool sel = (zone_sel == i);
        int  row = by + 5 + i * 2;
        conPrint(bx + 1, row, sel ? CON_YELLOW : CON_GREY, CON_BLACK, rows[i].lbl);
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "%4d  ", *rows[i].val);
        conPrint(bx + 20, row,
                 sel ? CON_WHITE : CON_CYAN, sel ? CON_DARKBLUE : CON_BLACK, vbuf);
    }

    // Visual representation of zone
    int zw = min(CFG_ZONE_W  / 4, 20);
    int zh = min(CFG_ZONE_H  / 4, 6);
    zw = max(1, zw); zh = max(1, zh);
    conPrint(bx + 2, by + 10, CON_DARKGREY, CON_BLACK, " Zone preview (1:4 scale):");
    conBox(bx + 4, by + 11, zw + 2, zh + 2, CON_DARKGREY, CON_BLACK);

    bool bkSel = (zone_sel == 2);
    conPrint(bx + 16, by + 13,
             bkSel ? CON_BLACK : CON_GREY,
             bkSel ? CON_CYAN  : CON_BLACK,
             "  < Back  ");
}

// ---- TIMING PAGE ----

static int timing_sel = 0;  // 0-5 = fields, 6=Back

static void drawTimingPage()
{
    conFillRect(0, 3, g_conW, g_conH - 4, CON_GREY, CON_BLACK);

    int bx = (g_conW - 54) / 2, by = 4;
    conBox(bx, by, 54, 22, CON_CYAN, CON_BLACK, "TIMING & HUMANISATION");

    struct Row { const char* lbl; const char* hint; int* val; int lo; int hi; };
    Row rows[6] = {
        { "  Scan interval (ms)  :", "0=busy poll", &CFG_SCAN_MS,      0,   100 },
        { "  Cooldown (ms)       :", "between triggers",  &CFG_COOLDOWN_MS,  0, 60000 },
        { "  Pre-delay min (ms)  :", "humanise press",    &CFG_DELAY_MIN_MS, 0,  2000 },
        { "  Pre-delay max (ms)  :", "humanise press",    &CFG_DELAY_MAX_MS, 0,  2000 },
        { "  Hold min (ms)       :", "key hold time",     &CFG_HOLD_MIN_MS,  1,  2000 },
        { "  Hold max (ms)       :", "key hold time",     &CFG_HOLD_MAX_MS,  1,  2000 },
    };

    for (int i = 0; i < 6; ++i)
    {
        bool sel = (timing_sel == i);
        int  row = by + 3 + i * 2;
        conPrint(bx + 1, row, sel ? CON_YELLOW : CON_GREY, CON_BLACK, rows[i].lbl);
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "%5d  ", *rows[i].val);
        conPrint(bx + 24, row,
                 sel ? CON_WHITE : CON_CYAN, sel ? CON_DARKBLUE : CON_BLACK, vbuf);
        conPrint(bx + 32, row, CON_DARKGREY, CON_BLACK, rows[i].hint);
    }

    conHLine(bx + 1, by + 16, 52, CON_DARKGREY, CON_BLACK);

    bool bkSel = (timing_sel == 6);
    conPrint(bx + 18, by + 18,
             bkSel ? CON_BLACK : CON_GREY,
             bkSel ? CON_CYAN  : CON_BLACK,
             "  < Back  ");
}

// ---- KEYS PAGE ----

static int keys_sel = 0;  // 0=trigger 1=exit 2=back

static void drawKeysPage()
{
    conFillRect(0, 3, g_conW, g_conH - 4, CON_GREY, CON_BLACK);

    int bx = (g_conW - 50) / 2, by = 5;
    conBox(bx, by, 50, 16, CON_CYAN, CON_BLACK, "KEY BINDINGS");

    struct Row { const char* lbl; WORD* vk; };
    Row rows[2] = {
        { "  Trigger key  : ", &CFG_VK     },
        { "  Exit key     : ", &CFG_EXIT_VK },
    };

    for (int i = 0; i < 2; ++i)
    {
        bool sel = (keys_sel == i);
        int  row = by + 3 + i * 3;
        conPrint(bx + 1, row, sel ? CON_YELLOW : CON_GREY, CON_BLACK, rows[i].lbl);
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "[ %-14s ]  ",
                 vkName(*rows[i].vk).c_str());
        conPrint(bx + 20, row,
                 sel ? CON_WHITE : CON_CYAN, sel ? CON_DARKBLUE : CON_BLACK, vbuf);
        if (sel)
            conPrint(bx + 2, row + 1, CON_DARKGREY, CON_BLACK,
                     " Press Enter to remap this key            ");
    }

    bool bkSel = (keys_sel == 2);
    conPrint(bx + 16, by + 12,
             bkSel ? CON_BLACK : CON_GREY,
             bkSel ? CON_CYAN  : CON_BLACK,
             "  < Back  ");

    conPrint(bx + 1, by + 14, CON_DARKGREY, CON_BLACK,
             " \x18\x19=navigate  Enter=remap  Esc/Back=done ");
}

// ================================================================
//  DRAW DISPATCH
// ================================================================

static void drawPage()
{
    drawHeader();
    drawFooter();
    switch (g_page)
    {
        case Page::MAIN:   drawMainPage();   break;
        case Page::COLOUR: drawColourPage(); break;
        case Page::ZONE:   drawZonePage();   break;
        case Page::TIMING: drawTimingPage(); break;
        case Page::KEYS:   drawKeysPage();   break;
    }
}

// ================================================================
//  INPUT DISPATCH
// ================================================================

static WORD readVK()
{
    INPUT_RECORD ir{};
    DWORD read = 0;
    while (true)
    {
        ReadConsoleInputA(g_hIn, &ir, 1, &read);
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
            return ir.Event.KeyEvent.wVirtualKeyCode;
    }
}

static void handleMain(WORD vk)
{
    switch (vk)
    {
    case VK_UP:
        g_sel = (g_sel + MAIN_ITEMS - 1) % MAIN_ITEMS;
        break;
    case VK_DOWN:
        g_sel = (g_sel + 1) % MAIN_ITEMS;
        break;
    case VK_RETURN:
        switch (g_sel)
        {
        case 0: g_page = Page::COLOUR; col_sel    = 0; break;
        case 1: g_page = Page::ZONE;   zone_sel   = 0; break;
        case 2: g_page = Page::TIMING; timing_sel = 0; break;
        case 3: g_page = Page::KEYS;   keys_sel   = 0; break;
        case 4:
            g_enabled.store(!g_enabled.load());
            break;
        case 5:
            g_running.store(false);
            break;
        }
        break;
    }
}

static void handleColour(WORD vk)
{
    int* vals[4] = { (int*)&CFG_R, (int*)&CFG_G, (int*)&CFG_B, &CFG_TOLERANCE };
    switch (vk)
    {
    case VK_UP:
        col_sel = (col_sel + COL_ITEMS - 1) % COL_ITEMS;
        break;
    case VK_DOWN:
        col_sel = (col_sel + 1) % COL_ITEMS;
        break;
    case VK_RETURN:
        if (col_sel == 4) { g_page = Page::MAIN; break; }
        {
            // Inline edit
            int bx = (g_conW - 50) / 2;
            int row = 5 + 3 + col_sel * 2;
            int newv = readIntAt(bx + 20, row, *vals[col_sel], 0, 255);
            *vals[col_sel] = newv;
        }
        break;
    case VK_ESCAPE:
        g_page = Page::MAIN;
        break;
    }
}

static void handleZone(WORD vk)
{
    switch (vk)
    {
    case VK_UP:
        zone_sel = (zone_sel + 3 - 1) % 3;
        break;
    case VK_DOWN:
        zone_sel = (zone_sel + 1) % 3;
        break;
    case VK_RETURN:
        if (zone_sel == 2) { g_page = Page::MAIN; break; }
        {
            int bx = (g_conW - 50) / 2;
            int row = 5 + 5 + zone_sel * 2;
            int* p = (zone_sel == 0) ? &CFG_ZONE_W : &CFG_ZONE_H;
            int newv = readIntAt(bx + 20, row, *p, 1, 500);
            *p = newv;
        }
        break;
    case VK_ESCAPE:
        g_page = Page::MAIN;
        break;
    }
}

static void handleTiming(WORD vk)
{
    int* vals[6] = {
        &CFG_SCAN_MS, &CFG_COOLDOWN_MS,
        &CFG_DELAY_MIN_MS, &CFG_DELAY_MAX_MS,
        &CFG_HOLD_MIN_MS,  &CFG_HOLD_MAX_MS
    };
    int los[6] = { 0,0,0,0,1,1 };
    int his[6] = { 100,60000,2000,2000,2000,2000 };

    switch (vk)
    {
    case VK_UP:
        timing_sel = (timing_sel + 7 - 1) % 7;
        break;
    case VK_DOWN:
        timing_sel = (timing_sel + 1) % 7;
        break;
    case VK_RETURN:
        if (timing_sel == 6) { g_page = Page::MAIN; break; }
        {
            int bx = (g_conW - 54) / 2;
            int row = 4 + 3 + timing_sel * 2;
            int newv = readIntAt(bx + 24, row,
                                 *vals[timing_sel],
                                 los[timing_sel],
                                 his[timing_sel]);
            *vals[timing_sel] = newv;
            // Clamp min <= max pairs
            if (CFG_DELAY_MAX_MS < CFG_DELAY_MIN_MS)
                CFG_DELAY_MAX_MS = CFG_DELAY_MIN_MS;
            if (CFG_HOLD_MAX_MS < CFG_HOLD_MIN_MS)
                CFG_HOLD_MAX_MS = CFG_HOLD_MIN_MS;
        }
        break;
    case VK_ESCAPE:
        g_page = Page::MAIN;
        break;
    }
}

static void handleKeys(WORD vk)
{
    switch (vk)
    {
    case VK_UP:
        keys_sel = (keys_sel + 3 - 1) % 3;
        break;
    case VK_DOWN:
        keys_sel = (keys_sel + 1) % 3;
        break;
    case VK_RETURN:
        if (keys_sel == 2) { g_page = Page::MAIN; break; }
        {
            // Remap
            int bx = (g_conW - 50) / 2;
            int row = 5 + 3 + keys_sel * 3;
            WORD newvk = captureVK(bx + 2, row + 1);
            if (keys_sel == 0) CFG_VK     = newvk;
            else               CFG_EXIT_VK = newvk;
        }
        break;
    case VK_ESCAPE:
        g_page = Page::MAIN;
        break;
    }
}

static void handleInput(WORD vk)
{
    switch (g_page)
    {
    case Page::MAIN:   handleMain(vk);   break;
    case Page::COLOUR: handleColour(vk); break;
    case Page::ZONE:   handleZone(vk);   break;
    case Page::TIMING: handleTiming(vk); break;
    case Page::KEYS:   handleKeys(vk);   break;
    }
}

// ================================================================
//  STATUS REFRESH THREAD  (redraws header every 500ms)
// ================================================================

static void statusThread()
{
    while (g_running.load(std::memory_order_relaxed))
    {
        drawHeader();
        Sleep(500);
    }
}

// ================================================================
//  ENTRY POINT
// ================================================================

int main()
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    conInit();

    // Size the console window nicely
    COORD sz{ 120, 35 };
    SetConsoleScreenBufferSize(g_hOut, sz);
    SMALL_RECT wr{ 0, 0, 119, 34 };
    SetConsoleWindowInfo(g_hOut, TRUE, &wr);
    // Refresh dims
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(g_hOut, &csbi))
        {
            g_conW = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
            g_conH = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
        }
    }

    SetConsoleTitleA("PixelTrigger v2.0");
    conClear();

    // Start background threads
    std::thread tTrigger(triggerThread);
    std::thread tExit(exitWatcher);
    std::thread tStatus(statusThread);

    // Initial full draw
    drawPage();

    // Event loop
    while (g_running.load())
    {
        WORD vk = readVK();
        if (!g_running.load()) break;
        handleInput(vk);
        conClear();
        drawPage();
    }

    g_running.store(false);
    conClear();
    conPrint(2, 2, CON_CYAN, CON_BLACK,
        " PixelTrigger stopped. Goodbye.");
    conGoto(0, 4);
    showCursor(true);

    tTrigger.join();
    tStatus.detach();
    tExit.detach();
    return 0;
}
