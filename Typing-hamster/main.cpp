#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdint>
#include <string>

// BEGIN PORTABLE POLICY
namespace policy
{
constexpr int WIDTHS[] = {160, 240, 320, 400};
constexpr unsigned FRAME_MS = 34;
constexpr unsigned IDLE_MS = 120;

int SizeIndex(int width)
{
    for (int i = 0; i < 4; ++i)
        if (WIDTHS[i] == width) return i;
    return 2;
}
int ClampAxis(int position, int start, int end, int extent)
{
    return std::clamp(position, start, std::max(start, end - extent));
}
bool ShouldListen(bool visible, bool paused, bool menuOpen)
{
    return visible && !paused && !menuOpen;
}
struct Animation
{
    int shown = -1;
    int nextHand = 1;
    bool pending = false;
    std::uint64_t inputAt = 0;
    std::uint64_t renderAt = 0;

    void Key(std::uint64_t now) { inputAt = now; pending = true; }
    bool Idle(std::uint64_t now) const { return now - inputAt >= IDLE_MS; }
    unsigned TapDelay(std::uint64_t now) const
    {
        auto elapsed = now - renderAt;
        return elapsed >= FRAME_MS ? 0 : static_cast<unsigned>(FRAME_MS - elapsed);
    }
    bool TapReady(std::uint64_t now) const
    {
        return pending && !Idle(now) && TapDelay(now) == 0;
    }
    void Presented(int frame, std::uint64_t now, bool tap)
    {
        shown = frame;
        renderAt = now;
        if (tap)
        {
            nextHand = (frame == 1) ? 2 : 1;
            pending = false;
        }
    }
};
}
// END PORTABLE POLICY

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

constexpr wchar_t WINDOW_CLASS[] = L"TypingPetDesktopV1";
constexpr wchar_t MUTEX_NAME[] = L"Local\\TypingPetDesktopV1.SingleInstance";
constexpr UINT TRAY_MESSAGE = WM_APP + 1;
constexpr UINT REVEAL_MESSAGE = WM_APP + 2;
constexpr UINT_PTR RESET_TIMER = 1;
constexpr UINT_PTR RENDER_TIMER = 2;
constexpr UINT TRAY_ID = 1;
enum Command : UINT
{
    CMD_PAUSE = 100, CMD_VISIBLE, CMD_LOCK, CMD_PASS,
    CMD_RESET_POSITION, CMD_EXIT, CMD_SIZE_FIRST = 200
};

struct Settings
{
    int width = 320;
    int x = 100, y = 100;
    bool paused = false;
    bool visible = true;
    bool locked = false;
    bool clickThrough = false;
};
struct Frame
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
};

Settings settings;
Frame frames[4][3]; // All four sizes are cached once at startup.
int heights[4] = {};
int sizeIndex = 2;
policy::Animation animation;
HINSTANCE appInstance = nullptr;
HICON trayIcon = nullptr;
bool ownTrayIcon = false;
bool trayAdded = false;
bool trayVersion4 = false;
bool keyboardRegistered = false;
bool menuOpen = false;
bool ready = false;
bool renderTimerActive = false;
bool idleTimerActive = false;
bool saveErrorShown = false;
UINT taskbarCreated = 0;
std::wstring configPath;

void Error(HWND hwnd, const std::wstring& text)
{
    MessageBoxW(hwnd, text.c_str(), L"TypingPet", MB_OK | MB_ICONERROR);
}

bool InitConfigPath()
{
    wchar_t folder[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(
            nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
            nullptr, SHGFP_TYPE_CURRENT, folder))) return false;
    std::wstring directory = std::wstring(folder) + L"\\TypingPet";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return false;
    configPath = directory + L"\\settings.ini";
    return true;
}

int ReadInt(const wchar_t* key, int fallback, int low, int high)
{
    wchar_t value[64] = {};
    std::wstring defaultText = std::to_wstring(fallback);
    GetPrivateProfileStringW(
        L"Pet", key, defaultText.c_str(), value, 64, configPath.c_str());
    try
    {
        std::size_t consumed = 0;
        int result = std::stoi(value, &consumed);
        if (consumed != std::wstring(value).size() || result < low || result > high)
            return fallback;
        return result;
    }
    catch (...) { return fallback; }
}

void LoadSettings()
{
    int width = ReadInt(L"Width", 320, 160, 400);
    sizeIndex = policy::SizeIndex(width);
    settings.width = policy::WIDTHS[sizeIndex];
    settings.x = ReadInt(L"X", 100, -1000000, 1000000);
    settings.y = ReadInt(L"Y", 100, -1000000, 1000000);
    settings.paused = ReadInt(L"Paused", 0, 0, 1) != 0;
    settings.visible = ReadInt(L"Visible", 1, 0, 1) != 0;
    settings.locked = ReadInt(L"Locked", 0, 0, 1) != 0;
    settings.clickThrough = ReadInt(L"ClickThrough", 0, 0, 1) != 0;
}

void CapturePosition(HWND hwnd)
{
    RECT rect = {};
    if (GetWindowRect(hwnd, &rect))
    {
        settings.x = rect.left;
        settings.y = rect.top;
    }
}

void SaveSettings(HWND hwnd)
{
    if (!ready) return;
    CapturePosition(hwnd);
    std::wstring section;
    auto add = [&](const wchar_t* key, int value)
    {
        section += key;
        section += L"=";
        section += std::to_wstring(value);
        section.push_back(L'\0');
    };
    add(L"Width", settings.width);
    add(L"X", settings.x);
    add(L"Y", settings.y);
    add(L"Paused", settings.paused);
    add(L"Visible", settings.visible);
    add(L"Locked", settings.locked);
    add(L"ClickThrough", settings.clickThrough);
    section.push_back(L'\0');

    std::wstring temporary = configPath + L".tmp";
    bool written = WritePrivateProfileSectionW(
        L"Pet", section.c_str(), temporary.c_str()) != FALSE;
    if (written)
        written = WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, temporary.c_str()) != FALSE;
    if (written)
        written = MoveFileExW(
            temporary.c_str(), configPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!written)
    {
        DeleteFileW(temporary.c_str());
        if (!saveErrorShown)
        {
            saveErrorShown = true;
            Error(hwnd, L"설정을 저장하지 못했습니다.\n" + configPath);
        }
    }
    else saveErrorShown = false;
}

void FreeFrames()
{
    for (auto& size : frames)
        for (Frame& frame : size)
        {
            if (frame.dc && frame.oldBitmap)
                SelectObject(frame.dc, frame.oldBitmap);
            if (frame.bitmap) DeleteObject(frame.bitmap);
            if (frame.dc) DeleteDC(frame.dc);
            frame = Frame{};
        }
    if (ownTrayIcon && trayIcon) DestroyIcon(trayIcon);
    trayIcon = nullptr;
    ownTrayIcon = false;
}

bool BuildFrame(Frame& frame, Gdiplus::Bitmap& image, int width, int height)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    frame.dc = CreateCompatibleDC(nullptr);
    frame.bitmap = CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!frame.dc || !frame.bitmap || !pixels) return false;
    frame.oldBitmap = SelectObject(frame.dc, frame.bitmap);
    if (!frame.oldBitmap || frame.oldBitmap == HGDI_ERROR)
    {
        frame.oldBitmap = nullptr;
        return false;
    }

    // A top-down DIB with premultiplied alpha can be reused directly by Windows.
    Gdiplus::Bitmap surface(
        width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
    Gdiplus::Graphics graphics(&surface);
    if (surface.GetLastStatus() != Gdiplus::Ok ||
        graphics.GetLastStatus() != Gdiplus::Ok) return false;
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    auto status = graphics.DrawImage(
        &image, Gdiplus::Rect(0, 0, width, height),
        0, 0, static_cast<INT>(image.GetWidth()),
        static_cast<INT>(image.GetHeight()), Gdiplus::UnitPixel, &attributes);
    graphics.Flush(Gdiplus::FlushIntentionSync);
    return status == Gdiplus::Ok;
}

bool LoadFrames()
{
    wchar_t exePath[32768] = {};
    DWORD length = GetModuleFileNameW(nullptr, exePath, 32768);
    if (length == 0 || length >= 32768) return false;
    std::wstring directory(exePath, length);
    directory = directory.substr(0, directory.find_last_of(L"\\/") + 1);
    const wchar_t* names[] = {L"idle.png", L"left.png", L"right.png"};
    UINT sourceWidth = 0, sourceHeight = 0;

    for (int pose = 0; pose < 3; ++pose)
    {
        std::wstring path = directory + L"images\\" + names[pose];
        Gdiplus::Bitmap image(path.c_str());
        if (image.GetLastStatus() != Gdiplus::Ok ||
            image.GetWidth() == 0 || image.GetHeight() == 0)
        {
            Error(nullptr, L"PNG 이미지를 불러오지 못했습니다.\n" + path);
            return false;
        }
        if (pose == 0)
        {
            sourceWidth = image.GetWidth();
            sourceHeight = image.GetHeight();
            if (sourceWidth > 8192 || sourceHeight > 8192) return false;
            for (int slot = 0; slot < 4; ++slot)
            {
                heights[slot] = MulDiv(policy::WIDTHS[slot], sourceHeight, sourceWidth);
                if (heights[slot] < 1 || heights[slot] > 2048) return false;
            }
            Gdiplus::Bitmap iconBitmap(32, 32, PixelFormat32bppPARGB);
            {
                Gdiplus::Graphics iconGraphics(&iconBitmap);
                iconGraphics.Clear(Gdiplus::Color(0, 0, 0, 0));
                iconGraphics.SetInterpolationMode(
                    Gdiplus::InterpolationModeHighQualityBicubic);
                iconGraphics.DrawImage(&image, 0, 0, 32, 32);
            }
            ownTrayIcon = iconBitmap.GetHICON(&trayIcon) == Gdiplus::Ok && trayIcon;
        }
        else if (image.GetWidth() != sourceWidth || image.GetHeight() != sourceHeight)
        {
            Error(nullptr, L"이미지 3장의 캔버스 크기가 모두 같아야 합니다.");
            return false;
        }
        for (int slot = 0; slot < 4; ++slot)
            if (!BuildFrame(frames[slot][pose], image,
                            policy::WIDTHS[slot], heights[slot])) return false;
        // The original full-resolution image is released after this iteration.
    }
    if (!trayIcon) trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    return trayIcon != nullptr;
}

bool ShowFrame(HWND hwnd, int index, bool force = false, bool tap = false)
{
    if (!force && animation.shown == index) return true;
    POINT source = {0, 0};
    SIZE size = {settings.width, heights[sizeIndex]};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL success = UpdateLayeredWindow(
        hwnd, nullptr, nullptr, &size, frames[sizeIndex][index].dc,
        &source, 0, &blend, ULW_ALPHA);
    if (success) animation.Presented(index, GetTickCount64(), tap);
    return success != FALSE;
}

bool Active()
{
    return policy::ShouldListen(settings.visible, settings.paused, menuOpen);
}

bool SetKeyboardEnabled(HWND hwnd, bool enabled)
{
    if (keyboardRegistered == enabled) return true;
    RAWINPUTDEVICE keyboard = {};
    keyboard.usUsagePage = 0x01;
    keyboard.usUsage = 0x06;
    keyboard.dwFlags = enabled ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    keyboard.hwndTarget = enabled ? hwnd : nullptr;
    if (!RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard))) return false;
    keyboardRegistered = enabled;
    return true;
}

void StopTimers(HWND hwnd)
{
    KillTimer(hwnd, RESET_TIMER);
    KillTimer(hwnd, RENDER_TIMER);
    renderTimerActive = false;
    idleTimerActive = false;
    animation.pending = false;
}

void ScheduleTap(HWND hwnd)
{
    auto now = GetTickCount64();
    if (!animation.pending || animation.Idle(now)) return;
    if (animation.TapReady(now))
    {
        if (renderTimerActive) KillTimer(hwnd, RENDER_TIMER);
        renderTimerActive = false;
        ShowFrame(hwnd, animation.nextHand, false, true);
    }
    else if (!renderTimerActive)
    {
        renderTimerActive = SetTimer(
            hwnd, RENDER_TIMER, animation.TapDelay(now), nullptr) != 0;
    }
}

void OnKeyPress(HWND hwnd)
{
    animation.Key(GetTickCount64());
    ScheduleTap(hwnd);
    if (!idleTimerActive)
        idleTimerActive = SetTimer(hwnd, RESET_TIMER, policy::IDLE_MS, nullptr) != 0;
}

void SyncActivity(HWND hwnd)
{
    if (!Active())
    {
        StopTimers(hwnd);
        ShowFrame(hwnd, 0);
    }
    if (!SetKeyboardEnabled(hwnd, Active()) && Active())
    {
        settings.paused = true;
        StopTimers(hwnd);
        ShowFrame(hwnd, 0);
        Error(hwnd, L"키보드 입력을 등록하지 못해 일시정지했습니다.");
    }
}

void UpdateTrayTip(HWND hwnd)
{
    if (!trayAdded) return;
    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = TRAY_ID;
    icon.uFlags = NIF_TIP;
    const wchar_t* text = !settings.visible ? L"TypingPet - 숨김" :
        settings.paused ? L"TypingPet - 일시정지" : L"TypingPet - 실행 중";
    lstrcpynW(icon.szTip, text, static_cast<int>(sizeof(icon.szTip) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

bool AddTray(HWND hwnd)
{
    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = TRAY_ID;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = TRAY_MESSAGE;
    icon.hIcon = trayIcon;
    lstrcpynW(icon.szTip, L"TypingPet", 128);
    trayAdded = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if (trayAdded)
    {
        icon.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
        UpdateTrayTip(hwnd);
    }
    return trayAdded;
}

void RemoveTray(HWND hwnd)
{
    if (!trayAdded) return;
    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    trayAdded = false;
}

void ApplyMouseMode(HWND hwnd)
{
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (settings.clickThrough) style |= WS_EX_TRANSPARENT;
    else style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void ClampPosition(HWND hwnd, bool reset = false)
{
    RECT candidate = {settings.x, settings.y,
        settings.x + settings.width, settings.y + heights[sizeIndex]};
    HMONITOR monitor = MonitorFromRect(&candidate, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;
    if (reset)
    {
        settings.x = info.rcWork.right - settings.width - 20;
        settings.y = info.rcWork.bottom - heights[sizeIndex] - 20;
    }
    settings.x = policy::ClampAxis(
        settings.x, info.rcWork.left, info.rcWork.right, settings.width);
    settings.y = policy::ClampAxis(
        settings.y, info.rcWork.top, info.rcWork.bottom, heights[sizeIndex]);
    SetWindowPos(hwnd, HWND_TOPMOST, settings.x, settings.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

void Reveal(HWND hwnd)
{
    settings.visible = true;
    CapturePosition(hwnd);
    ClampPosition(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SyncActivity(hwnd);
    UpdateTrayTip(hwnd);
    SaveSettings(hwnd);
}

void HandleCommand(HWND hwnd, UINT command)
{
    CapturePosition(hwnd);
    switch (command)
    {
    case CMD_PAUSE:
        settings.paused = !settings.paused;
        break;
    case CMD_VISIBLE:
        settings.visible = !settings.visible;
        ShowWindow(hwnd, settings.visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        break;
    case CMD_LOCK:
        settings.locked = !settings.locked;
        break;
    case CMD_PASS:
        settings.clickThrough = !settings.clickThrough;
        ApplyMouseMode(hwnd);
        break;
    case CMD_RESET_POSITION:
        settings.visible = true;
        ClampPosition(hwnd, true);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        break;
    case CMD_EXIT:
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    default:
        if (command >= CMD_SIZE_FIRST && command < CMD_SIZE_FIRST + 4)
        {
            sizeIndex = static_cast<int>(command - CMD_SIZE_FIRST);
            settings.width = policy::WIDTHS[sizeIndex];
            StopTimers(hwnd);
            ShowFrame(hwnd, 0, true);
            ClampPosition(hwnd);
        }
        break;
    }
    SyncActivity(hwnd);
    UpdateTrayTip(hwnd);
    SaveSettings(hwnd);
}

void ShowMenu(HWND hwnd)
{
    if (menuOpen || !ready) return;
    HMENU menu = CreatePopupMenu();
    HMENU sizes = CreatePopupMenu();
    if (!menu || !sizes)
    {
        if (menu) DestroyMenu(menu);
        if (sizes) DestroyMenu(sizes);
        return;
    }
    AppendMenuW(menu, MF_STRING | (settings.paused ? MF_CHECKED : 0),
                CMD_PAUSE, L"일시정지");
    AppendMenuW(menu, MF_STRING | (trayAdded ? 0 : MF_GRAYED),
                CMD_VISIBLE, settings.visible ? L"펫 숨기기" : L"펫 보이기");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const wchar_t* labels[] = {
        L"작게 (160px)", L"보통 (240px)", L"크게 (320px)", L"아주 크게 (400px)"
    };
    for (int i = 0; i < 4; ++i)
        AppendMenuW(sizes, MF_STRING, CMD_SIZE_FIRST + i, labels[i]);
    CheckMenuRadioItem(sizes, CMD_SIZE_FIRST, CMD_SIZE_FIRST + 3,
                       CMD_SIZE_FIRST + sizeIndex, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"크기");
    AppendMenuW(menu, MF_STRING | (settings.locked ? MF_CHECKED : 0),
                CMD_LOCK, L"위치 잠금");
    AppendMenuW(menu, MF_STRING | (settings.clickThrough ? MF_CHECKED : 0) |
                (trayAdded ? 0 : MF_GRAYED), CMD_PASS, L"클릭 통과");
    AppendMenuW(menu, MF_STRING, CMD_RESET_POSITION, L"위치 초기화");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"종료");

    menuOpen = true;
    SyncActivity(hwnd);
    POINT point = {};
    GetCursorPos(&point);
    HWND previous = GetForegroundWindow();
    // A temporary menu owner keeps the tray menu usable even when the pet is hidden.
    HWND owner = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"STATIC", L"TypingPet Menu", WS_POPUP,
        point.x, point.y, 1, 1, hwnd, nullptr, appInstance, nullptr);
    HWND menuOwner = owner ? owner : hwnd;
    if (owner) ShowWindow(owner, SW_SHOWNOACTIVATE);
    SetForegroundWindow(menuOwner);
    UINT command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, 0, menuOwner, nullptr));
    PostMessageW(menuOwner, WM_NULL, 0, 0);
    DestroyMenu(menu); // Also destroys its size submenu.
    if (owner) DestroyWindow(owner);
    menuOpen = false;

    if (!IsWindow(hwnd)) return; // The session may have ended while the menu was open.

    if (command) HandleCommand(hwnd, command);
    else SyncActivity(hwnd);
    if (trayAdded)
    {
        NOTIFYICONDATAW icon = {};
        icon.cbSize = sizeof(icon);
        icon.hWnd = hwnd;
        icon.uID = TRAY_ID;
        Shell_NotifyIconW(NIM_SETFOCUS, &icon);
    }
    if (IsWindow(hwnd) && IsWindow(previous) && previous != hwnd)
        SetForegroundWindow(previous);
}

LRESULT CALLBACK WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (taskbarCreated && message == taskbarCreated && ready)
    {
        trayAdded = false;
        if (!AddTray(hwnd))
        {
            // Never leave an invisible, click-through app with no way to control it.
            settings.visible = true;
            settings.clickThrough = false;
            ApplyMouseMode(hwnd);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SyncActivity(hwnd);
            SaveSettings(hwnd);
            Error(hwnd, L"트레이 아이콘을 복구하지 못했습니다.\n펫을 우클릭해 조작할 수 있습니다.");
        }
        return 0;
    }
    switch (message)
    {
    case TRAY_MESSAGE:
    {
        UINT event = trayVersion4 ? LOWORD(lParam) : static_cast<UINT>(lParam);
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP ||
            event == NIN_SELECT || event == NIN_KEYSELECT)
            ShowMenu(hwnd);
        return 0;
    }
    case REVEAL_MESSAGE:
        if (ready) Reveal(hwnd);
        return 0;

    case WM_INPUT:
    {
        if (Active())
        {
            RAWINPUT input = {};
            UINT size = sizeof(input);
            UINT result = GetRawInputData(
                reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                &input, &size, sizeof(RAWINPUTHEADER));
            if (result != static_cast<UINT>(-1) &&
                input.header.dwType == RIM_TYPEKEYBOARD)
            {
                const RAWKEYBOARD& key = input.data.keyboard;
                if (!(key.Flags & RI_KEY_BREAK) && key.VKey < 255)
                    OnKeyPress(hwnd);
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_TIMER:
        if (!Active())
        {
            StopTimers(hwnd);
            return 0;
        }
        if (wParam == RENDER_TIMER)
        {
            KillTimer(hwnd, RENDER_TIMER);
            renderTimerActive = false;
            ScheduleTap(hwnd);
        }
        else if (wParam == RESET_TIMER)
        {
            KillTimer(hwnd, RESET_TIMER);
            idleTimerActive = false;
            auto now = GetTickCount64();
            if (!animation.Idle(now))
            {
                idleTimerActive = SetTimer(
                    hwnd, RESET_TIMER,
                    static_cast<UINT>(policy::IDLE_MS - (now - animation.inputAt)),
                    nullptr) != 0;
            }
            else
            {
                StopTimers(hwnd);
                ShowFrame(hwnd, 0);
            }
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps = {};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        if (!settings.locked && !settings.clickThrough)
        {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                         static_cast<LPARAM>(GetMessagePos()));
        }
        return 0;
    case WM_RBUTTONUP:
        ShowMenu(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        CapturePosition(hwnd);
        ClampPosition(hwnd);
        SaveSettings(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:
        if (ready)
        {
            CapturePosition(hwnd);
            ClampPosition(hwnd);
            if (animation.shown >= 0) ShowFrame(hwnd, animation.shown, true);
            SaveSettings(hwnd);
        }
        return 0;
    case WM_CLOSE:
        SaveSettings(hwnd);
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        SaveSettings(hwnd);
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        StopTimers(hwnd);
        SetKeyboardEnabled(hwnd, false);
        RemoveTray(hwnd);
        ready = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int RunApp(HINSTANCE instance)
{
    appInstance = instance;
    if (!InitConfigPath())
    {
        Error(nullptr, L"설정을 저장할 폴더를 만들지 못했습니다.");
        return 1;
    }
    LoadSettings();
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return 1;
    if (!LoadFrames())
    {
        Error(nullptr, L"이미지 초기화에 실패했습니다.\nimages 폴더의 PNG 3장을 확인해 주세요.");
        FreeFrames();
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = trayIcon;
    HWND hwnd = nullptr;
    if (RegisterClassW(&wc))
        hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            WINDOW_CLASS, L"TypingPet", WS_POPUP,
            settings.x, settings.y, settings.width, heights[sizeIndex],
            nullptr, nullptr, instance, nullptr);

    int exitCode = 1;
    if (!hwnd)
    {
        Error(nullptr, L"펫 창을 만들지 못했습니다.");
    }
    else if (!ShowFrame(hwnd, 0) || !AddTray(hwnd))
    {
        Error(hwnd, L"펫 또는 트레이 아이콘을 표시하지 못했습니다.");
        DestroyWindow(hwnd);
    }
    else
    {
        ready = true;
        ClampPosition(hwnd);
        ApplyMouseMode(hwnd);
        if (settings.visible) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SyncActivity(hwnd);
        UpdateTrayTip(hwnd);
        MSG message = {};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        exitCode = result == -1 ? 1 : static_cast<int>(message.wParam);
        if (IsWindow(hwnd))
        {
            SaveSettings(hwnd);
            DestroyWindow(hwnd);
        }
    }
    FreeFrames();
    Gdiplus::GdiplusShutdown(token);
    return exitCode;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    DWORD status = GetLastError();
    if (!mutex)
    {
        Error(nullptr, L"중복 실행 방지를 초기화하지 못했습니다.");
        return 1;
    }
    if (status == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(WINDOW_CLASS, nullptr);
        if (existing) PostMessageW(existing, REVEAL_MESSAGE, 0, 0);
        else MessageBoxW(nullptr, L"TypingPet이 이미 실행 중입니다.\n트레이 아이콘을 확인해 주세요.",
                         L"TypingPet", MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }
    int result = RunApp(instance);
    CloseHandle(mutex);
    return result;
}

