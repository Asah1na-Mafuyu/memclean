#include "task_manager.h"
#include "task_dialogs.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <thread>

namespace mc::task {
namespace {
constexpr wchar_t kWindowClass[] = L"memclean.TaskManager.v1", kGraphClass[] = L"memclean.TaskGraphs.v1";
constexpr UINT kSampleReady = WM_APP + 20, kActionReady = WM_APP + 21;
enum ControlId {
    Tabs = 1000,
    Search,
    Refresh,
    Speed,
    Pause,
    Topmost,
    NewTask,
    Admin,
    Action,
    Details,
    Folder,
    Export,
    Status,
    Summary,
    Graphs,
    Native
};
enum Page { Processes, PerformancePage, DetailPage, Startup, Users, Services, History };
enum Command {
    ShowInfo = 3000,
    OpenFolder,
    FileProperties,
    SwitchWindow,
    CloseWindow,
    EndTask,
    EndTree,
    Affinity,
    EcoOn,
    EcoOff,
    PriorityLow,
    PriorityBelow,
    PriorityNormal,
    PriorityAbove,
    PriorityHigh,
    StartupToggle,
    StartupSettings,
    SessionDisconnect,
    SessionLogoff,
    SessionProcesses,
    ServiceStart,
    ServiceStop,
    ServiceRestart,
    ServicePause,
    ServiceContinue,
    ServiceAutomatic,
    ServiceManual,
    ServiceDisable,
    ServiceProcess
};
const COLORREF kBackground = RGB(245, 247, 250), kText = RGB(30, 44, 58), kMuted = RGB(92, 108, 120);
struct Cell {
    std::wstring text;
    double number = 0;
    bool numeric = false;
};
Cell TextCell(std::wstring text) {
    return {std::move(text), 0, false};
}
Cell NumberCell(double number, std::wstring text) {
    return {std::move(text), number, true};
}
Cell CountCell(uint64_t value) {
    return NumberCell(static_cast<double>(value), std::to_wstring(value));
}
Cell MemoryCell(uint64_t value, bool known = true) {
    return NumberCell(known ? static_cast<double>(value) : -1, known ? Bytes(value) : L"—");
}
struct Row {
    std::wstring key;
    size_t index = 0;
    std::vector<Cell> cells;
    bool protectedRow = false;
};
struct GraphPoint {
    ULONGLONG tick = 0;
    std::array<double, 6> value{};
    double diskWrite = -1, networkWrite = -1;
};
struct ModalGuard {
    unsigned& depth;
    explicit ModalGuard(unsigned& value) : depth(value) { ++depth; }
    ~ModalGuard() { --depth; }
};
std::wstring IdentityKey(Identity id) {
    return std::to_wstring(id.pid) + L":" + std::to_wstring(id.created);
}
std::wstring ControlText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}
std::wstring SystemFile(const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};
    GetSystemDirectoryW(directory, MAX_PATH);
    return std::wstring(directory) + L"\\" + name;
}
Result ShellOpen(HWND owner, const std::wstring& file, const wchar_t* parameters = nullptr,
                 const wchar_t* verb = L"open") {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_INVOKEIDLIST;
    info.lpFile = file.c_str();
    info.lpParameters = parameters;
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        return {false, WinError(GetLastError())};
    if (info.hProcess)
        CloseHandle(info.hProcess);
    return {true, L"已打开。"};
}
Result Reveal(HWND owner, const std::wstring& file, bool directory = false) {
    if (file.empty())
        return {false, L"没有可打开的位置。"};
    if (directory)
        return ShellOpen(owner, file);
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    auto args = L"/select,\"" + file + L"\"";
    return ShellOpen(owner, std::wstring(windows) + L"\\explorer.exe", args.c_str());
}

class Manager {
    HINSTANCE instance_;
    HWND window_ = nullptr, list_ = nullptr, tabs_ = nullptr, graph_ = nullptr;
    HFONT font_ = nullptr, headingFont_ = nullptr;
    HBRUSH background_ = nullptr;
    UINT dpi_ = 96;
    Page page_ = Processes;
    int sortColumn_ = 2;
    bool ascending_ = false, busy_ = false, actionBusy_ = false, closing_ = false, paused_ = false;
    unsigned modalDepth_ = 0;
    std::atomic_bool cancel_{false};
    std::thread worker_;
    std::unique_ptr<Sampler> sampler_;
    Snapshot snapshot_;
    std::vector<Row> rows_;
    std::vector<std::pair<std::wstring, int>> columns_;
    std::deque<GraphPoint> history_;
    std::wstring status_ = L"正在读取系统信息……";
    std::wstring ownPath_ = ExecutablePath();
    int D(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND Control(int id) const { return GetDlgItem(window_, id); }
    HWND Add(const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND handle = CreateWindowExW(ex, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return handle;
    }
    void Place(int id, int x, int y, int w, int h) { MoveWindow(Control(id), D(x), D(y), D(w), D(h), TRUE); }
    void SetStatus(std::wstring text) {
        status_ = std::move(text);
        SetWindowTextW(Control(Status), status_.c_str());
    }
    void Fonts() {
        HFONT old = font_, oldHeading = headingFont_;
        font_ =
            CreateFontW(-D(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        headingFont_ = CreateFontW(-D(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Microsoft YaHei UI");
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (old)
            DeleteObject(old);
        if (oldHeading)
            DeleteObject(oldHeading);
    }
    void Layout() {
        RECT rect{};
        GetClientRect(window_, &rect);
        int w = MulDiv(rect.right, 96, dpi_), h = MulDiv(rect.bottom, 96, dpi_);
        Place(NewTask, 20, 16, 116, 34);
        Place(Admin, 146, 16, 150, 34);
        Place(Native, 306, 16, 160, 34);
        Place(Topmost, w - 142, 19, 122, 28);
        Place(Summary, 20, 63, w - 40, 28);
        Place(Tabs, 20, 101, w - 40, 34);
        Place(Search, 20, 149, std::max(220, w - 590), 30);
        Place(Refresh, w - 550, 148, 92, 32);
        Place(Speed, w - 448, 149, 108, 150);
        Place(Pause, w - 330, 148, 100, 32);
        Place(Export, w - 220, 148, 200, 32);
        MoveWindow(list_, D(20), D(192), D(w - 40), D(std::max(280, h - 300)), TRUE);
        Place(Graphs, 20, 192, w - 40, std::max(280, h - 300));
        Place(Action, 20, h - 91, 142, 36);
        Place(Details, 174, h - 91, 100, 36);
        Place(Folder, 286, h - 91, 120, 36);
        Place(Status, 20, h - 43, w - 40, 28);
        InvalidateRect(window_, nullptr, TRUE);
    }
    std::wstring SelectedKey() const {
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        return selected >= 0 && selected < static_cast<int>(rows_.size()) ? rows_[selected].key : L"";
    }
    const Row* SelectedRow() const {
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        return selected >= 0 && selected < static_cast<int>(rows_.size()) ? &rows_[selected] : nullptr;
    }
    void Buttons() {
        const auto row = SelectedRow();
        bool ready = !actionBusy_ && !closing_;
        EnableWindow(Control(Refresh), ready && !busy_);
        EnableWindow(Control(Admin), ready && !IsAdministrator());
        EnableWindow(Control(NewTask), ready);
        bool protectedProcess = row && (page_ == Processes || page_ == DetailPage) && row->protectedRow;
        bool readOnlyStartup = row && page_ == Startup &&
                               (snapshot_.startups[row->index].source == StartupSource::UserFolder ||
                                snapshot_.startups[row->index].source == StartupSource::CommonFolder);
        EnableWindow(Control(Action), ready && row && !protectedProcess && !readOnlyStartup &&
                                          page_ != History && page_ != PerformancePage);
        EnableWindow(Control(Details), ready && row);
        EnableWindow(Control(Folder),
                     ready && row && (page_ == Processes || page_ == DetailPage || page_ == Startup));
        EnableWindow(Control(Export), ready && !rows_.empty() && page_ != PerformancePage);
        EnableWindow(Control(Search), page_ != PerformancePage);
        const wchar_t* action = page_ == Startup                        ? L"停用 / 恢复配置"
                                : (page_ == Services || page_ == Users) ? L"操作…"
                                                                        : L"结束任务";
        SetWindowTextW(Control(Action), action);
    }
    void Columns() {
        ListView_SetItemCountEx(list_, 0, 0);
        rows_.clear();
        while (ListView_DeleteColumn(list_, 0)) {
        }
        if (page_ == Processes)
            columns_ = {{L"进程", 190},   {L"PID", 70}, {L"CPU", 80},   {L"工作集", 105}, {L"读 I/O", 100},
                        {L"写 I/O", 100}, {L"GPU", 75}, {L"窗口", 230}, {L"用户", 170}};
        else if (page_ == DetailPage)
            columns_ = {{L"进程", 175},     {L"PID", 65},    {L"CPU", 75},   {L"工作集", 100},
                        {L"专用提交", 100}, {L"线程", 65},   {L"句柄", 70},  {L"优先级", 100},
                        {L"会话", 65},      {L"父 PID", 75}, {L"状态", 190}, {L"路径", 400}};
        else if (page_ == Startup)
            columns_ = {
                {L"名称", 180}, {L"配置状态", 180}, {L"来源", 190}, {L"命令 / 文件", 450}, {L"位置", 420}};
        else if (page_ == Users)
            columns_ = {{L"用户", 240},       {L"会话", 80},   {L"状态", 100},    {L"CPU", 90},
                        {L"工作集合计", 130}, {L"进程数", 90}, {L"会话名称", 180}};
        else if (page_ == Services)
            columns_ = {{L"服务名称", 190}, {L"显示名称", 280}, {L"状态", 110},
                        {L"PID", 70},       {L"启动类型", 150}, {L"账户", 220}};
        else if (page_ == History)
            columns_ = {{L"进程", 210},     {L"PID", 75},         {L"状态", 100},
                        {L"CPU 时间", 130}, {L"累计读 I/O", 150}, {L"累计写 I/O", 150}};
        else
            columns_.clear();
        for (size_t i = 0; i < columns_.size(); ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = columns_[i].first.data();
            column.cx = D(columns_[i].second);
            ListView_InsertColumn(list_, static_cast<int>(i), &column);
        }
        ShowWindow(list_, page_ == PerformancePage ? SW_HIDE : SW_SHOW);
        ShowWindow(graph_, page_ == PerformancePage ? SW_SHOW : SW_HIDE);
        Buttons();
    }
    void RebuildRows() {
        auto selected = SelectedKey();
        int top = ListView_GetTopIndex(list_);
        std::wstring filter = Lower(ControlText(Control(Search)));
        std::vector<Row> rows;
        auto add = [&](Row row) {
            bool matches = filter.empty();
            for (const auto& cell : row.cells)
                if (Lower(cell.text).find(filter) != std::wstring::npos) {
                    matches = true;
                    break;
                }
            if (matches)
                rows.push_back(std::move(row));
        };
        if (page_ == Processes || page_ == DetailPage)
            for (size_t i = 0; i < snapshot_.processes.size(); ++i) {
                const auto& p = snapshot_.processes[i];
                Row row;
                row.key = IdentityKey(p.id);
                row.index = i;
                row.protectedRow = IsProtectedProcess(p, GetCurrentProcessId(), ownPath_);
                row.cells = {TextCell(p.name), CountCell(p.id.pid), NumberCell(p.cpu, Percent(p.cpu)),
                             MemoryCell(p.workingSet, p.memoryKnown)};
                if (page_ == Processes) {
                    row.cells.push_back(NumberCell(p.readRate, Rate(p.readRate)));
                    row.cells.push_back(NumberCell(p.writeRate, Rate(p.writeRate)));
                    row.cells.push_back(NumberCell(p.gpu, Percent(p.gpu)));
                    row.cells.push_back(TextCell(p.title));
                    row.cells.push_back(TextCell(p.user));
                } else {
                    row.cells.push_back(MemoryCell(p.privateBytes, p.memoryKnown));
                    row.cells.push_back(CountCell(p.threads));
                    row.cells.push_back(CountCell(p.handles));
                    row.cells.push_back(TextCell(PriorityName(p.priority)));
                    row.cells.push_back(CountCell(p.session));
                    row.cells.push_back(CountCell(p.parent));
                    row.cells.push_back(TextCell(p.protection));
                    row.cells.push_back(TextCell(p.path));
                }
                if (filter.empty() || Lower(p.path).find(filter) != std::wstring::npos)
                    rows.push_back(std::move(row));
                else
                    add(std::move(row));
            }
        if (page_ == Startup)
            for (size_t i = 0; i < snapshot_.startups.size(); ++i) {
                const auto& entry = snapshot_.startups[i];
                std::wstring state = entry.disabled ? L"已由 memclean 停用" : L"配置存在 · 系统可另行禁用";
                add({std::to_wstring(static_cast<int>(entry.source)) + L":" + entry.name +
                         (entry.disabled ? L":off" : L":on"),
                     i,
                     {TextCell(entry.name), TextCell(state), TextCell(StartupSourceName(entry.source)),
                      TextCell(entry.command), TextCell(entry.location)}});
            }
        if (page_ == Users)
            for (size_t i = 0; i < snapshot_.sessions.size(); ++i) {
                const auto& s = snapshot_.sessions[i];
                add({std::to_wstring(s.id) + L":" + std::to_wstring(s.logon),
                     i,
                     {TextCell(s.name), CountCell(s.id), TextCell(SessionState(s.state)),
                      NumberCell(s.cpu, Percent(s.cpu)), MemoryCell(s.memory), CountCell(s.processes),
                      TextCell(s.station)}});
            }
        if (page_ == Services)
            for (size_t i = 0; i < snapshot_.services.size(); ++i) {
                const auto& s = snapshot_.services[i];
                add({s.name,
                     i,
                     {TextCell(s.name), TextCell(s.display), TextCell(ServiceState(s.state)),
                      CountCell(s.pid), TextCell(ServiceStartName(s.startType)), TextCell(s.account)}});
            }
        if (page_ == History)
            for (size_t i = 0; i < snapshot_.history.size(); ++i) {
                const auto& h = snapshot_.history[i];
                add({IdentityKey(h.id),
                     i,
                     {TextCell(h.name), CountCell(h.id.pid), TextCell(h.running ? L"运行中" : L"已退出"),
                      NumberCell(h.cpuKnown ? static_cast<double>(h.cpu) : -1,
                                 h.cpuKnown ? Duration(h.cpu / 10000000) : L"—"),
                      MemoryCell(h.read, h.ioKnown), MemoryCell(h.write, h.ioKnown)}});
            }
        if (sortColumn_ >= 0 && sortColumn_ < static_cast<int>(columns_.size())) {
            std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                const auto& x = a.cells[sortColumn_];
                const auto& y = b.cells[sortColumn_];
                if (x.numeric && y.numeric) {
                    if (x.number != y.number)
                        return ascending_ ? x.number < y.number : x.number > y.number;
                } else {
                    int compare = _wcsicmp(x.text.c_str(), y.text.c_str());
                    if (compare)
                        return ascending_ ? compare < 0 : compare > 0;
                }
                return a.key < b.key;
            });
        }
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        rows_ = std::move(rows);
        ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()),
                                LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].key == selected)
                ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
        if (top >= 0 && top < static_cast<int>(rows_.size()))
            ListView_EnsureVisible(list_, top, FALSE);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        Buttons();
    }
    void UpdateSummary() {
        const auto& p = snapshot_.performance;
        auto text = L"CPU " + Percent(p.cpu) + L"     内存 " +
                    (p.memory.valid ? std::to_wstring(p.memory.load) + L"%" : L"—") + L"     进程 " +
                    std::to_wstring(p.processes) + L"     线程 " + std::to_wstring(p.threads) +
                    L"     句柄 " + std::to_wstring(p.handles) + L"     已运行 " + Duration(p.uptime);
        SetWindowTextW(Control(Summary), text.c_str());
    }
    void Sample() {
        if (busy_ || closing_ || modalDepth_)
            return;
        if (worker_.joinable())
            worker_.join();
        busy_ = true;
        Buttons();
        auto page = page_;
        worker_ = std::thread([this, page] {
            auto result = std::make_unique<Snapshot>();
            try {
                if (!sampler_)
                    sampler_ = std::make_unique<Sampler>();
                *result = sampler_->Sample(page == Services, page == Startup, page == Users);
            } catch (...) {
                result->error = L"读取系统信息时发生异常。";
            }
            if (PostMessageW(window_, kSampleReady, static_cast<WPARAM>(page),
                             reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    }
    void StartAction(std::function<Result()> operation) {
        if (busy_ || closing_) {
            SetStatus(L"正在刷新，请稍后重试操作。 ");
            return;
        }
        if (worker_.joinable())
            worker_.join();
        busy_ = actionBusy_ = true;
        cancel_.store(false);
        Buttons();
        SetStatus(L"正在执行所选操作……");
        worker_ = std::thread([this, operation = std::move(operation)] {
            auto result = std::make_unique<Result>();
            try {
                *result = operation();
            } catch (...) {
                *result = {false, L"操作发生异常，已停止。"};
            }
            if (PostMessageW(window_, kActionReady, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    }
    bool Confirm(const std::wstring& message) {
        ModalGuard modal(modalDepth_);
        return MessageBoxW(window_, message.c_str(), L"确认操作",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }
    void Report(const Result& result) {
        SetStatus((result.success ? L"" : L"操作未完成：") + result.message);
    }
    void ShowSelectedInfo() {
        ModalGuard modal(modalDepth_);
        const auto row = SelectedRow();
        if (!row)
            return;
        if (page_ == Processes || page_ == DetailPage) {
            auto p = snapshot_.processes[row->index];
            ShowDetails(window_, p.name + L" · 详情", ProcessDetails(p.id));
        } else {
            std::wstring text;
            for (size_t i = 0; i < row->cells.size(); ++i)
                text += columns_[i].first + L"：" + row->cells[i].text + L"\r\n\r\n";
            if (page_ == Services)
                text += L"服务命令：\r\n" + snapshot_.services[row->index].path;
            if (page_ == History)
                text += L"统计仅覆盖此任务管理器窗口打开以来观察到的进程，不是 Windows 的历史应用数据库。";
            ShowDetails(window_, L"详情", text);
        }
    }
    void GoToProcess(DWORD pid) {
        page_ = DetailPage;
        TabCtrl_SetCurSel(tabs_, page_);
        SetWindowTextW(Control(Search), L"");
        Columns();
        RebuildRows();
        for (size_t i = 0; i < rows_.size(); ++i)
            if (snapshot_.processes[rows_[i].index].id.pid == pid) {
                ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list_, static_cast<int>(i), FALSE);
                break;
            }
    }
    void Execute(int command) {
        if (command == StartupSettings) {
            Report(ShellOpen(window_, L"ms-settings:startupapps"));
            return;
        }
        if (command == ShowInfo) {
            ShowSelectedInfo();
            return;
        }
        const Row* row = SelectedRow();
        if (!row || actionBusy_)
            return;
        if (page_ == Processes || page_ == DetailPage) {
            const auto p = snapshot_.processes[row->index];
            if (command == OpenFolder) {
                Report(Reveal(window_, p.path));
                return;
            }
            if (command == FileProperties) {
                Report(ShellOpen(window_, p.path, nullptr, L"properties"));
                return;
            }
            if (command == SwitchWindow) {
                Report(ActivateProcessWindow(p.id));
                return;
            }
            if (command == Affinity) {
                ModalGuard modal(modalDepth_);
                DWORD_PTR current = 0, allowed = 0, chosen = 0;
                auto result = QueryAffinity(p.id, current, allowed);
                if (!result.success) {
                    Report(result);
                    return;
                }
                if (ChooseAffinity(window_, current, allowed, chosen))
                    StartAction([=] { return ChangeAffinity(p.id, chosen); });
                return;
            }
            if (command == EndTask || command == EndTree || command == CloseWindow) {
                auto question =
                    (command == CloseWindow ? L"请求正常关闭 "
                     : command == EndTree   ? L"结束进程及当前已发现的子进程："
                                            : L"结束进程：") +
                    p.name + L"（PID " + std::to_wstring(p.id.pid) + L"）？\n\n" +
                    (command == CloseWindow
                         ? L"程序可能会提示保存工作。"
                         : L"未保存的工作可能丢失。任务管理器中的手动操作不会套用 Minecraft 清理排除规则。");
                if (!Confirm(question))
                    return;
                if (command == CloseWindow)
                    StartAction([=] { return CloseProcessWindow(p.id); });
                else
                    StartAction([this, p, command] { return EndProcess(p.id, command == EndTree, cancel_); });
                return;
            }
            if (command == EcoOn || command == EcoOff) {
                StartAction([=] { return SetEcoQos(p.id, command == EcoOn); });
                return;
            }
            DWORD priority = 0;
            switch (command) {
            case PriorityLow:
                priority = IDLE_PRIORITY_CLASS;
                break;
            case PriorityBelow:
                priority = BELOW_NORMAL_PRIORITY_CLASS;
                break;
            case PriorityNormal:
                priority = NORMAL_PRIORITY_CLASS;
                break;
            case PriorityAbove:
                priority = ABOVE_NORMAL_PRIORITY_CLASS;
                break;
            case PriorityHigh:
                priority = HIGH_PRIORITY_CLASS;
                break;
            }
            if (priority) {
                if (!Confirm(L"将 " + p.name + L" 的优先级设为“" + PriorityName(priority) +
                             L"”？\n\n这会改变 CPU 调度，较高优先级可能影响其他程序的响应。"))
                    return;
                StartAction([=] { return ChangePriority(p.id, priority); });
            }
        } else if (page_ == Startup) {
            auto entry = snapshot_.startups[row->index];
            if (command == OpenFolder) {
                if (entry.source == StartupSource::UserFolder || entry.source == StartupSource::CommonFolder)
                    Report(Reveal(window_, entry.location, true));
                else
                    Report(ShellOpen(window_, L"ms-settings:startupapps"));
            } else if (command == StartupToggle) {
                if (entry.source == StartupSource::UserFolder ||
                    entry.source == StartupSource::CommonFolder) {
                    Report(Reveal(window_, entry.location, true));
                    return;
                }
                auto text =
                    (entry.disabled ? L"恢复启动配置：" : L"停用启动配置：") + entry.name + L"？\n\n" +
                    (entry.disabled
                         ? L"将从 memclean 的备份恢复原始 Run 值；同名不同内容的值不会被覆盖。Windows "
                           L"自己的禁用状态仍需在系统设置中管理。"
                         : L"先校验备份，再从 Run 中移除该值。可以在此窗口恢复，当前运行的程序不受影响。");
                if (Confirm(text))
                    StartAction([=] { return ChangeStartup(entry); });
            }
        } else if (page_ == Services) {
            auto service = snapshot_.services[row->index];
            if (command == ServiceProcess) {
                GoToProcess(service.pid);
                return;
            }
            ServiceAction action;
            switch (command) {
            case ServiceStart:
                action = ServiceAction::Start;
                break;
            case ServiceStop:
                action = ServiceAction::Stop;
                break;
            case ServiceRestart:
                action = ServiceAction::Restart;
                break;
            case ServicePause:
                action = ServiceAction::Pause;
                break;
            case ServiceContinue:
                action = ServiceAction::Continue;
                break;
            case ServiceAutomatic:
                action = ServiceAction::Automatic;
                break;
            case ServiceManual:
                action = ServiceAction::Manual;
                break;
            case ServiceDisable:
                action = ServiceAction::Disabled;
                break;
            default:
                return;
            }
            const wchar_t* actions[]{L"启动", L"停止",         L"重启",         L"暂停",
                                     L"继续", L"改为自动启动", L"改为手动启动", L"改为禁用"};
            if (Confirm(L"对服务“" + service.display + L"”（" + service.name + L"）执行“" +
                        actions[command - ServiceStart] +
                        L"”？\n\n服务变更可能影响依赖它的应用。停止或重启不会自动停止依赖服务，"
                        L"已完成的动作不会回滚。"))
                StartAction([this, service, action] { return ChangeService(service, action, cancel_); });
        } else if (page_ == Users) {
            auto session = snapshot_.sessions[row->index];
            if (command == SessionProcesses) {
                page_ = Processes;
                TabCtrl_SetCurSel(tabs_, page_);
                Columns();
                SetWindowTextW(Control(Search), session.name.c_str());
                RebuildRows();
                return;
            }
            if (command == SessionDisconnect || command == SessionLogoff) {
                if (Confirm((command == SessionLogoff ? L"注销用户 " : L"断开用户会话 ") + session.name +
                            L"？\n\n" +
                            (command == SessionLogoff ? L"该用户的程序会被关闭，未保存的工作可能丢失。"
                                                      : L"会话中的应用将继续运行，但用户连接会断开。")))
                    StartAction([=] { return ChangeSession(session, command == SessionLogoff); });
            }
        }
    }
    void Menu(POINT point) {
        ModalGuard modal(modalDepth_);
        auto row = SelectedRow();
        if (!row || actionBusy_)
            return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ShowInfo, L"查看详情");
        if (page_ == Processes || page_ == DetailPage) {
            auto p = snapshot_.processes[row->index];
            bool protect = IsProtectedProcess(p, GetCurrentProcessId(), ExecutablePath());
            UINT manage = MF_STRING | (protect ? MF_GRAYED : 0);
            AppendMenuW(menu, MF_STRING | (p.path.empty() ? MF_GRAYED : 0), OpenFolder, L"打开文件所在位置");
            AppendMenuW(menu, MF_STRING | (p.path.empty() ? MF_GRAYED : 0), FileProperties, L"文件属性");
            AppendMenuW(menu, manage, SwitchWindow, L"切换到窗口");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, manage, CloseWindow, L"请求正常关闭");
            AppendMenuW(menu, manage, EndTask, L"结束进程…");
            AppendMenuW(menu, manage, EndTree, L"结束进程树…");
            HMENU priorities = CreatePopupMenu();
            const wchar_t* labels[]{L"低", L"低于正常", L"正常", L"高于正常", L"高"};
            for (int i = 0; i < 5; ++i)
                AppendMenuW(priorities, manage, PriorityLow + i, labels[i]);
            AppendMenuW(menu, MF_POPUP | (protect ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(priorities),
                        L"设置优先级");
            AppendMenuW(menu, manage, Affinity, L"设置 CPU 关联性…");
            AppendMenuW(menu, manage, EcoOn, L"请求节能 QoS");
            AppendMenuW(menu, manage, EcoOff, L"关闭节能 QoS 请求");
        } else if (page_ == Startup) {
            auto source = snapshot_.startups[row->index].source;
            bool readOnly = source == StartupSource::UserFolder || source == StartupSource::CommonFolder;
            AppendMenuW(menu, MF_STRING | (readOnly ? MF_GRAYED : 0), StartupToggle,
                        L"停用 / 恢复 Run 配置…");
            AppendMenuW(menu, MF_STRING, OpenFolder, L"打开位置 / 系统设置");
            AppendMenuW(menu, MF_STRING, StartupSettings, L"Windows 启动设置（含商店应用）");
        } else if (page_ == Services) {
            const wchar_t* labels[]{L"启动", L"停止",         L"重启",         L"暂停",
                                    L"继续", L"改为自动启动", L"改为手动启动", L"改为禁用"};
            for (int i = 0; i < 8; ++i)
                AppendMenuW(menu, MF_STRING, ServiceStart + i, labels[i]);
            AppendMenuW(menu, MF_STRING | (snapshot_.services[row->index].pid ? 0 : MF_GRAYED),
                        ServiceProcess, L"转到对应进程");
        } else if (page_ == Users) {
            AppendMenuW(menu, MF_STRING, SessionProcesses, L"查看该用户的进程");
            AppendMenuW(menu, MF_STRING, SessionDisconnect, L"断开其他用户会话…");
            AppendMenuW(menu, MF_STRING, SessionLogoff, L"注销其他用户…");
        }
        int choice =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (choice)
            Execute(choice);
    }
    void ExportCsv() {
        ModalGuard modal(modalDepth_);
        if (rows_.empty() || page_ == PerformancePage)
            return;
        wchar_t path[32768] = L"memclean-export.csv";
        OPENFILENAMEW save{};
        save.lStructSize = sizeof(save);
        save.hwndOwner = window_;
        save.lpstrFile = path;
        save.nMaxFile = 32768;
        save.lpstrFilter = L"CSV 表格\0*.csv\0\0";
        save.lpstrDefExt = L"csv";
        save.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&save))
            return;
        std::wstring csv;
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (i)
                csv += L",";
            csv += CsvCell(columns_[i].first);
        }
        csv += L"\r\n";
        for (const auto& row : rows_) {
            for (size_t i = 0; i < row.cells.size(); ++i) {
                if (i)
                    csv += L",";
                csv += CsvCell(row.cells[i].text);
            }
            csv += L"\r\n";
        }
        int length = WideCharToMultiByte(CP_UTF8, 0, csv.c_str(), static_cast<int>(csv.size()), nullptr, 0,
                                         nullptr, nullptr);
        std::string bytes = "\xEF\xBB\xBF";
        bytes.resize(static_cast<size_t>(length) + 3);
        WideCharToMultiByte(CP_UTF8, 0, csv.c_str(), static_cast<int>(csv.size()), bytes.data() + 3, length,
                            nullptr, nullptr);
        Handle file(
            CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        if (!file ||
            !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size())
            Report({false, L"导出失败：" + WinError(GetLastError())});
        else
            SetStatus(L"已导出当前筛选和排序后的表格。 ");
    }
    void NewProcess() {
        std::wstring command;
        {
            ModalGuard modal(modalDepth_);
            if (!PromptCommand(window_, command))
                return;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &process)) {
            Report({false, WinError(GetLastError())});
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetStatus(L"任务已启动。 ");
        Sample();
    }
    void PaintGraphs(HWND child) {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(child, &paint);
        RECT area{};
        GetClientRect(child, &area);
        if (area.right <= 0 || area.bottom <= 0) {
            EndPaint(child, &paint);
            return;
        }
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, area.right, area.bottom);
        auto oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &area, background_);
        SetBkMode(dc, TRANSPARENT);
        const wchar_t* names[]{
            L"CPU",       L"物理内存", L"磁盘 · 读取 / 写入", L"物理网卡 · 接收 / 发送", L"GPU · 最忙引擎",
            L"已提交内存"};
        const COLORREF colors[]{RGB(0, 112, 140), RGB(108, 71, 170), RGB(26, 125, 72),
                                RGB(173, 103, 0), RGB(0, 126, 140),  RGB(93, 98, 160)};
        const auto& p = snapshot_.performance;
        std::wstring values[]{Percent(p.cpu),
                              p.memory.valid ? Bytes(p.memory.total - p.memory.available) + L" / " +
                                                   Bytes(p.memory.total)
                                             : L"—",
                              Rate(p.diskRead) + L" / " + Rate(p.diskWrite),
                              Rate(p.networkRead) + L" / " + Rate(p.networkWrite),
                              Percent(p.gpu),
                              Bytes(p.commit) + L" / " + Bytes(p.commitLimit)};
        int gap = D(12), width = (area.right - gap) / 2, height = (area.bottom - gap * 2) / 3;
        ULONGLONG now = history_.empty() ? GetTickCount64() : history_.back().tick;
        for (int chart = 0; chart < 6; ++chart) {
            RECT card{(chart % 2) * (width + gap), (chart / 2) * (height + gap),
                      (chart % 2) * (width + gap) + width, (chart / 2) * (height + gap) + height};
            FillRect(dc, &card, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            auto oldFont = SelectObject(dc, font_);
            SetTextColor(dc, colors[chart]);
            RECT title{card.left + D(12), card.top + D(7), card.right - D(12), card.top + D(28)};
            DrawTextW(dc, names[chart], -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT valueRect{card.left + D(12), card.top + D(31), card.right - D(12), card.top + D(54)};
            DrawTextW(dc, values[chart].c_str(), -1, &valueRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT plot{card.left + D(12), card.top + D(61), card.right - D(12), card.bottom - D(19)};
            if (plot.bottom > plot.top) {
                HPEN grid = CreatePen(PS_SOLID, 1, RGB(227, 233, 237));
                auto oldPen = SelectObject(dc, grid);
                for (int j = 0; j <= 4; ++j) {
                    int y = plot.top + (plot.bottom - plot.top) * j / 4;
                    MoveToEx(dc, plot.left, y, nullptr);
                    LineTo(dc, plot.right, y);
                }
                SelectObject(dc, oldPen);
                DeleteObject(grid);
                double maximum = (chart == 2 || chart == 3) ? 1024.0 : 100.0;
                if (chart == 2 || chart == 3)
                    for (const auto& point : history_)
                        if (now - point.tick <= 120000) {
                            maximum = std::max(maximum, point.value[chart] * 1.15);
                            maximum =
                                std::max(maximum, (chart == 2 ? point.diskWrite : point.networkWrite) * 1.15);
                        }
                for (int series = 0; series < ((chart == 2 || chart == 3) ? 2 : 1); ++series) {
                    HPEN line = CreatePen(series ? PS_DASH : PS_SOLID, series ? 1 : D(2),
                                          series ? RGB(130, 150, 170) : colors[chart]);
                    oldPen = SelectObject(dc, line);
                    bool first = true;
                    for (const auto& point : history_) {
                        if (now - point.tick > 120000)
                            continue;
                        double value =
                            series ? (chart == 2 ? point.diskWrite : point.networkWrite) : point.value[chart];
                        if (value < 0 || !std::isfinite(value)) {
                            first = true;
                            continue;
                        }
                        int x = plot.right -
                                static_cast<int>((now - point.tick) *
                                                 static_cast<uint64_t>(plot.right - plot.left) / 120000);
                        int y = plot.bottom - static_cast<int>(std::clamp(value / maximum, 0.0, 1.0) *
                                                               (plot.bottom - plot.top));
                        if (first)
                            MoveToEx(dc, x, y, nullptr);
                        else
                            LineTo(dc, x, y);
                        first = false;
                    }
                    SelectObject(dc, oldPen);
                    DeleteObject(line);
                }
            }
            SetTextColor(dc, kMuted);
            RECT foot{card.left + D(12), card.bottom - D(17), card.right - D(12), card.bottom};
            auto note = chart == 2   ? L"最近 120 秒   ·   实线：读取   虚线：写入"
                        : chart == 3 ? L"最近 120 秒   ·   实线：接收   虚线：发送"
                                     : L"最近 120 秒   ·   — 表示等待采样或计数器不可用";
            DrawTextW(dc, note, -1, &foot, DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
        }
        BitBlt(target, 0, 0, area.right, area.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(child, &paint);
    }
    void CreateControls() {
        background_ = CreateSolidBrush(kBackground);
        Fonts();
        Add(L"BUTTON", L"运行新任务…", WS_TABSTOP | BS_PUSHBUTTON, NewTask);
        Add(L"BUTTON", L"以管理员身份重开", WS_TABSTOP | BS_PUSHBUTTON, Admin);
        SendMessageW(Control(Admin), BCM_SETSHIELD, 0, TRUE);
        Add(L"BUTTON", L"Windows 任务管理器", WS_TABSTOP | BS_PUSHBUTTON, Native);
        Add(L"BUTTON", L"窗口置顶", WS_TABSTOP | BS_AUTOCHECKBOX, Topmost);
        Add(L"STATIC", L"正在读取系统性能……", SS_LEFT, Summary);
        tabs_ = Add(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_SINGLELINE, Tabs);
        const wchar_t* pages[]{L"进程", L"性能", L"详细信息", L"启动项", L"用户", L"服务", L"本次运行统计"};
        for (int i = 0; i < 7; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(pages[i]);
            TabCtrl_InsertItem(tabs_, i, &item);
        }
        Add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Search, WS_EX_CLIENTEDGE);
        SendMessageW(Control(Search), EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"搜索进程名、PID、用户、路径…"));
        Add(L"BUTTON", L"刷新", WS_TABSTOP | BS_PUSHBUTTON, Refresh);
        Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Speed);
        for (auto text : {L"每 1 秒", L"每 2 秒", L"每 5 秒"})
            SendMessageW(Control(Speed), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(Control(Speed), CB_SETCURSEL, 1, 0);
        Add(L"BUTTON", L"暂停刷新", WS_TABSTOP | BS_PUSHBUTTON, Pause);
        Add(L"BUTTON", L"导出当前表格为 CSV…", WS_TABSTOP | BS_PUSHBUTTON, Export);
        list_ = Add(WC_LISTVIEWW, L"",
                    WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 1100,
                    WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list_,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        graph_ = CreateWindowExW(0, kGraphClass, L"", WS_CHILD, 0, 0, 1, 1, window_,
                                 reinterpret_cast<HMENU>(Graphs), instance_, this);
        Add(L"BUTTON", L"结束任务", WS_TABSTOP | BS_PUSHBUTTON, Action);
        Add(L"BUTTON", L"详情…", WS_TABSTOP | BS_PUSHBUTTON, Details);
        Add(L"BUTTON", L"打开位置", WS_TABSTOP | BS_PUSHBUTTON, Folder);
        Add(L"STATIC", status_.c_str(), SS_LEFT | SS_ENDELLIPSIS, Status);
        Columns();
        Layout();
        SetTimer(window_, 1, 2000, nullptr);
    }
    static LRESULT CALLBACK GraphProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto manager = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            manager = static_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
        }
        if (manager && message == WM_PAINT) {
            manager->PaintGraphs(window);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return TRUE;
        return DefWindowProcW(window, message, wp, lp);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto manager = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            manager = static_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            manager->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
        }
        return manager ? manager->Message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_SIZE:
            if (Control(Summary))
                Layout();
            return 0;
        case WM_TIMER:
            if (!paused_ && !IsIconic(window_))
                Sample();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {D(1060), D(730)};
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            Fonts();
            auto rect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Columns();
            RebuildRows();
            Layout();
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(window_, &rect);
            FillRect(reinterpret_cast<HDC>(wp), &rect, background_);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wp), kText);
            return reinterpret_cast<LRESULT>(background_);
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(background_);
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == Search && HIWORD(wp) == EN_CHANGE) {
                RebuildRows();
                return 0;
            }
            if (id == Speed && HIWORD(wp) == CBN_SELCHANGE) {
                int speeds[]{1000, 2000, 5000};
                auto index = SendMessageW(Control(Speed), CB_GETCURSEL, 0, 0);
                if (index >= 0 && index < 3)
                    SetTimer(window_, 1, speeds[index], nullptr);
                return 0;
            }
            if (id == Refresh) {
                Sample();
                return 0;
            }
            if (id == Pause) {
                paused_ = !paused_;
                SetWindowTextW(Control(Pause), paused_ ? L"继续刷新" : L"暂停刷新");
                if (!paused_)
                    Sample();
                return 0;
            }
            if (id == Topmost) {
                SetWindowPos(window_, IsDlgButtonChecked(window_, Topmost) ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                             0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            if (id == Admin) {
                Report(ShellOpen(window_, ExecutablePath(), L"--taskmgr", L"runas"));
                return 0;
            }
            if (id == Native) {
                Report(ShellOpen(window_, SystemFile(L"taskmgr.exe")));
                return 0;
            }
            if (id == NewTask) {
                NewProcess();
                return 0;
            }
            if (id == Details) {
                ShowSelectedInfo();
                return 0;
            }
            if (id == Folder) {
                Execute(OpenFolder);
                return 0;
            }
            if (id == Export) {
                ExportCsv();
                return 0;
            }
            if (id == Action) {
                if (page_ == Startup)
                    Execute(StartupToggle);
                else if (page_ == Services || page_ == Users) {
                    RECT rect{};
                    GetWindowRect(Control(Action), &rect);
                    Menu({rect.left, rect.top});
                } else
                    Execute(EndTask);
                return 0;
            }
            if (id == IDCANCEL) {
                SendMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == Tabs && hdr->code == TCN_SELCHANGE) {
                page_ = static_cast<Page>(TabCtrl_GetCurSel(tabs_));
                sortColumn_ = page_ == Processes || page_ == DetailPage ? 2 : 0;
                ascending_ = sortColumn_ == 0;
                SetWindowTextW(Control(Search), L"");
                Columns();
                RebuildRows();
                if (page_ == History)
                    SetStatus(
                        L"仅统计此窗口打开以来观察到的 CPU 和 I/O；最多保留 5000 个已结束或活动进程记录。 ");
                else if (page_ == Startup)
                    SetStatus(L"Run 配置可备份停用与恢复；启动目录只读。Windows "
                              L"自己的禁用状态与商店应用请使用系统启动设置。 ");
                else if (page_ == PerformancePage)
                    SetStatus(snapshot_.performance.processorName + L"  ·  " +
                              snapshot_.performance.graphicsName);
                else
                    SetStatus(L"右键可查看更多操作；未知数据以 — 显示，系统与关键进程受到保护。 ");
                Sample();
                return 0;
            }
            if (hdr->hwndFrom == list_) {
                if (hdr->code == LVN_GETDISPINFOW) {
                    auto info = reinterpret_cast<NMLVDISPINFOW*>(lp);
                    if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 &&
                        info->item.iItem < static_cast<int>(rows_.size()) && info->item.iSubItem >= 0 &&
                        info->item.iSubItem < static_cast<int>(rows_[info->item.iItem].cells.size()))
                        info->item.pszText = const_cast<wchar_t*>(
                            rows_[info->item.iItem].cells[info->item.iSubItem].text.c_str());
                    return 0;
                }
                if (hdr->code == LVN_COLUMNCLICK) {
                    int column = reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;
                    ascending_ = column == sortColumn_ ? !ascending_ : true;
                    sortColumn_ = column;
                    RebuildRows();
                    return 0;
                }
                if (hdr->code == LVN_ITEMCHANGED) {
                    Buttons();
                    return 0;
                }
                if (hdr->code == NM_DBLCLK) {
                    ShowSelectedInfo();
                    return 0;
                }
                if (hdr->code == NM_RCLICK) {
                    POINT point{};
                    GetCursorPos(&point);
                    Menu(point);
                    return 0;
                }
                if (hdr->code == LVN_KEYDOWN) {
                    if (reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE &&
                        (page_ == Processes || page_ == DetailPage))
                        Execute(EndTask);
                    return 0;
                }
                if (hdr->code == NM_CUSTOMDRAW) {
                    auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                        return CDRF_NOTIFYITEMDRAW;
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        size_t i = draw->nmcd.dwItemSpec;
                        draw->clrText = i < rows_.size() && rows_[i].protectedRow ? kMuted : kText;
                        return CDRF_DODEFAULT;
                    }
                }
            }
            break;
        }
        case kSampleReady: {
            std::unique_ptr<Snapshot> data(reinterpret_cast<Snapshot*>(lp));
            if (worker_.joinable())
                worker_.join();
            busy_ = false;
            if (!data->error.empty())
                SetStatus(L"刷新失败：" + data->error);
            else {
                snapshot_ = std::move(*data);
                UpdateSummary();
                const auto& p = snapshot_.performance;
                history_.push_back({GetTickCount64(),
                                    {p.cpu, p.memory.valid ? static_cast<double>(p.memory.load) : -1,
                                     p.diskRead, p.networkRead, p.gpu,
                                     p.commitLimit ? 100.0 * static_cast<double>(p.commit) /
                                                         static_cast<double>(p.commitLimit)
                                                   : -1},
                                    p.diskWrite,
                                    p.networkWrite});
                while (history_.size() > 256)
                    history_.pop_front();
                RebuildRows();
                InvalidateRect(graph_, nullptr, FALSE);
                if (page_ == Services && !snapshot_.servicesError.empty())
                    SetStatus(snapshot_.servicesError);
                if (page_ == Startup && !snapshot_.startupsError.empty())
                    SetStatus(snapshot_.startupsError);
                if (page_ == Users && !snapshot_.sessionsError.empty())
                    SetStatus(snapshot_.sessionsError);
                if (status_ == L"正在读取系统信息……")
                    SetStatus(L"双击查看详情，右键打开操作菜单。默认每 2 秒刷新；最小化时暂停采样。 ");
            }
            Buttons();
            if (closing_)
                DestroyWindow(window_);
            else if (static_cast<Page>(wp) != page_)
                Sample();
            return 0;
        }
        case kActionReady: {
            std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));
            if (worker_.joinable())
                worker_.join();
            busy_ = actionBusy_ = false;
            Report(*result);
            Buttons();
            if (closing_)
                DestroyWindow(window_);
            else
                Sample();
            return 0;
        }
        case WM_CLOSE:
            if (busy_) {
                closing_ = true;
                cancel_.store(true);
                KillTimer(window_, 1);
                SetStatus(L"正在等待当前操作结束后关闭……");
                Buttons();
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

public:
    explicit Manager(HINSTANCE instance) : instance_(instance) {}
    ~Manager() {
        cancel_.store(true);
        if (worker_.joinable())
            worker_.join();
        if (font_)
            DeleteObject(font_);
        if (headingFont_)
            DeleteObject(headingFont_);
        if (background_)
            DeleteObject(background_);
    }
    bool Create() {
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.hInstance = instance_;
        type.lpfnWndProc = WindowProc;
        type.lpszClassName = kWindowClass;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        type.hIconSm = type.hIcon;
        if (!RegisterClassExW(&type))
            return false;
        type.lpfnWndProc = GraphProc;
        type.lpszClassName = kGraphClass;
        if (!RegisterClassExW(&type))
            return false;
        dpi_ = GetDpiForSystem();
        RECT size{0, 0, D(1220), D(820)};
        AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        window_ =
            CreateWindowExW(0, kWindowClass,
                            IsAdministrator() ? L"memclean · 任务管理器（管理员）" : L"memclean · 任务管理器",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left,
                            size.bottom - size.top, nullptr, nullptr, instance_, this);
        if (!window_)
            return false;
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        Sample();
        return true;
    }
    int Loop() {
        MSG message{};
        int status = 0;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_F5) {
                Sample();
                continue;
            }
            if (message.message == WM_KEYDOWN && message.wParam == 'F' && GetKeyState(VK_CONTROL) < 0) {
                SetFocus(Control(Search));
                continue;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return status < 0 ? 1 : static_cast<int>(message.wParam);
    }
};
} // namespace
int RunWindow(HINSTANCE instance) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    {
        Manager manager(instance);
        if (manager.Create())
            result = manager.Loop();
        else
            MessageBoxW(nullptr, L"任务管理器窗口创建失败。", L"memclean", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    return result;
}
} // namespace mc::task
