#include "task_manager.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace mc;
namespace task = mc::task;
int checks = 0;
void Check(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
    ++checks;
    std::cout << "PASS " << text << '\n';
}
struct Fixture {
    Handle process, ready, stop, childReady, childStop;
    DWORD pid = 0;
    Fixture(const wchar_t* path, bool tree = false) {
        std::wstring suffix =
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        std::wstring readyName = L"Local\\memclean.TaskTest.Ready." + suffix,
                     stopName = L"Local\\memclean.TaskTest.Stop." + suffix;
        std::wstring childReadyName = L"Local\\memclean.TaskTest.ChildReady." + suffix,
                     childStopName = L"Local\\memclean.TaskTest.ChildStop." + suffix;
        ready.reset(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
        stop.reset(CreateEventW(nullptr, TRUE, FALSE, stopName.c_str()));
        childReady.reset(CreateEventW(nullptr, TRUE, FALSE, childReadyName.c_str()));
        childStop.reset(CreateEventW(nullptr, TRUE, FALSE, childStopName.c_str()));
        std::wstring command =
            L"\"" + std::wstring(path) + L"\" \"" + readyName + L"\" \"" + stopName + L"\"";
        if (tree)
            command += L" \"" + childReadyName + L"\" \"" + childStopName + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &info))
            throw std::runtime_error("could not start isolated fixture");
        process.reset(info.hProcess);
        CloseHandle(info.hThread);
        pid = info.dwProcessId;
        if (WaitForSingleObject(ready.get(), 10000) != WAIT_OBJECT_0 ||
            (tree && WaitForSingleObject(childReady.get(), 10000) != WAIT_OBJECT_0)) {
            SetEvent(stop.get());
            SetEvent(childStop.get());
            throw std::runtime_error("fixture readiness timeout");
        }
    }
    ~Fixture() {
        SetEvent(stop.get());
        SetEvent(childStop.get());
        WaitForSingleObject(process.get(), 10000);
    }
    Identity id() const { return Identify(process.get(), pid); }
};
void MetricsAndPolicy() {
    Check(task::CounterRate(100, 300, 2) == 100, "counter rate uses elapsed time");
    Check(task::CounterRate(300, 100, 2) < 0, "counter reset cannot unsigned-underflow");
    Check(task::CounterRate(0, 100, 0) < 0, "first sample is unavailable, not a fake zero");
    Check(task::CpuPercent(0, 10000000, 1, 4) == 25, "process CPU normalized over logical processors");
    Check(task::CpuPercent(0, 1000000000, 1, 1) == 100, "CPU result bounded at 100 percent");
    Check(task::CpuPercent(0, 1, 1, 0) < 0, "unknown CPU count unavailable");
    task::HistoryEntry history;
    task::ProcessInfo observation;
    observation.id = {100, 100};
    task::ObserveHistory(history, observation);
    Check(!history.cpuKnown && !history.ioKnown, "missing history counters remain unavailable");
    observation.timeKnown = observation.ioKnown = true;
    observation.cpuTicks = 10000000;
    observation.readBytes = 1000;
    observation.writeBytes = 2000;
    task::ObserveHistory(history, observation);
    Check(history.cpuKnown && history.ioKnown && !history.cpu && !history.read && !history.write,
          "first readable history sample excludes work before observation");
    observation.cpuTicks += 200;
    observation.readBytes += 300;
    observation.writeBytes += 400;
    task::ObserveHistory(history, observation);
    Check(history.cpu == 200 && history.read == 300 && history.write == 400,
          "history accumulates from first readable sample");
    observation.timeKnown = observation.ioKnown = false;
    task::ObserveHistory(history, observation);
    Check(!history.cpuKnown && !history.ioKnown && history.cpu == 200 && history.read == 300,
          "temporary counter failure does not reset history or claim zero usage");
    observation.timeKnown = observation.ioKnown = true;
    observation.cpuTicks = observation.readBytes = observation.writeBytes = 0;
    task::ObserveHistory(history, observation);
    Check(!history.cpuKnown && !history.ioKnown, "history rejects decreasing counters without underflow");
    Check(task::CsvCell(L"=SUM(A1:A2)") == L"\"'=SUM(A1:A2)\"", "CSV formula neutralized");
    Check(task::CsvCell(L"  @SUM(A1)") == L"\"'  @SUM(A1)\"", "CSV leading whitespace formula neutralized");
    Check(task::CsvCell(L"a\"b,c") == L"\"a\"\"b,c\"", "CSV quotes and commas escaped");
    task::ProcessInfo p;
    p.id = {100, 100};
    p.queryable = true;
    p.critical = false;
    p.sid = L"S-1-5-21-test";
    p.path = L"C:\\Example\\app.exe";
    p.name = L"app.exe";
    Check(!task::IsProtectedProcess(p, 200, L"C:\\Example\\memclean.exe"),
          "ordinary user process manageable");
    Check(task::IsProtectedProcess(p, 100, L""), "task manager self protected");
    p.critical = true;
    Check(task::IsProtectedProcess(p, 200, L""), "critical process protected");
    p.critical = false;
    p.sid = L"S-1-5-18";
    Check(task::IsProtectedProcess(p, 200, L""), "SYSTEM account process protected");
    p.sid = L"S-1-5-21-test";
    p.name = L"WINLOGON.EXE";
    Check(task::IsProtectedProcess(p, 200, L""), "essential process names protected case-insensitively");
    std::vector<task::ProcessInfo> tree(4);
    tree[0].id = {10, 100};
    tree[1].id = {11, 101};
    tree[1].parent = 10;
    tree[2].id = {12, 102};
    tree[2].parent = 11;
    tree[3].id = {13, 90};
    tree[3].parent = 10;
    auto order = task::Descendants(tree, {10, 100});
    Check(order.size() == 3 && order.front().pid == 12 && order.back().pid == 10,
          "tree descendants precede parent and exclude reused parent PID");
    Check(task::Descendants(tree, {10, 99}).empty(), "stale tree root rejected");
}
void LiveReadOnly() {
    task::Sampler sampler;
    auto first = sampler.Sample(true, true, true);
    Check(first.error.empty() && !first.processes.empty(), "process snapshot available");
    Check(first.performance.memory.valid && first.performance.processors > 0,
          "live system memory and processor count");
    auto self = std::find_if(first.processes.begin(), first.processes.end(),
                             [](const auto& p) { return p.id.pid == GetCurrentProcessId(); });
    Check(self != first.processes.end() && self->queryable && self->memoryKnown,
          "self metadata and memory readable");
    Check(!first.services.empty(), "services enumerate read-only");
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Check(std::any_of(first.sessions.begin(), first.sessions.end(),
                      [&](const auto& s) { return s.id == session; }),
          "current user session enumerated");
    auto details = task::ProcessDetails(Identify(GetCurrentProcessId()));
    Check(details.find(L"memclean_task_tests") != std::wstring::npos,
          "process details include executable or command line");
    Sleep(1100);
    auto second = sampler.Sample(false, false, false);
    auto self2 = std::find_if(second.processes.begin(), second.processes.end(),
                              [](const auto& p) { return p.id.pid == GetCurrentProcessId(); });
    Check(self2 != second.processes.end() && self2->cpu >= 0 && self2->cpu <= 100,
          "second sample has valid per-process CPU");
    Check(second.performance.cpu >= 0 && second.performance.cpu <= 100, "live PDH system CPU available");
    Check(!second.history.empty(), "in-window process history populated");
    std::cout << "Counters: CPU=" << second.performance.cpu << " disk_read=" << second.performance.diskRead
              << " network_read=" << second.performance.networkRead << " GPU=" << second.performance.gpu
              << '\n';
    std::atomic_bool cancel{false};
    Check(!task::EndProcess(Identify(GetCurrentProcessId()), false, cancel).success,
          "self termination rejected");
    Check(!task::ChangePriority(Identify(GetCurrentProcessId()), REALTIME_PRIORITY_CLASS).success,
          "real-time priority rejected");
    task::SessionInfo current;
    current.id = session;
    current.logon = 1;
    Check(!task::ChangeSession(current, true).success,
          "current session logoff rejected without calling WTS logoff");
    task::StartupEntry folder;
    folder.source = task::StartupSource::UserFolder;
    Check(!task::ChangeStartup(folder).success,
          "startup directory edits rejected without filesystem mutation");
    Check(!task::ChangeService({}, task::ServiceAction::Stop, cancel).success,
          "empty service mutation rejected");
}
void ProcessActions(const wchar_t* path) {
    Check(!IsAdministrator(), "tests use ordinary user privileges");
    Fixture fixture(path);
    auto id = fixture.id();
    auto stale = id;
    ++stale.created;
    Check(task::ChangePriority(id, BELOW_NORMAL_PRIORITY_CLASS).success &&
              GetPriorityClass(fixture.process.get()) == BELOW_NORMAL_PRIORITY_CLASS,
          "isolated fixture priority changed");
    Check(!task::ChangePriority(stale, HIGH_PRIORITY_CLASS).success &&
              GetPriorityClass(fixture.process.get()) == BELOW_NORMAL_PRIORITY_CLASS,
          "stale identity cannot change priority");
    Check(task::ChangePriority(id, NORMAL_PRIORITY_CLASS).success, "isolated fixture priority restored");
    if (GetActiveProcessorGroupCount() == 1) {
        DWORD_PTR current = 0, allowed = 0;
        Check(task::QueryAffinity(id, current, allowed).success && current && allowed, "CPU affinity read");
        Check(!task::ChangeAffinity(id, 0).success, "empty affinity rejected");
        auto one = allowed & (~allowed + 1);
        Check(task::ChangeAffinity(id, one).success, "isolated fixture affinity applied");
        DWORD_PTR actual = 0, system = 0;
        GetProcessAffinityMask(fixture.process.get(), &actual, &system);
        Check(actual == one, "OS reports selected affinity");
        Check(task::ChangeAffinity(id, current).success, "isolated fixture affinity restored");
    }
    std::atomic_bool cancel{true};
    Check(!task::EndProcess(id, false, cancel).success &&
              WaitForSingleObject(fixture.process.get(), 0) == WAIT_TIMEOUT,
          "cancelled end-task leaves target alive");
    cancel.store(false);
    Check(!task::EndProcess(stale, false, cancel).success &&
              WaitForSingleObject(fixture.process.get(), 0) == WAIT_TIMEOUT,
          "stale identity cannot terminate target");
    Check(task::CloseProcessWindow(id).success, "normal-close request sent only to fixture");
    Check(WaitForSingleObject(fixture.process.get(), 10000) == WAIT_OBJECT_0, "fixture obeys normal close");
    DWORD exit = 1;
    GetExitCodeProcess(fixture.process.get(), &exit);
    Check(exit == 0, "normal close preserved fixture data");
    Fixture forced(path);
    Check(task::EndProcess(forced.id(), false, cancel).success, "isolated end-task request succeeds");
    Check(WaitForSingleObject(forced.process.get(), 5000) == WAIT_OBJECT_0, "isolated target actually exits");
    Fixture parent(path, true);
    task::Sampler sampler;
    auto snapshot = sampler.Sample(false, false, false);
    auto descendants = task::Descendants(snapshot.processes, parent.id());
    Check(descendants.size() == 2, "real fixture child relationship found");
    Handle child(OpenProcess(SYNCHRONIZE, FALSE, descendants.front().pid));
    Check(static_cast<bool>(child), "test retains isolated child handle");
    Check(task::EndProcess(parent.id(), true, cancel).success, "real isolated process tree terminated");
    Check(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0 &&
              WaitForSingleObject(parent.process.get(), 5000) == WAIT_OBJECT_0,
          "both isolated tree members actually exit");
}
std::vector<BYTE> Data(const std::wstring& text) {
    auto begin = reinterpret_cast<const BYTE*>(text.c_str());
    return {begin, begin + (text.size() + 1) * sizeof(wchar_t)};
}
void StartupRoundTrip() {
    const std::wstring base = L"Software\\memclean\\Tests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64());
    const auto from = base + L"\\Source", to = base + L"\\Backup";
    struct Cleanup {
        std::wstring path;
        ~Cleanup() {
            if (path.rfind(L"Software\\memclean\\Tests\\", 0) == 0)
                RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        }
    } cleanup{base};
    HKEY source = nullptr, backup = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, from.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &source,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("isolated registry key creation failed");
    const auto original = Data(L"%TEMP%\\nonexecuted-test.exe --example");
    RegSetValueExW(source, L"fixture", 0, REG_EXPAND_SZ, original.data(),
                   static_cast<DWORD>(original.size()));
    auto move = task::MoveRegistryValue(HKEY_CURRENT_USER, KEY_WOW64_64KEY, from, to, L"fixture",
                                        REG_EXPAND_SZ, original);
    Check(move.success, "isolated registry value backed up and removed");
    DWORD type = 0, size = 0;
    Check(RegQueryValueExW(source, L"fixture", nullptr, &type, nullptr, &size) == ERROR_FILE_NOT_FOUND,
          "disabled source value absent");
    auto restore = task::MoveRegistryValue(HKEY_CURRENT_USER, KEY_WOW64_64KEY, to, from, L"fixture",
                                           REG_EXPAND_SZ, original);
    Check(restore.success, "isolated registry value restored");
    std::vector<BYTE> read(original.size());
    size = static_cast<DWORD>(read.size());
    Check(RegQueryValueExW(source, L"fixture", nullptr, &type, read.data(), &size) == ERROR_SUCCESS &&
              type == REG_EXPAND_SZ && read == original,
          "registry type and bytes preserved exactly");
    Check(!task::MoveRegistryValue(HKEY_CURRENT_USER, KEY_WOW64_64KEY, from, to, L"fixture", REG_SZ,
                                   Data(L"stale"))
               .success,
          "stale startup snapshot rejected");
    RegCreateKeyExW(HKEY_CURRENT_USER, to.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &backup, nullptr);
    auto conflict = Data(L"different value");
    RegSetValueExW(backup, L"fixture", 0, REG_SZ, conflict.data(), static_cast<DWORD>(conflict.size()));
    Check(!task::MoveRegistryValue(HKEY_CURRENT_USER, KEY_WOW64_64KEY, from, to, L"fixture", REG_EXPAND_SZ,
                                   original)
               .success,
          "different existing destination never overwritten");
    size = static_cast<DWORD>(read.size());
    Check(RegQueryValueExW(source, L"fixture", nullptr, &type, read.data(), &size) == ERROR_SUCCESS &&
              read == original,
          "original survives destination conflict");
    RegCloseKey(backup);
    RegCloseKey(source);
}
} // namespace
int wmain(int count, wchar_t** args) {
    try {
        if (count != 2)
            throw std::runtime_error("fixture path required");
        MetricsAndPolicy();
        LiveReadOnly();
        ProcessActions(args[1]);
        StartupRoundTrip();
        std::cout << "ALL " << checks
                  << " TASK MANAGER CHECKS PASSED. Only isolated fixtures and a temporary test registry key "
                     "were modified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
