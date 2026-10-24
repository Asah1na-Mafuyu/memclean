#include "core.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace {
constexpr wchar_t kClass[] = L"memclean.Main.v1";
constexpr ULONGLONG kCooldownMilliseconds = 30'000;
constexpr UINT kLog = WM_APP + 1, kFinished = WM_APP + 2, kInspected = WM_APP + 3;
enum Id {
    Refresh = 100,
    Clean,
    Cancel,
    Advanced,
    List,
    Log,
    Low,
    All,
    Cache,
    None,
    Heading,
    Subtitle,
    Available,
    Total,
    Foreground,
    Tip,
    Status,
    SelectAll,
    SelectNone
};
const COLORREF kBackground = RGB(245, 247, 250), kInk = RGB(28, 43, 57), kMuted = RGB(88, 106, 121);
const COLORREF kAccent = RGB(0, 104, 93);

class Application;
Application* gApplication = nullptr;

class Application {
    HWND window_ = nullptr, list_ = nullptr, log_ = nullptr;
    HINSTANCE instance_;
    HFONT font_ = nullptr, titleFont_ = nullptr, numberFont_ = nullptr;
    HBRUSH background_ = nullptr;
    UINT dpi_ = 96;
    bool advanced_ = false, admin_ = false, busy_ = false, inspecting_ = false;
    bool closePending_ = false, updatingList_ = false;
    std::atomic_bool cancel_{false};
    std::thread worker_;
    HWINEVENTHOOK hook_ = nullptr;
    std::mutex foregroundMutex_;
    mc::Identity foreground_;
    std::wstring foregroundName_ = L"尚未识别";
    std::wstring ownPath_;
    mc::Snapshot snapshot_;
    ULONGLONG completedAt_ = 0;
    bool hasRun_ = false;
    int D(int n) const { return MulDiv(n, static_cast<int>(dpi_), 96); }
    HWND Control(int id) const { return GetDlgItem(window_, id); }
    void Text(int id, const std::wstring& text) { SetWindowTextW(Control(id), text.c_str()); }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return control;
    }
    void Place(int id, int x, int y, int width, int height) {
        MoveWindow(Control(id), D(x), D(y), D(width), D(height), TRUE);
    }
    void Fonts() {
        if (font_)
            DeleteObject(font_);
        if (titleFont_)
            DeleteObject(titleFont_);
        if (numberFont_)
            DeleteObject(numberFont_);
        font_ =
            CreateFontW(-D(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        titleFont_ = CreateFontW(-D(29), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
        numberFont_ = CreateFontW(-D(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  L"Microsoft YaHei UI");
        if (window_) {
            for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(Control(Heading), WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
            SendMessageW(Control(Available), WM_SETFONT, reinterpret_cast<WPARAM>(numberFont_), TRUE);
        }
    }
    void Layout() {
        RECT client{};
        GetClientRect(window_, &client);
        const int w = MulDiv(client.right, 96, dpi_), h = MulDiv(client.bottom, 96, dpi_);
        Place(Heading, 28, 22, w - 56, 42);
        Place(Subtitle, 30, 72, w - 60, 24);
        Place(Available, 30, 117, w - 60, 38);
        Place(Total, 30, 161, w - 60, 25);
        Place(Foreground, 30, 200, w - 60, 26);
        if (!advanced_) {
            Place(Refresh, 30, 239, 112, 34);
            Place(SelectAll, 152, 239, 118, 34);
            Place(SelectNone, 280, 239, 110, 34);
            Place(Tip, 410, 246, w - 440, 25);
            const int tableHeight = std::max(120, h - 517);
            Place(List, 30, 283, w - 60, tableHeight);
            int tableWidth = w - 64;
            ListView_SetColumnWidth(list_, 0, D(240));
            ListView_SetColumnWidth(list_, 1, D(76));
            ListView_SetColumnWidth(list_, 2, D(116));
            ListView_SetColumnWidth(list_, 3, D(std::max(220, tableWidth - 452)));
            Place(Log, 30, h - 222, w - 60, 104);
            Place(Status, 30, h - 107, w - 60, 24);
            Place(Clean, 30, h - 68, 190, 42);
            Place(Cancel, 232, h - 68, 102, 42);
            Place(Advanced, w - 210, h - 68, 180, 42);
        } else {
            Place(Tip, 30, 239, w - 60, 48);
            Place(None, 34, 303, w - 68, 30);
            Place(Low, 34, 343, w - 68, 30);
            Place(All, 34, 383, w - 68, 30);
            Place(Cache, 34, 435, w - 68, 30);
            Place(Log, 30, 489, w - 60, std::max(65, h - 607));
            Place(Status, 30, h - 107, w - 60, 24);
            Place(Clean, 30, h - 68, 230, 42);
            Place(Cancel, 272, h - 68, 102, 42);
        }
        InvalidateRect(window_, nullptr, TRUE);
    }
    void AppendLog(const std::wstring& message) {
        int length = GetWindowTextLengthW(log_);
        if (length > 180000) {
            SetWindowTextW(log_, L"（较早的记录已从显示区移除）\r\n");
            length = GetWindowTextLengthW(log_);
        }
        SendMessageW(log_, EM_SETSEL, length, length);
        std::wstring line = message + L"\r\n";
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(log_, EM_SCROLLCARET, 0, 0);
    }
    void PostLog(const std::wstring& line) {
        auto text = std::make_unique<std::wstring>(line);
        if (PostMessageW(window_, kLog, 0, reinterpret_cast<LPARAM>(text.get())))
            text.release();
    }
    void UpdateNumbers() {
        auto memory = mc::ReadMemory();
        if (memory.valid) {
            Text(Available, L"可用内存  " + mc::Bytes(memory.available));
            Text(Total, L"总内存 " + mc::Bytes(memory.total) + L"     ·     当前使用 " +
                            std::to_wstring(memory.load) + L"%     ·     仅手动清理");
        } else {
            Text(Available, L"无法读取内存状态");
            Text(Total, L"清理结果将不显示内存变化。 ");
        }
        if (!advanced_) {
            std::lock_guard<std::mutex> lock(foregroundMutex_);
            Text(Foreground,
                 L"正在保护：" + foregroundName_ + L"  ｜  Minecraft / Java  ｜  系统与管理员程序");
        }
        if (!busy_ && !inspecting_)
            UpdateButtons();
    }
    unsigned SelectedCount() const {
        unsigned count = 0;
        for (int i = 0; i < static_cast<int>(snapshot_.processes.size()); ++i)
            if (snapshot_.processes[i].eligible && ListView_GetCheckState(list_, i))
                ++count;
        return count;
    }
    void UpdateButtons() {
        const bool ready = !busy_ && !inspecting_;
        const ULONGLONG elapsed = GetTickCount64() - completedAt_;
        bool cooldown = hasRun_ && elapsed < kCooldownMilliseconds;
        EnableWindow(Control(Cancel), busy_ && !cancel_.load());
        EnableWindow(Control(Clean), ready && !cooldown && (advanced_ || SelectedCount() > 0));
        if (!advanced_) {
            for (int id : {Refresh, SelectAll, SelectNone, Advanced, List})
                EnableWindow(Control(id), ready);
            if (ready && !cooldown)
                Text(Clean, L"清理所选后台程序");
        } else {
            for (int id : {None, Low, All, Cache})
                EnableWindow(Control(id), ready);
            if (ready && !cooldown)
                Text(Clean, admin_ ? L"执行所选全局清理" : L"以管理员身份打开");
        }
        if (ready && cooldown) {
            auto remaining = (kCooldownMilliseconds - elapsed + 999) / 1000;
            Text(Clean, L"请稍候 " + std::to_wstring(remaining) + L" 秒");
        }
    }
    void Populate() {
        updatingList_ = true;
        ListView_DeleteAllItems(list_);
        unsigned eligible = 0, protectedCount = 0;
        for (size_t i = 0; i < snapshot_.processes.size(); ++i) {
            const auto& p = snapshot_.processes[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(p.name.c_str());
            ListView_InsertItem(list_, &item);
            auto pid = std::to_wstring(p.id.pid), memory = p.workingSet ? mc::Bytes(p.workingSet) : L"—";
            ListView_SetItemText(list_, item.iItem, 1, const_cast<wchar_t*>(pid.c_str()));
            ListView_SetItemText(list_, item.iItem, 2, const_cast<wchar_t*>(memory.c_str()));
            ListView_SetItemText(list_, item.iItem, 3, const_cast<wchar_t*>(p.reason.c_str()));
            ListView_SetCheckState(list_, item.iItem, p.eligible);
            if (p.eligible)
                ++eligible;
            else
                ++protectedCount;
        }
        updatingList_ = false;
        Text(Tip, L"可清理 " + std::to_wstring(eligible) + L" 项 · 已保护 / 跳过 " +
                      std::to_wstring(protectedCount) + L" 项");
        Text(Status, snapshot_.valid ? L"取消勾选可跳过对应进程；执行前会再次核对前台与进程身份。"
                                     : L"扫描未完成，请查看下方记录并重试。 ");
        UpdateButtons();
    }
    void Scan() {
        if (busy_ || inspecting_)
            return;
        if (worker_.joinable())
            worker_.join();
        inspecting_ = true;
        Text(Status, L"正在核对后台程序与保护范围……");
        UpdateButtons();
        worker_ = std::thread([this] {
            auto data = std::make_unique<mc::Snapshot>();
            try {
                *data = mc::Inspect(ReadForeground());
            } catch (...) {
                data->error = L"读取进程信息时出现异常。";
            }
            if (PostMessageW(window_, kInspected, 0, reinterpret_cast<LPARAM>(data.get())))
                data.release();
        });
    }
    void Elevate() {
        auto path = mc::ExecutablePath();
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = L"--advanced";
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            DWORD error = GetLastError();
            AppendLog(error == ERROR_CANCELLED ? L"已取消管理员授权，没有执行全局清理。"
                                               : L"无法打开管理员窗口：" + mc::WinError(error));
        } else {
            if (info.hProcess)
                CloseHandle(info.hProcess);
            AppendLog(L"已打开高级窗口。所有全局项目默认关闭，请在该窗口中手动选择和执行。 ");
        }
    }
    void Begin() {
        if (busy_ || inspecting_ || (hasRun_ && GetTickCount64() - completedAt_ < kCooldownMilliseconds))
            return;
        mc::GlobalOptions options;
        std::vector<mc::Identity> targets;
        if (advanced_) {
            if (!admin_) {
                Elevate();
                return;
            }
            if (IsDlgButtonChecked(window_, Low) == BST_CHECKED)
                options.standby = mc::Standby::LowPriority;
            if (IsDlgButtonChecked(window_, All) == BST_CHECKED)
                options.standby = mc::Standby::All;
            options.fileCache = IsDlgButtonChecked(window_, Cache) == BST_CHECKED;
            if (!mc::ValidGlobalOptions(options)) {
                Text(Status, L"请先选择至少一个全局清理项目。 ");
                return;
            }
            std::wstring warning =
                L"全局缓存无法排除前台程序或 Minecraft，可能导致重新读取数据与短暂卡顿。\n\n";
            if (options.standby == mc::Standby::All)
                warning += L"你选择了“全部待机列表”，正在游戏或工作时不建议执行。\n\n";
            warning += L"确认现在执行所选全局清理？";
            if (MessageBoxW(window_, warning.c_str(), L"确认全局清理",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return;
        } else {
            for (int i = 0; i < static_cast<int>(snapshot_.processes.size()); ++i)
                if (snapshot_.processes[i].eligible && ListView_GetCheckState(list_, i))
                    targets.push_back(snapshot_.processes[i].id);
            if (targets.empty())
                return;
        }
        if (worker_.joinable())
            worker_.join();
        busy_ = true;
        cancel_.store(false);
        Text(Status, L"正在处理；可以取消尚未开始的项目。 ");
        Text(Clean, L"正在清理……");
        AppendLog(L"—— 手动清理开始 ——");
        UpdateButtons();
        worker_ = std::thread([this, targets = std::move(targets), options] {
            auto result = std::make_unique<mc::RunResult>();
            try {
                auto report = [this](const std::wstring& line) { PostLog(line); };
                *result =
                    advanced_
                        ? mc::CleanGlobal(options, cancel_, report)
                        : mc::CleanSelected(targets, [this] { return ReadForeground(); }, cancel_, report);
            } catch (...) {
                result->error = L"操作发生异常，已停止后续项目。";
            }
            if (PostMessageW(window_, kFinished, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    }
    void Finish(const mc::RunResult& result) {
        if (worker_.joinable())
            worker_.join();
        busy_ = false;
        // An unsuccessful lock or permission request must not leave the UI stuck.
        if (result.before.valid || !result.items.empty()) {
            hasRun_ = true;
            completedAt_ = GetTickCount64();
        }
        if (!result.error.empty())
            AppendLog(L"已停止：" + result.error);
        std::wstring summary = result.cancelled ? L"已取消后续项目。 " : L"处理结束。 ";
        summary += L"成功 " + std::to_wstring(result.succeeded) + L" · 跳过 " +
                   std::to_wstring(result.skipped) + L" · 失败 " + std::to_wstring(result.failed);
        AppendLog(summary);
        if (result.before.valid && result.after.valid)
            AppendLog(L"系统可用内存" + mc::AvailableChange(result.before.available, result.after.available) +
                      L"；该变化同时受其他程序影响。 ");
        if (!advanced_)
            AppendLog(L"工作集回收不会释放程序仍在使用的内存；后续访问时占用可能回升。 ");
        Text(Status, summary);
        UpdateNumbers();
        UpdateButtons();
        if (closePending_)
            DestroyWindow(window_);
    }
    void CreateControls() {
        Fonts();
        background_ = CreateSolidBrush(kBackground);
        Add(L"STATIC", advanced_ ? L"memclean  /  高级" : L"memclean", SS_LEFT, Heading);
        Add(L"STATIC",
            advanced_ ? L"全局缓存清理 · 每次手动选择 · 单独管理员窗口"
                      : L"轻量内存清理  /  保护正在使用的程序",
            SS_LEFT, Subtitle);
        Add(L"STATIC", L"正在读取内存状态", SS_LEFT, Available);
        Add(L"STATIC", L"", SS_LEFT, Total);
        Add(L"STATIC", advanced_ ? L"全局操作影响整个系统，不适用前台与 Minecraft 排除规则。" : L"", SS_LEFT,
            Foreground);
        Add(L"STATIC", L"", SS_LEFT, Tip);
        Add(L"STATIC", L"就绪", SS_LEFT, Status);
        Add(L"BUTTON", advanced_ ? (admin_ ? L"执行所选全局清理" : L"以管理员身份打开") : L"清理所选后台程序",
            WS_TABSTOP | BS_OWNERDRAW, Clean);
        Add(L"BUTTON", L"取消任务", WS_TABSTOP | BS_PUSHBUTTON, Cancel);
        log_ = Add(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, Log,
                   WS_EX_CLIENTEDGE);
        SendMessageW(log_, EM_SETLIMITTEXT, 220000, 0);
        if (!advanced_) {
            Add(L"BUTTON", L"刷新列表", WS_TABSTOP | BS_PUSHBUTTON, Refresh);
            Add(L"BUTTON", L"选择可清理项", WS_TABSTOP | BS_PUSHBUTTON, SelectAll);
            Add(L"BUTTON", L"取消勾选", WS_TABSTOP | BS_PUSHBUTTON, SelectNone);
            Add(L"BUTTON", L"高级全局清理…", WS_TABSTOP | BS_PUSHBUTTON, Advanced);
            SendMessageW(Control(Advanced), BCM_SETSHIELD, 0, TRUE);
            list_ =
                Add(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, List, WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                                         LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const wchar_t* labels[] = {L"程序", L"PID", L"工作集", L"保护 / 清理状态"};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(labels[i]);
                column.cx = D(140);
                ListView_InsertColumn(list_, i, &column);
            }
            AppendLog(L"默认只处理当前用户的后台桌面应用。Minecraft、Java、前台程序和系统程序均保留。 ");
            AppendLog(L"待机内存本来就可供 Windows 使用；不建议为了降低占用数字而反复清理。 ");
        } else {
            Text(Tip, L"低优先级待机列表相对温和。全部待机列表与文件缓存可能增加后续磁盘读取。\r\n所有项目默"
                      L"认关闭；不会清理全系统工作集，也不会强制写回修改页。 ");
            Add(L"BUTTON", L"不处理待机列表（默认）", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, None);
            Add(L"BUTTON", L"只清理低优先级待机列表", WS_TABSTOP | BS_AUTORADIOBUTTON, Low);
            Add(L"BUTTON", L"清理全部待机列表（可能出现明显卡顿）", WS_TABSTOP | BS_AUTORADIOBUTTON, All);
            Add(L"BUTTON", L"清理系统文件缓存（可能增加后续读取）", WS_GROUP | WS_TABSTOP | BS_AUTOCHECKBOX,
                Cache);
            CheckRadioButton(window_, None, None, None);
            AppendLog(L"此窗口仅在你点击执行并确认后才会进行全局操作。 ");
            AppendLog(L"管理员权限只用于所选项目，权限状态在每项操作后恢复。操作结果会逐项显示。 ");
        }
        Fonts();
        Layout();
        UpdateNumbers();
        UpdateButtons();
    }
    LRESULT DrawButton(DRAWITEMSTRUCT* item) {
        RECT rect = item->rcItem;
        bool disabled = (item->itemState & ODS_DISABLED) != 0;
        bool pressed = (item->itemState & ODS_SELECTED) != 0;
        HBRUSH brush = CreateSolidBrush(disabled ? RGB(190, 201, 207) : pressed ? RGB(0, 77, 68) : kAccent);
        FillRect(item->hDC, &rect, brush);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, RGB(255, 255, 255));
        auto old = SelectObject(item->hDC, font_);
        wchar_t title[128]{};
        GetWindowTextW(item->hwndItem, title, 128);
        DrawTextW(item->hDC, title, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, old);
        if (item->itemState & ODS_FOCUS) {
            InflateRect(&rect, -D(4), -D(4));
            DrawFocusRect(item->hDC, &rect);
        }
        return TRUE;
    }

public:
    explicit Application(HINSTANCE instance, bool advanced)
        : instance_(instance), advanced_(advanced), admin_(mc::IsAdministrator()),
          ownPath_(mc::ExecutablePath()) {}
    ~Application() {
        cancel_.store(true);
        if (worker_.joinable())
            worker_.join();
        if (hook_)
            UnhookWinEvent(hook_);
        if (font_)
            DeleteObject(font_);
        if (titleFont_)
            DeleteObject(titleFont_);
        if (numberFont_)
            DeleteObject(numberFont_);
        if (background_)
            DeleteObject(background_);
    }
    void RememberForeground(HWND foregroundWindow) {
        if (!foregroundWindow)
            return;
        DWORD pid = 0;
        GetWindowThreadProcessId(foregroundWindow, &pid);
        if (!pid || pid == GetCurrentProcessId())
            return;
        mc::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        auto path = process ? mc::ImagePath(process.get()) : std::wstring{};
        if (!path.empty() && _wcsicmp(path.c_str(), ownPath_.c_str()) == 0)
            return;
        auto id = process ? mc::Identify(process.get(), pid) : mc::Identity{};
        std::wstring name =
            path.empty() ? L"身份无法确认（暂停清理）" : path.substr(path.find_last_of(L"\\/") + 1);
        std::lock_guard<std::mutex> lock(foregroundMutex_);
        foreground_ = id;
        foregroundName_ = name;
    }
    mc::Identity ReadForeground() {
        // Also read synchronously on the worker; do not depend solely on hook delivery.
        RememberForeground(GetForegroundWindow());
        std::lock_guard<std::mutex> lock(foregroundMutex_);
        return foreground_;
    }
    static void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
        if (gApplication)
            gApplication->RememberForeground(hwnd);
    }
    bool Create() {
        RememberForeground(GetForegroundWindow());
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance_;
        cls.lpfnWndProc = WindowProc;
        cls.lpszClassName = kClass;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        cls.hIconSm = cls.hIcon;
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&cls))
            return false;
        dpi_ = GetDpiForSystem();
        RECT size{0, 0, D(1000), D(760)};
        AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        window_ =
            CreateWindowExW(0, kClass, advanced_ ? L"memclean · 高级全局清理" : L"memclean · 手动内存清理",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left,
                            size.bottom - size.top, nullptr, nullptr, instance_, this);
        if (!window_)
            return false;
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundEvent, 0,
                                0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        SetTimer(window_, 1, 1000, nullptr);
        if (!advanced_)
            Scan();
        return true;
    }
    bool FilterMessage(MSG* message) { return ::IsDialogMessageW(window_, message) != FALSE; }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->Message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_TIMER:
            ReadForeground();
            UpdateNumbers();
            return 0;
        case WM_SIZE:
            if (Control(Heading))
                Layout();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            Fonts();
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {D(900), D(745)};
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(window_, &rect);
            FillRect(reinterpret_cast<HDC>(wp), &rect,
                     background_ ? background_ : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND control = reinterpret_cast<HWND>(lp);
            if (control == log_) {
                SetTextColor(dc, kMuted);
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
            }
            int id = GetDlgCtrlID(control);
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, kBackground);
            SetTextColor(dc, id == Available ? kAccent : (id == Heading ? kInk : kMuted));
            return reinterpret_cast<LRESULT>(background_);
        }
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(wp), kBackground);
            return reinterpret_cast<LRESULT>(background_);
        case WM_DRAWITEM:
            if (wp == Clean)
                return DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case Refresh:
                Scan();
                return 0;
            case Clean:
                Begin();
                return 0;
            case Cancel:
                cancel_.store(true);
                Text(Status, L"正在取消后续项目；当前系统调用结束后即停止。 ");
                UpdateButtons();
                return 0;
            case Advanced:
                if (!busy_ && !inspecting_)
                    Elevate();
                return 0;
            case SelectAll:
            case SelectNone:
                if (!busy_ && !inspecting_) {
                    updatingList_ = true;
                    for (int i = 0; i < static_cast<int>(snapshot_.processes.size()); ++i)
                        ListView_SetCheckState(list_, i,
                                               LOWORD(wp) == SelectAll && snapshot_.processes[i].eligible);
                    updatingList_ = false;
                    UpdateButtons();
                }
                return 0;
            case IDCANCEL:
                SendMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == List && hdr->code == LVN_ITEMCHANGING && !updatingList_) {
                auto change = reinterpret_cast<NMLISTVIEW*>(lp);
                if (change->iItem >= 0 && change->iItem < static_cast<int>(snapshot_.processes.size()) &&
                    (change->uChanged & LVIF_STATE) &&
                    ((change->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2 &&
                    !snapshot_.processes[change->iItem].eligible)
                    return TRUE;
            }
            if (hdr->idFrom == List && hdr->code == LVN_ITEMCHANGED && !updatingList_)
                UpdateButtons();
            if (hdr->idFrom == List && hdr->code == NM_CUSTOMDRAW) {
                auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                    return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    size_t i = draw->nmcd.dwItemSpec;
                    if (i < snapshot_.processes.size())
                        draw->clrText = snapshot_.processes[i].eligible ? kInk : kMuted;
                    return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case kLog: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
            AppendLog(*line);
            return 0;
        }
        case kInspected: {
            std::unique_ptr<mc::Snapshot> data(reinterpret_cast<mc::Snapshot*>(lp));
            if (worker_.joinable())
                worker_.join();
            inspecting_ = false;
            snapshot_ = std::move(*data);
            if (!snapshot_.valid) {
                AppendLog(L"扫描失败：" + snapshot_.error);
                snapshot_.processes.clear();
            }
            Populate();
            if (closePending_)
                DestroyWindow(window_);
            return 0;
        }
        case kFinished: {
            std::unique_ptr<mc::RunResult> result(reinterpret_cast<mc::RunResult*>(lp));
            Finish(*result);
            return 0;
        }
        case WM_CLOSE:
            if (busy_ || inspecting_) {
                cancel_.store(true);
                closePending_ = true;
                Text(Status, L"正在结束当前任务，随后关闭窗口。 ");
                UpdateButtons();
            } else
                DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, 1);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wp, lp);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool advanced = false, valid = arguments && count == 1;
    if (arguments && count == 2 && wcscmp(arguments[1], L"--advanced") == 0) {
        advanced = true;
        valid = true;
    }
    if (arguments)
        LocalFree(arguments);
    if (!valid) {
        MessageBoxW(nullptr, L"支持的选项只有 --advanced（仅打开高级窗口，不执行清理）。", L"memclean",
                    MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    try {
        Application application(instance, advanced);
        gApplication = &application;
        if (!application.Create()) {
            MessageBoxW(nullptr, L"窗口创建失败。", L"memclean", MB_OK | MB_ICONERROR);
            return 1;
        }
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!application.FilterMessage(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        gApplication = nullptr;
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    } catch (...) {
        gApplication = nullptr;
        MessageBoxW(nullptr, L"程序遇到错误，请关闭后重试。没有启用任何自动清理。", L"memclean",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
