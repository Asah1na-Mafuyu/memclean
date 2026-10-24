#include "core.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mc {
namespace {
constexpr uint64_t kMinimumWorkingSet = 16ull * 1024 * 1024;
constexpr ULONG kSystemMemoryListInformation = 80;
enum class MemoryListCommand : ULONG { PurgeStandbyList = 4, PurgeLowPriorityStandbyList = 5 };
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}
bool Under(const std::wstring& path, const std::wstring& directory) {
    auto p = Lower(path), d = Lower(directory);
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    return !d.empty() && p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == L'\\';
}
std::vector<BYTE> UserSid(HANDLE process) {
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return {};
    token.reset(raw);
    DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    if (!length)
        return {};
    std::vector<BYTE> data(length);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), length, &length))
        return {};
    auto user = reinterpret_cast<TOKEN_USER*>(data.data());
    if (!IsValidSid(user->User.Sid))
        return {};
    std::vector<BYTE> sid(GetLengthSid(user->User.Sid));
    if (!CopySid(static_cast<DWORD>(sid.size()), sid.data(), user->User.Sid))
        return {};
    return sid;
}
bool Elevated(HANDLE process, bool& value) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &length))
        return false;
    value = elevation.TokenIsElevated != 0;
    return true;
}
BOOL CALLBACK CollectWindows(HWND window, LPARAM parameter) {
    if (IsWindowVisible(window)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid)
            reinterpret_cast<std::unordered_set<DWORD>*>(parameter)->insert(pid);
    }
    return TRUE;
}
void Propagate(std::vector<Process>& processes, std::unordered_set<DWORD>& set) {
    std::unordered_map<DWORD, const Process*> byPid;
    for (const auto& p : processes)
        byPid[p.id.pid] = &p;
    bool changed;
    do {
        changed = false;
        for (const auto& p : processes) {
            auto parent = byPid.find(p.parent);
            // A recycled parent PID must not create a fictitious process tree.
            if (parent != byPid.end() && p.id.created && parent->second->id.created &&
                parent->second->id.created <= p.id.created && set.count(p.parent) && !set.count(p.id.pid)) {
                set.insert(p.id.pid);
                changed = true;
            }
        }
    } while (changed);
}
std::wstring WindowsPath() {
    wchar_t path[MAX_PATH]{};
    UINT n = GetWindowsDirectoryW(path, MAX_PATH);
    return n && n < MAX_PATH ? path : L"";
}

// A privilege is enabled only for the duration of one operation, then restored.
class Privilege {
    Handle token_;
    TOKEN_PRIVILEGES previous_{};
    bool changed_ = false;
    DWORD error_ = ERROR_SUCCESS;

public:
    explicit Privilege(const wchar_t* name) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw)) {
            error_ = GetLastError();
            return;
        }
        token_.reset(raw);
        TOKEN_PRIVILEGES desired{};
        desired.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, name, &desired.Privileges[0].Luid)) {
            error_ = GetLastError();
            return;
        }
        desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD length = 0;
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(token_.get(), FALSE, &desired, sizeof(previous_), &previous_, &length)) {
            error_ = GetLastError();
            return;
        }
        error_ = GetLastError(); // TRUE can still mean ERROR_NOT_ALL_ASSIGNED.
        changed_ = previous_.PrivilegeCount != 0;
    }
    ~Privilege() {
        if (changed_)
            AdjustTokenPrivileges(token_.get(), FALSE, &previous_, 0, nullptr, nullptr);
    }
    bool valid() const { return error_ == ERROR_SUCCESS; }
    DWORD error() const { return error_; }
};
void Record(RunResult& result, ItemResult item, const Reporter& report) {
    if (item.kind == ResultKind::Success)
        ++result.succeeded;
    else if (item.kind == ResultKind::Skipped)
        ++result.skipped;
    else
        ++result.failed;
    const wchar_t* prefix = item.kind == ResultKind::Success   ? L"完成  "
                            : item.kind == ResultKind::Skipped ? L"跳过  "
                                                               : L"失败  ";
    report(prefix + item.name + (item.pid ? L" [" + std::to_wstring(item.pid) + L"]" : L"") + L"：" +
           item.detail);
    result.items.push_back(std::move(item));
}
} // namespace

bool operator==(const Identity& a, const Identity& b) {
    return a.pid == b.pid && a.created == b.created;
}
Identity Identify(HANDLE process, DWORD pid) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return {};
    return {pid, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
}
Identity Identify(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process ? Identify(process.get(), pid) : Identity{};
}
std::wstring ImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}
std::wstring ExecutablePath() {
    return ImagePath(GetCurrentProcess());
}
std::wstring WinError(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = n && buffer ? buffer : L"未知错误";
    if (buffer)
        LocalFree(buffer);
    while (!message.empty() && iswspace(message.back()))
        message.pop_back();
    return message + L"（" + std::to_wstring(code) + L"）";
}
std::wstring Bytes(uint64_t bytes) {
    std::wostringstream text;
    if (bytes >= (1ull << 30))
        text << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / (1ull << 30) << L" GB";
    else
        text << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / (1ull << 20) << L" MB";
    return text.str();
}
std::wstring AvailableChange(uint64_t before, uint64_t after) {
    // Subtract only in the proven non-negative direction; never underflow uint64_t.
    return after >= before ? L"增加 " + Bytes(after - before) : L"减少 " + Bytes(before - after);
}
Memory ReadMemory() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory))
        return {};
    return {memory.ullTotalPhys, memory.ullAvailPhys, memory.dwMemoryLoad, true};
}
bool IsAdministrator() {
    bool elevated = false;
    return Elevated(GetCurrentProcess(), elevated) && elevated;
}
bool IsMinecraftOrJava(const std::wstring& name, const std::wstring& path) {
    const auto n = Lower(name), p = Lower(path);
    if (n == L"java.exe" || n == L"javaw.exe" || n == L"javaws.exe" || n == L"openjdk.exe")
        return true;
    const wchar_t* names[] = {L"minecraft",     L"pcl",      L"hmcl",      L"bakaxl",
                              L"prismlauncher", L"multimc",  L"polymc",    L"atlauncher",
                              L"gdlauncher",    L"modrinth", L"curseforge"};
    for (auto word : names)
        if (n.find(word) != std::wstring::npos)
            return true;
    return p.find(L"\\.minecraft\\") != std::wstring::npos || p.find(L"\\minecraft\\") != std::wstring::npos;
}

void ApplyPolicy(std::vector<Process>& processes, Identity foreground, DWORD ownPid,
                 const std::wstring& ownPath, const std::wstring& windowsPath) {
    std::unordered_set<DWORD> front, game, desktop;
    std::wstring frontPath;
    bool frontVerified = false;
    for (auto& p : processes) {
        p.reason.clear();
        p.eligible = false;
        if (p.id == foreground && foreground.created && p.verified) {
            frontVerified = true;
            frontPath = Lower(p.path);
            front.insert(p.id.pid);
        }
        if (p.desktopTree)
            desktop.insert(p.id.pid);
        if (IsMinecraftOrJava(p.name, p.path))
            game.insert(p.id.pid);
    }
    // Browsers and Electron apps can keep renderer processes beside the window owner.
    if (!frontPath.empty())
        for (const auto& p : processes)
            if (Lower(p.path) == frontPath)
                front.insert(p.id.pid);
    Propagate(processes, front);
    Propagate(processes, game);
    Propagate(processes, desktop);
    for (auto& p : processes) {
        p.desktopTree = desktop.count(p.id.pid) != 0;
        if (p.id.pid == ownPid || (!ownPath.empty() && Lower(p.path) == Lower(ownPath)))
            p.reason = L"清理工具自身";
        else if (game.count(p.id.pid))
            p.reason = L"保护 Minecraft / Java";
        else if (front.count(p.id.pid))
            p.reason = L"保护前台程序及关联进程";
        else if (!frontVerified)
            p.reason = L"前台身份未确认，暂不清理";
        else if (!p.verified || !p.id.created || p.path.empty())
            p.reason = L"无法确认进程身份";
        else if (!p.sameUser || !p.sameSession)
            p.reason = L"其他用户或系统会话";
        else if (p.elevated || p.critical || Under(p.path, windowsPath))
            p.reason = L"系统或管理员程序";
        else if (!p.desktopTree)
            p.reason = L"非桌面应用，保守跳过";
        else if (p.workingSet < kMinimumWorkingSet)
            p.reason = L"工作集小于 16 MB";
        else {
            p.eligible = true;
            p.reason = L"可清理的后台程序";
        }
    }
}

Snapshot Inspect(Identity foreground) {
    Snapshot result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        result.error = WinError(GetLastError());
        return result;
    }
    auto ourSid = UserSid(GetCurrentProcess());
    DWORD ourSession = 0;
    auto windowsPath = WindowsPath();
    if (ourSid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &ourSession) || windowsPath.empty()) {
        result.error = L"无法读取当前用户、会话或 Windows 目录，已停止扫描。";
        return result;
    }
    std::unordered_set<DWORD> desktop;
    if (!EnumWindows(CollectWindows, reinterpret_cast<LPARAM>(&desktop))) {
        result.error = L"无法读取桌面窗口，已停止扫描。";
        return result;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        result.error = WinError(GetLastError());
        return result;
    }
    do {
        Process p;
        p.id.pid = entry.th32ProcessID;
        p.parent = entry.th32ParentProcessID;
        p.name = entry.szExeFile;
        p.desktopTree = desktop.count(p.id.pid) != 0;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.id.pid));
        if (process) {
            p.id = Identify(process.get(), entry.th32ProcessID);
            if (!p.id.pid)
                p.id.pid = entry.th32ProcessID;
            p.path = ImagePath(process.get());
            auto sid = UserSid(process.get());
            p.sameUser = !sid.empty() && EqualSid(sid.data(), ourSid.data());
            DWORD session = 0;
            p.sameSession = ProcessIdToSessionId(p.id.pid, &session) && session == ourSession && session != 0;
            BOOL critical = TRUE;
            bool elevationKnown = Elevated(process.get(), p.elevated);
            bool criticalKnown = IsProcessCritical(process.get(), &critical) != FALSE;
            p.critical = critical != FALSE;
            p.verified = p.id.created && !p.path.empty() && !sid.empty() && elevationKnown && criticalKnown;
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(process.get(), &counters, sizeof(counters)))
                p.workingSet = counters.WorkingSetSize;
        }
        result.processes.push_back(std::move(p));
    } while (Process32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        result.error = L"进程列表读取不完整，已停止扫描。";
        return result;
    }
    ApplyPolicy(result.processes, foreground, GetCurrentProcessId(), ExecutablePath(), windowsPath);
    std::stable_sort(result.processes.begin(), result.processes.end(),
                     [](const Process& a, const Process& b) {
                         if (a.eligible != b.eligible)
                             return a.eligible > b.eligible;
                         return a.workingSet > b.workingSet;
                     });
    result.valid = true;
    return result;
}

OperationGuard::OperationGuard() {
    // Keep the original name so memclean and pre-rename builds cannot clean concurrently.
    mutex_.reset(CreateMutexW(nullptr, FALSE, L"Local\\MemoryClean.ManualOperation.v1"));
    if (!mutex_)
        return;
    DWORD result = WaitForSingleObject(mutex_.get(), 0);
    owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}
OperationGuard::~OperationGuard() {
    if (owned_)
        ReleaseMutex(mutex_.get());
}

RunResult CleanSelected(const std::vector<Identity>& targets, const ForegroundReader& foreground,
                        const std::atomic_bool& cancel, const Reporter& report) {
    RunResult result;
    OperationGuard guard;
    if (!guard.acquired()) {
        result.error = L"另一个清理任务正在运行，或任务锁不可访问。";
        return result;
    }
    result.before = ReadMemory();
    // Rebuild the tree for every target so newly foregrounded applications are protected.
    for (const auto& target : targets) {
        if (cancel.load()) {
            result.cancelled = true;
            break;
        }
        const Identity front = foreground();
        auto snapshot = Inspect(front);
        if (!snapshot.valid) {
            result.error = snapshot.error;
            break;
        }
        auto match = std::find_if(snapshot.processes.begin(), snapshot.processes.end(),
                                  [&](const Process& p) { return p.id == target; });
        if (match == snapshot.processes.end()) {
            Record(result, {ResultKind::Skipped, target.pid, L"进程", L"已退出或身份发生变化"}, report);
            continue;
        }
        if (!match->eligible) {
            Record(result, {ResultKind::Skipped, target.pid, match->name, match->reason}, report);
            continue;
        }
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, target.pid));
        if (!process) {
            DWORD error = GetLastError();
            Record(result,
                   {ResultKind::Skipped, target.pid, match->name, L"无法获得权限：" + WinError(error)},
                   report);
            continue;
        }
        // Keep this exact handle from identity validation through the API call (PID reuse safe).
        if (!(Identify(process.get(), target.pid) == target)) {
            Record(result, {ResultKind::Skipped, target.pid, match->name, L"身份已变化"}, report);
            continue;
        }
        if (cancel.load()) {
            result.cancelled = true;
            break;
        }
        // Fail closed if foreground changes after the policy snapshot.
        if (!(foreground() == front)) {
            Record(result, {ResultKind::Skipped, target.pid, match->name, L"前台已切换，本次跳过"}, report);
            continue;
        }
        BOOL critical = TRUE;
        if (!IsProcessCritical(process.get(), &critical) || critical) {
            Record(result, {ResultKind::Skipped, target.pid, match->name, L"关键进程或状态无法确认"}, report);
            continue;
        }
        if (EmptyWorkingSet(process.get()))
            Record(result, {ResultKind::Success, target.pid, match->name, L"已请求回收工作集"}, report);
        else
            Record(result, {ResultKind::Failed, target.pid, match->name, WinError(GetLastError())}, report);
    }
    result.after = ReadMemory();
    return result;
}

bool ValidGlobalOptions(GlobalOptions options) {
    bool known = options.standby == Standby::None || options.standby == Standby::LowPriority ||
                 options.standby == Standby::All;
    return known && (options.standby != Standby::None || options.fileCache);
}
RunResult CleanGlobal(GlobalOptions options, const std::atomic_bool& cancel, const Reporter& report) {
    RunResult result;
    if (!ValidGlobalOptions(options)) {
        result.error = L"未选择有效的高级清理项目。";
        return result;
    }
    if (!IsAdministrator()) {
        result.error = L"高级清理需要在管理员窗口中手动执行。";
        return result;
    }
    OperationGuard guard;
    if (!guard.acquired()) {
        result.error = L"另一个清理任务正在运行，或任务锁不可访问。";
        return result;
    }
    result.before = ReadMemory();
    if (options.standby != Standby::None && !cancel.load()) {
        ItemResult item;
        item.name = options.standby == Standby::LowPriority ? L"低优先级待机列表" : L"全部待机列表";
        Privilege privilege(SE_PROF_SINGLE_PROCESS_NAME);
        if (!privilege.valid()) {
            item.kind = ResultKind::Failed;
            item.detail = L"权限启用失败：" + WinError(privilege.error());
        } else {
            // No undocumented structures. Use the fixed 32-bit MemoryListCommand value.
            using NtSet = LONG(NTAPI*)(ULONG, PVOID, ULONG);
            using NtError = ULONG(WINAPI*)(LONG);
            auto module = GetModuleHandleW(L"ntdll.dll");
            auto set = reinterpret_cast<NtSet>(GetProcAddress(module, "NtSetSystemInformation"));
            auto convert = reinterpret_cast<NtError>(GetProcAddress(module, "RtlNtStatusToDosError"));
            if (!set) {
                item.kind = ResultKind::Failed;
                item.detail = L"当前 Windows 未提供所需接口";
            } else {
                auto command = options.standby == Standby::LowPriority
                                   ? MemoryListCommand::PurgeLowPriorityStandbyList
                                   : MemoryListCommand::PurgeStandbyList;
                LONG status = set(kSystemMemoryListInformation, &command, sizeof(command));
                item.kind = status >= 0 ? ResultKind::Success : ResultKind::Failed;
                if (status >= 0)
                    item.detail = L"系统已完成请求";
                else {
                    std::wostringstream error;
                    error << L"NTSTATUS 0x" << std::hex << std::uppercase << static_cast<ULONG>(status);
                    if (convert)
                        error << L"；" << WinError(convert(status));
                    item.detail = error.str();
                }
            }
        }
        Record(result, std::move(item), report);
    }
    if (options.fileCache && !cancel.load()) {
        ItemResult item;
        item.name = L"系统文件缓存";
        Privilege privilege(SE_INCREASE_QUOTA_NAME);
        if (!privilege.valid()) {
            item.kind = ResultKind::Failed;
            item.detail = L"权限启用失败：" + WinError(privilege.error());
        } else if (SetSystemFileCacheSize(std::numeric_limits<SIZE_T>::max(),
                                          std::numeric_limits<SIZE_T>::max(), 0)) {
            item.kind = ResultKind::Success;
            item.detail = L"系统已完成请求";
        } else {
            item.kind = ResultKind::Failed;
            item.detail = WinError(GetLastError());
        }
        Record(result, std::move(item), report);
    }
    result.cancelled = cancel.load();
    result.after = ReadMemory();
    return result;
}
} // namespace mc
