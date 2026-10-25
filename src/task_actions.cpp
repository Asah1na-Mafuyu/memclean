#include "task_manager.h"
#include <psapi.h>
#include <winternl.h>
#include <algorithm>
#include <sstream>

namespace mc::task {
namespace {
Result VerifiedProcess(Identity target, DWORD rights, Handle& handle) {
    handle.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | rights, FALSE, target.pid));
    if (!handle)
        return {false, L"无法打开进程：" + WinError(GetLastError())};
    if (!target.created || !(Identify(handle.get(), target.pid) == target))
        return {false, L"进程已退出或身份发生变化，请刷新后重试。"};
    ProcessInfo p;
    p.id = target;
    p.path = ImagePath(handle.get());
    p.name = p.path.substr(p.path.find_last_of(L"\\/") + 1);
    p.user = ProcessUser(handle.get(), &p.sid);
    BOOL critical = TRUE;
    p.queryable = IsProcessCritical(handle.get(), &critical) && !p.path.empty();
    p.critical = critical != FALSE;
    if (IsProtectedProcess(p, GetCurrentProcessId(), ExecutablePath()))
        return {false, L"系统、关键进程、工具自身或身份无法确认的进程仅可查看。"};
    return {true, L""};
}
struct WindowAction {
    DWORD pid = 0;
    bool activate = false, found = false;
};
BOOL CALLBACK ActOnWindow(HWND window, LPARAM data) {
    auto& action = *reinterpret_cast<WindowAction*>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != action.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;
    if (action.activate) {
        if (IsIconic(window))
            ShowWindowAsync(window, SW_RESTORE);
        action.found = SetForegroundWindow(window) != FALSE;
        return FALSE;
    }
    if (PostMessageW(window, WM_CLOSE, 0, 0))
        action.found = true;
    return TRUE;
}
std::wstring CommandLine(HANDLE process) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query)
        return L"不可用";
    ULONG size = 0;
    query(process, 60, nullptr, 0, &size);
    if (size < sizeof(UNICODE_STRING) || size > 1024 * 1024)
        return L"无权限或当前系统不支持";
    std::vector<BYTE> data(size);
    if (query(process, 60, data.data(), size, &size) < 0)
        return L"无权限或进程已退出";
    auto text = reinterpret_cast<UNICODE_STRING*>(data.data());
    auto begin = reinterpret_cast<uintptr_t>(data.data()), buffer = reinterpret_cast<uintptr_t>(text->Buffer);
    if (text->Length % sizeof(wchar_t) || buffer < begin || buffer - begin > data.size() ||
        text->Length > data.size() - (buffer - begin))
        return L"无法验证返回的数据";
    return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
}
} // namespace

Result EndProcess(Identity target, bool tree, const std::atomic_bool& cancel) {
    std::vector<Identity> targets{target};
    if (tree) {
        Sampler sampler;
        auto snapshot = sampler.Sample(false, false, false);
        if (!snapshot.error.empty())
            return {false, snapshot.error};
        targets = Descendants(snapshot.processes, target);
        if (targets.empty())
            return {false, L"进程已经退出或身份发生变化。"};
    }
    // Validate and retain every handle before terminating anything; refuse unsafe trees as a whole.
    std::vector<Handle> processes;
    for (auto id : targets) {
        if (cancel.load())
            return {false, L"操作已取消。"};
        Handle handle;
        auto check = VerifiedProcess(id, PROCESS_TERMINATE | SYNCHRONIZE, handle);
        if (!check.success)
            return {false, L"未执行结束操作。PID " + std::to_wstring(id.pid) + L"：" + check.message};
        processes.push_back(std::move(handle));
    }
    unsigned done = 0;
    for (auto& process : processes) {
        if (cancel.load())
            return {false, L"已取消后续项目；已发送 " + std::to_wstring(done) + L" 个结束请求。"};
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
            continue;
        if (!TerminateProcess(process.get(), 1))
            return {false, L"已发送 " + std::to_wstring(done) + L" 个结束请求，后续失败：" +
                               WinError(GetLastError())};
        ++done;
    }
    return {true, L"已发送 " + std::to_wstring(done) + L" 个结束请求。新建的子进程不属于此次快照。"};
}
Result ChangePriority(Identity target, DWORD priority) {
    if (priority != IDLE_PRIORITY_CLASS && priority != BELOW_NORMAL_PRIORITY_CLASS &&
        priority != NORMAL_PRIORITY_CLASS && priority != ABOVE_NORMAL_PRIORITY_CLASS &&
        priority != HIGH_PRIORITY_CLASS)
        return {false, L"不支持这个优先级；不提供实时优先级。"};
    Handle process;
    auto check = VerifiedProcess(target, PROCESS_SET_INFORMATION, process);
    if (!check.success)
        return check;
    if (!SetPriorityClass(process.get(), priority))
        return {false, WinError(GetLastError())};
    return {true, L"优先级已设为“" + PriorityName(priority) + L"”。"};
}
Result QueryAffinity(Identity target, DWORD_PTR& current, DWORD_PTR& allowed) {
    if (GetActiveProcessorGroupCount() != 1)
        return {false, L"多处理器组设备请在 Windows 系统任务管理器中设置关联性。"};
    Handle process;
    auto check = VerifiedProcess(target, 0, process);
    if (!check.success)
        return check;
    if (!GetProcessAffinityMask(process.get(), &current, &allowed) || !allowed)
        return {false, L"无法读取 CPU 关联性：" + WinError(GetLastError())};
    return {true, L""};
}
Result ChangeAffinity(Identity target, DWORD_PTR mask) {
    DWORD_PTR current = 0, allowed = 0;
    auto check = QueryAffinity(target, current, allowed);
    if (!check.success)
        return check;
    if (!mask || (mask & ~allowed))
        return {false, L"请至少选择一个有效的逻辑处理器。"};
    Handle process;
    check = VerifiedProcess(target, PROCESS_SET_INFORMATION, process);
    if (!check.success)
        return check;
    if (!SetProcessAffinityMask(process.get(), mask))
        return {false, WinError(GetLastError())};
    return {true, L"CPU 关联性已更新。"};
}
Result SetEcoQos(Identity target, bool enabled) {
    Handle process;
    auto check = VerifiedProcess(target, PROCESS_SET_INFORMATION, process);
    if (!check.success)
        return check;
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = enabled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    if (!SetProcessInformation(process.get(), ProcessPowerThrottling, &state, sizeof(state)))
        return {false, L"当前系统或进程不支持此操作：" + WinError(GetLastError())};
    return {true, enabled ? L"已请求节能 QoS（可能降低执行速度）。" : L"已关闭执行速度节能请求。"};
}
Result CloseProcessWindow(Identity target) {
    Handle process;
    auto check = VerifiedProcess(target, 0, process);
    if (!check.success)
        return check;
    WindowAction action{target.pid, false, false};
    EnumWindows(ActOnWindow, reinterpret_cast<LPARAM>(&action));
    return {action.found,
            action.found ? L"已请求正常关闭，程序可能提示保存文件。" : L"没有可关闭的窗口，或权限不足。"};
}
Result ActivateProcessWindow(Identity target) {
    Handle process;
    auto check = VerifiedProcess(target, 0, process);
    if (!check.success)
        return check;
    WindowAction action{target.pid, true, false};
    EnumWindows(ActOnWindow, reinterpret_cast<LPARAM>(&action));
    return {action.found,
            action.found ? L"已请求切换到程序。" : L"没有可切换的窗口，或 Windows 阻止了前台切换。"};
}
std::wstring ProcessDetails(Identity target) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid));
    if (!process || !(Identify(process.get(), target.pid) == target))
        return L"进程已退出、身份已变化或没有读取权限。";
    std::wstring sid;
    auto user = ProcessUser(process.get(), &sid);
    FILETIME created{}, exited{}, kernel{}, time{};
    GetProcessTimes(process.get(), &created, &exited, &kernel, &time);
    FILETIME local{};
    SYSTEMTIME calendar{};
    FileTimeToLocalFileTime(&created, &local);
    FileTimeToSystemTime(&local, &calendar);
    wchar_t start[80]{};
    swprintf_s(start, L"%04u-%02u-%02u %02u:%02u:%02u", calendar.wYear, calendar.wMonth, calendar.wDay,
               calendar.wHour, calendar.wMinute, calendar.wSecond);
    std::wostringstream out;
    out << L"PID：" << target.pid << L"\r\n用户：" << user << L"\r\n开始时间：" << start << L"\r\n优先级："
        << PriorityName(GetPriorityClass(process.get())) << L"\r\n\r\n路径：\r\n"
        << ImagePath(process.get()) << L"\r\n\r\n命令行：\r\n"
        << CommandLine(process.get());
    return out.str();
}
} // namespace mc::task
