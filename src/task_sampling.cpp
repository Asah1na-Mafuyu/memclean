#include "task_manager.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace mc::task {
namespace {
uint64_t Ticks(FILETIME time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
using Key = std::pair<DWORD, uint64_t>;
Key ProcessKey(Identity id) {
    return {id.pid, id.created};
}
struct WindowInfo {
    HWND window = nullptr;
    std::wstring title;
};
BOOL CALLBACK ReadWindows(HWND hwnd, LPARAM data) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    auto& windows = *reinterpret_cast<std::map<DWORD, WindowInfo>*>(data);
    if (title[0] && !windows.count(pid))
        windows[pid] = {hwnd, title};
    return TRUE;
}
double PdhValue(PDH_HCOUNTER counter) {
    if (!counter)
        return -1;
    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value) !=
            ERROR_SUCCESS ||
        (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
        !std::isfinite(value.doubleValue))
        return -1;
    return std::max(0.0, value.doubleValue);
}
std::wstring HardwareName() {
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return L"处理器";
    return value;
}
std::wstring GraphicsName() {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    std::wstring result;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            !(device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
            std::wstring name = device.DeviceString;
            if (result.find(name) == std::wstring::npos) {
                if (!result.empty())
                    result += L" / ";
                result += name;
            }
        }
        device = {};
        device.cb = sizeof(device);
    }
    return result.empty() ? L"未检测到显示适配器" : result;
}
} // namespace

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}
std::wstring Percent(double value) {
    if (value < 0 || !std::isfinite(value))
        return L"—";
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << value << L"%";
    return text.str();
}
std::wstring Rate(double value) {
    if (value < 0 || !std::isfinite(value))
        return L"—";
    if (value < 1024 * 1024) {
        std::wostringstream text;
        text << std::fixed << std::setprecision(1) << value / 1024 << L" KB/s";
        return text.str();
    }
    return Bytes(static_cast<uint64_t>(value)) + L"/s";
}
std::wstring Duration(uint64_t seconds) {
    std::wostringstream text;
    if (seconds >= 86400)
        text << seconds / 86400 << L" 天 ";
    text << std::setfill(L'0') << std::setw(2) << seconds / 3600 % 24 << L":" << std::setw(2)
         << seconds / 60 % 60 << L":" << std::setw(2) << seconds % 60;
    return text.str();
}
std::wstring PriorityName(DWORD value) {
    switch (value) {
    case IDLE_PRIORITY_CLASS:
        return L"低";
    case BELOW_NORMAL_PRIORITY_CLASS:
        return L"低于正常";
    case NORMAL_PRIORITY_CLASS:
        return L"正常";
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return L"高于正常";
    case HIGH_PRIORITY_CLASS:
        return L"高";
    case REALTIME_PRIORITY_CLASS:
        return L"实时";
    default:
        return L"—";
    }
}
std::wstring CsvCell(const std::wstring& value) {
    std::wstring safe = value;
    auto first = safe.find_first_not_of(L" \t\r\n");
    if (first != std::wstring::npos && std::wstring(L"=+-@").find(safe[first]) != std::wstring::npos)
        safe.insert(0, L"'");
    std::wstring result = L"\"";
    for (auto c : safe) {
        if (c == L'"')
            result += L'"';
        result += c;
    }
    return result + L"\"";
}
std::wstring ProcessUser(HANDLE process, std::wstring* sidText) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return L"—";
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size)
        return L"—";
    std::vector<BYTE> data(size);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), size, &size))
        return L"—";
    auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    if (sidText) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(sid, &text)) {
            *sidText = text;
            LocalFree(text);
        }
    }
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    if (!nameSize)
        return L"—";
    std::vector<wchar_t> name(nameSize), domain(domainSize + 1);
    if (!LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use))
        return L"—";
    return domain[0] ? std::wstring(domain.data()) + L"\\" + name.data() : name.data();
}
double CounterRate(uint64_t before, uint64_t after, double seconds) {
    return seconds > 0 && after >= before ? static_cast<double>(after - before) / seconds : -1;
}
double CpuPercent(uint64_t before, uint64_t after, double seconds, DWORD processors) {
    double rate = CounterRate(before, after, seconds);
    return rate >= 0 && processors ? std::clamp(rate / (100000.0 * processors), 0.0, 100.0) : -1;
}
void ObserveHistory(HistoryEntry& history, const ProcessInfo& process) {
    history.id = process.id;
    history.name = process.name;
    history.running = true;
    history.cpuKnown = process.timeKnown;
    history.ioKnown = process.ioKnown;
    if (process.timeKnown) {
        if (!history.cpuBaseline) {
            history.firstCpu = process.cpuTicks;
            history.cpuBaseline = true;
        }
        history.cpuKnown = process.cpuTicks >= history.firstCpu;
        if (history.cpuKnown)
            history.cpu = process.cpuTicks - history.firstCpu;
    }
    if (process.ioKnown) {
        if (!history.ioBaseline) {
            history.firstRead = process.readBytes;
            history.firstWrite = process.writeBytes;
            history.ioBaseline = true;
        }
        history.ioKnown = process.readBytes >= history.firstRead && process.writeBytes >= history.firstWrite;
        if (history.ioKnown) {
            history.read = process.readBytes - history.firstRead;
            history.write = process.writeBytes - history.firstWrite;
        }
    }
}
bool IsProtectedProcess(const ProcessInfo& process, DWORD ownPid, const std::wstring& ownPath) {
    if (process.id.pid <= 4 || process.id.pid == ownPid || process.critical || !process.queryable ||
        !process.id.created)
        return true;
    if (!ownPath.empty() && Lower(process.path) == Lower(ownPath))
        return true;
    if (process.sid.empty() || process.sid == L"S-1-5-18" || process.sid == L"S-1-5-19" ||
        process.sid == L"S-1-5-20")
        return true;
    static const std::set<std::wstring> protectedNames{L"smss.exe",     L"csrss.exe", L"wininit.exe",
                                                       L"winlogon.exe", L"lsass.exe", L"services.exe",
                                                       L"svchost.exe",  L"dwm.exe",   L"fontdrvhost.exe"};
    return protectedNames.count(Lower(process.name)) != 0;
}
std::vector<Identity> Descendants(const std::vector<ProcessInfo>& processes, Identity root) {
    std::vector<Identity> result;
    auto found =
        std::find_if(processes.begin(), processes.end(), [&](const ProcessInfo& p) { return p.id == root; });
    if (found == processes.end())
        return result;
    result.push_back(root);
    for (size_t i = 0; i < result.size(); ++i) {
        for (const auto& p : processes) {
            if (p.parent == result[i].pid && p.id.created && p.id.created >= result[i].created &&
                std::none_of(result.begin(), result.end(), [&](Identity id) { return id == p.id; }))
                result.push_back(p.id);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

struct Sampler::Impl {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER cpu = nullptr, diskRead = nullptr, diskWrite = nullptr, gpu = nullptr;
    ULONGLONG previousTick = 0;
    std::map<Key, ProcessInfo> previous;
    std::map<Key, HistoryEntry> history;
    std::map<ULONG64, std::pair<uint64_t, uint64_t>> previousNetwork;
    std::wstring processorName = HardwareName(), graphicsName = GraphicsName();
    Impl() {
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &cpu);
            PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &diskRead);
            PdhAddEnglishCounterW(query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &diskWrite);
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu);
        }
    }
    ~Impl() {
        if (query)
            PdhCloseQuery(query);
    }
};
Sampler::Sampler() : impl_(std::make_unique<Impl>()) {}
Sampler::~Sampler() = default;

Snapshot Sampler::Sample(bool includeServices, bool includeStartup, bool includeUsers) {
    Snapshot result;
    auto& state = *impl_;
    auto now = GetTickCount64();
    double seconds = state.previousTick ? static_cast<double>(now - state.previousTick) / 1000 : 0;
    auto& perf = result.performance;
    perf.memory = ReadMemory();
    perf.uptime = now / 1000;
    perf.processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    perf.processorName = state.processorName;
    perf.graphicsName = state.graphicsName;
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof(info);
    if (GetPerformanceInfo(&info, sizeof(info))) {
        perf.commit = info.CommitTotal * info.PageSize;
        perf.commitLimit = info.CommitLimit * info.PageSize;
        perf.cache = info.SystemCache * info.PageSize;
        perf.processes = info.ProcessCount;
        perf.threads = info.ThreadCount;
        perf.handles = info.HandleCount;
    }
    std::map<DWORD, double> processGpu;
    if (state.query && PdhCollectQueryData(state.query) == ERROR_SUCCESS && seconds > 0) {
        perf.cpu = PdhValue(state.cpu);
        if (perf.cpu >= 0)
            perf.cpu = std::min(perf.cpu, 100.0);
        perf.diskRead = PdhValue(state.diskRead);
        perf.diskWrite = PdhValue(state.diskWrite);
        DWORD size = 0, count = 0;
        if (state.gpu &&
            PdhGetFormattedCounterArrayW(state.gpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         nullptr) == PDH_MORE_DATA &&
            size < 16 * 1024 * 1024) {
            std::vector<BYTE> data(size);
            auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(data.data());
            if (PdhGetFormattedCounterArrayW(state.gpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                             items) == ERROR_SUCCESS) {
                std::map<std::wstring, double> engines;
                for (DWORD i = 0; i < count; ++i) {
                    const auto& value = items[i].FmtValue;
                    if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
                        !std::isfinite(value.doubleValue) || value.doubleValue < 0)
                        continue;
                    std::wstring name = items[i].szName;
                    size_t luid = name.find(L"_luid_");
                    if (luid != std::wstring::npos)
                        engines[name.substr(luid)] += value.doubleValue;
                    if (name.rfind(L"pid_", 0) == 0) {
                        wchar_t* end = nullptr;
                        auto pid = wcstoul(name.c_str() + 4, &end, 10);
                        if (end && *end == L'_')
                            processGpu[pid] = std::max(processGpu[pid], std::min(100.0, value.doubleValue));
                    }
                }
                for (const auto& engine : engines)
                    perf.gpu = std::max(perf.gpu, std::min(100.0, engine.second));
            }
        }
    }
    PMIB_IF_TABLE2 adapters = nullptr;
    std::map<ULONG64, std::pair<uint64_t, uint64_t>> network;
    if (GetIfTable2(&adapters) == NO_ERROR) {
        double rx = 0, tx = 0;
        bool comparable = false;
        for (ULONG i = 0; i < adapters->NumEntries; ++i) {
            const auto& row = adapters->Table[i];
            if (row.OperStatus != IfOperStatusUp || !row.InterfaceAndOperStatusFlags.HardwareInterface ||
                row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            network[row.InterfaceLuid.Value] = {row.InOctets, row.OutOctets};
            auto old = state.previousNetwork.find(row.InterfaceLuid.Value);
            if (old != state.previousNetwork.end() && seconds > 0) {
                double r = CounterRate(old->second.first, row.InOctets, seconds),
                       t = CounterRate(old->second.second, row.OutOctets, seconds);
                if (r >= 0 && t >= 0) {
                    comparable = true;
                    rx += r;
                    tx += t;
                }
            }
        }
        if (comparable) {
            perf.networkRead = rx;
            perf.networkWrite = tx;
        }
        FreeMibTable(adapters);
    }
    state.previousNetwork = std::move(network);
    std::map<DWORD, WindowInfo> windows;
    EnumWindows(ReadWindows, reinterpret_cast<LPARAM>(&windows));
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!snapshot || !Process32FirstW(snapshot.get(), &entry)) {
        result.error = WinError(GetLastError());
        return result;
    }
    std::map<Key, ProcessInfo> current;
    const auto ownPath = ExecutablePath();
    for (auto& item : state.history)
        item.second.running = false;
    do {
        ProcessInfo p;
        p.id.pid = entry.th32ProcessID;
        p.parent = entry.th32ParentProcessID;
        p.name = entry.szExeFile;
        p.threads = entry.cntThreads;
        ProcessIdToSessionId(p.id.pid, &p.session);
        auto window = windows.find(p.id.pid);
        if (window != windows.end()) {
            p.window = window->second.window;
            p.title = window->second.title;
        }
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.id.pid));
        if (process) {
            auto id = Identify(process.get(), p.id.pid);
            if (id.created)
                p.id = id;
            auto old = state.previous.find(ProcessKey(p.id));
            if (old != state.previous.end()) {
                p.path = old->second.path;
                p.user = old->second.user;
                p.sid = old->second.sid;
                p.elevated = old->second.elevated;
            } else {
                p.path = ImagePath(process.get());
                p.user = ProcessUser(process.get(), &p.sid);
                HANDLE raw = nullptr;
                if (OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) {
                    Handle token(raw);
                    TOKEN_ELEVATION elevation{};
                    DWORD bytes = 0;
                    if (GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation),
                                            &bytes))
                        p.elevated = elevation.TokenIsElevated != 0;
                }
            }
            BOOL critical = TRUE;
            p.queryable = p.id.created && IsProcessCritical(process.get(), &critical);
            p.critical = critical != FALSE;
            p.priority = GetPriorityClass(process.get());
            GetProcessHandleCount(process.get(), &p.handles);
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory))) {
                p.memoryKnown = true;
                p.workingSet = memory.WorkingSetSize;
                p.privateBytes = memory.PrivateUsage;
                p.peakWorkingSet = memory.PeakWorkingSetSize;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            IO_COUNTERS io{};
            if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                p.timeKnown = true;
                p.cpuTicks = Ticks(kernel) + Ticks(user);
            }
            if (GetProcessIoCounters(process.get(), &io)) {
                p.ioKnown = true;
                p.readBytes = io.ReadTransferCount;
                p.writeBytes = io.WriteTransferCount;
            }
            if (old != state.previous.end()) {
                if (p.timeKnown && old->second.timeKnown)
                    p.cpu = CpuPercent(old->second.cpuTicks, p.cpuTicks, seconds, perf.processors);
                if (p.ioKnown && old->second.ioKnown) {
                    p.readRate = CounterRate(old->second.readBytes, p.readBytes, seconds);
                    p.writeRate = CounterRate(old->second.writeBytes, p.writeBytes, seconds);
                }
            }
            if (processGpu.count(p.id.pid))
                p.gpu = processGpu[p.id.pid];
            if (p.id.created) {
                auto key = ProcessKey(p.id);
                ObserveHistory(state.history[key], p);
                current[key] = p;
            }
        }
        if (IsProtectedProcess(p, GetCurrentProcessId(), ownPath))
            p.protection = L"系统 / 受保护 / 身份未知";
        else if (p.elevated)
            p.protection = L"管理员进程";
        else
            p.protection = L"可管理";
        result.processes.push_back(std::move(p));
    } while (Process32NextW(snapshot.get(), &entry));
    state.previous = std::move(current);
    state.previousTick = now;
    while (state.history.size() > 5000) {
        auto stale = std::find_if(state.history.begin(), state.history.end(),
                                  [](const auto& pair) { return !pair.second.running; });
        if (stale == state.history.end())
            stale = std::min_element(
                state.history.begin(), state.history.end(),
                [](const auto& a, const auto& b) { return a.second.id.created < b.second.id.created; });
        state.history.erase(stale);
    }
    for (const auto& h : state.history)
        result.history.push_back(h.second);
    if (includeServices)
        result.services = ReadServices(result.servicesError);
    if (includeStartup)
        result.startups = ReadStartup(result.startupsError);
    if (includeUsers) {
        result.sessions = ReadSessions(result.sessionsError);
        for (auto& session : result.sessions)
            for (const auto& p : result.processes)
                if (p.session == session.id) {
                    ++session.processes;
                    session.memory += p.workingSet;
                    if (p.cpu >= 0)
                        session.cpu = std::max(0.0, session.cpu) + p.cpu;
                }
    }
    return result;
}
} // namespace mc::task
