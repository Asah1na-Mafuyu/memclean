#include <windows.h>
#include <shellapi.h>
#include <cstdint>

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args || count != 3) {
        if (args)
            LocalFree(args);
        return 2;
    }
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, args[1]);
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, args[2]);
    LocalFree(args);
    if (!ready || !stop) {
        if (ready)
            CloseHandle(ready);
        if (stop)
            CloseHandle(stop);
        return 3;
    }
    constexpr SIZE_T length = 128ull * 1024 * 1024;
    auto memory = static_cast<volatile unsigned char*>(
        VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory)
        return 4;
    for (SIZE_T i = 0; i < length; i += 4096)
        memory[i] = 0x5A;
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"memclean.IsolatedFixture";
    RegisterClassW(&cls);
    // A visible off-screen tool window qualifies as a desktop app without stealing focus.
    HWND window =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"memclean test fixture",
                        WS_OVERLAPPEDWINDOW, -30000, -30000, 200, 100, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 5;
    SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetEvent(ready);
    ULONGLONG start = GetTickCount64();
    bool stopNow = false;
    while (!stopNow && GetTickCount64() - start < 45000) {
        DWORD wait = MsgWaitForMultipleObjects(1, &stop, FALSE, 1000, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0)
            break;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                stopNow = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // Memory contents must remain valid after a working-set trim.
    bool intact = true;
    for (SIZE_T i = 0; i < length; i += 4096)
        if (memory[i] != 0x5A) {
            intact = false;
            break;
        }
    VirtualFree(const_cast<unsigned char*>(memory), 0, MEM_RELEASE);
    DestroyWindow(window);
    CloseHandle(stop);
    CloseHandle(ready);
    return intact ? 0 : 6;
}
