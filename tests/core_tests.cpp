#include "core.h"
#include <psapi.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
int passed = 0;
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
    ++passed;
    std::cout << "PASS " << message << '\n';
}
mc::Process Example(DWORD pid, DWORD parent, const wchar_t* name, const wchar_t* path,
                    uint64_t created = 100) {
    mc::Process p;
    p.id = {pid, created};
    p.parent = parent;
    p.name = name;
    p.path = path;
    p.verified = p.sameUser = p.sameSession = p.desktopTree = true;
    p.elevated = p.critical = false;
    p.workingSet = 128ull << 20;
    return p;
}
void PolicyTests() {
    Check(mc::IsMinecraftOrJava(L"JAVA.exe", L"D:\\jdk\\bin\\java.exe"), "Java protected case-insensitively");
    Check(mc::IsMinecraftOrJava(L"Minecraft.Windows.exe", L"C:\\Apps\\game.exe"), "Bedrock protected");
    Check(mc::IsMinecraftOrJava(L"PCL2.exe", L"D:\\MC\\PCL2.exe"), "PCL launcher protected");
    Check(mc::IsMinecraftOrJava(L"custom.exe", L"D:\\.minecraft\\runtime\\custom.exe"),
          "custom executable under .minecraft protected");
    Check(!mc::IsMinecraftOrJava(L"editor.exe", L"D:\\Apps\\editor.exe"), "ordinary app not misclassified");
    std::vector<mc::Process> processes{Example(10, 0, L"editor.exe", L"D:\\Apps\\editor.exe"),
                                       Example(11, 10, L"renderer.exe", L"D:\\Apps\\renderer.exe", 101),
                                       Example(12, 0, L"editor.exe", L"D:\\Apps\\editor.exe"),
                                       Example(20, 0, L"javaw.exe", L"D:\\Java\\javaw.exe"),
                                       Example(21, 20, L"helper.exe", L"D:\\Java\\helper.exe", 101),
                                       Example(30, 0, L"background.exe", L"D:\\Apps\\background.exe"),
                                       Example(40, 0, L"service.exe", L"C:\\Windows\\System32\\service.exe"),
                                       Example(50, 0, L"admin.exe", L"D:\\Apps\\admin.exe"),
                                       Example(60, 0, L"other.exe", L"D:\\Apps\\other.exe"),
                                       Example(70, 0, L"critical.exe", L"D:\\Apps\\critical.exe"),
                                       Example(80, 0, L"headless.exe", L"D:\\Apps\\headless.exe"),
                                       Example(90, 0, L"memclean.exe", L"D:\\memclean\\memclean.exe"),
                                       Example(100, 0, L"small.exe", L"D:\\Apps\\small.exe")};
    processes[7].elevated = true;
    processes[8].sameUser = false;
    processes[9].critical = true;
    processes[10].desktopTree = false;
    processes[12].workingSet = 1024;
    mc::ApplyPolicy(processes, {10, 100}, 90, L"D:\\memclean\\memclean.exe", L"C:\\Windows");
    Check(!processes[0].eligible && !processes[1].eligible && !processes[2].eligible,
          "foreground, child and same-image siblings protected");
    Check(!processes[3].eligible && !processes[4].eligible, "Java and descendants protected");
    Check(processes[5].eligible, "background desktop application eligible");
    for (size_t i = 6; i < processes.size(); ++i)
        Check(!processes[i].eligible, "system/admin/other-user/critical/headless/self/small excluded");
    mc::ApplyPolicy(processes, {10, 999}, 90, L"D:\\memclean\\memclean.exe", L"C:\\Windows");
    Check(std::none_of(processes.begin(), processes.end(), [](const mc::Process& p) { return p.eligible; }),
          "stale foreground identity fails closed");
    mc::ApplyPolicy(processes, {}, 90, L"D:\\memclean\\memclean.exe", L"C:\\Windows");
    Check(std::none_of(processes.begin(), processes.end(), [](const mc::Process& p) { return p.eligible; }),
          "unknown foreground fails closed");
    Check(mc::AvailableChange(200, 100).find(L"减少") == 0,
          "decreased available memory has negative direction");
    Check(mc::AvailableChange(0, std::numeric_limits<uint64_t>::max()).find(L"增加") == 0,
          "maximum unsigned delta does not overflow");
    Check(!mc::ValidGlobalOptions({}), "global defaults have no operation");
    Check(!mc::ValidGlobalOptions({static_cast<mc::Standby>(99), true}), "invalid native command rejected");
    Check(mc::ValidGlobalOptions({mc::Standby::LowPriority, false}), "low-priority choice accepted");
    Check(mc::ValidGlobalOptions({mc::Standby::None, true}), "file cache choice accepted");
    auto memory = mc::ReadMemory();
    Check(memory.valid && memory.total >= memory.available, "live memory counters valid");
    bool secondAcquired = true;
    {
        mc::OperationGuard first;
        Check(first.acquired(), "operation lock available");
        std::thread thread([&] {
            mc::OperationGuard second;
            secondAcquired = second.acquired();
        });
        thread.join();
        Check(!secondAcquired, "concurrent worker rejected by operation lock");
    }
    mc::OperationGuard released;
    Check(released.acquired(), "operation lock released after scope");
}

struct Fixture {
    mc::Handle process, ready, stop;
    DWORD pid = 0;
    explicit Fixture(const wchar_t* path) {
        std::wstring suffix =
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        std::wstring readyName = L"Local\\memclean.Test.Ready." + suffix,
                     stopName = L"Local\\memclean.Test.Stop." + suffix;
        ready.reset(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
        stop.reset(CreateEventW(nullptr, TRUE, FALSE, stopName.c_str()));
        if (!ready || !stop)
            throw std::runtime_error("fixture event creation failed");
        std::wstring command =
            L"\"" + std::wstring(path) + L"\" \"" + readyName + L"\" \"" + stopName + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &info))
            throw std::runtime_error("fixture creation failed");
        process.reset(info.hProcess);
        CloseHandle(info.hThread);
        pid = info.dwProcessId;
        if (WaitForSingleObject(ready.get(), 10000) != WAIT_OBJECT_0) {
            SetEvent(stop.get());
            WaitForSingleObject(process.get(), 5000);
            throw std::runtime_error("fixture not ready");
        }
    }
    ~Fixture() {
        SetEvent(stop.get());
        WaitForSingleObject(process.get(), 10000);
    }
};
void Integration(const wchar_t* path, const wchar_t* anchorPath) {
    Fixture anchor(anchorPath);
    Fixture fixture(path);
    auto foreground = mc::Identify(anchor.process.get(), anchor.pid);
    auto target = mc::Identify(fixture.process.get(), fixture.pid);
    auto snapshot = mc::Inspect(foreground);
    Check(snapshot.valid, "real process scan succeeded");
    auto item = std::find_if(snapshot.processes.begin(), snapshot.processes.end(),
                             [&](const mc::Process& p) { return p.id == target; });
    Check(item != snapshot.processes.end(), "isolated fixture identity found");
    // Tests must run unelevated, just like the default UI. Never relax production safety checks.
    Check(!mc::IsAdministrator(), "test runner has ordinary user privileges");
    Check(item->eligible, "isolated background fixture eligible under real policy");
    std::atomic_bool cancel{false};
    auto silent = [](const std::wstring&) {};
    auto readForeground = [&] { return foreground; };
    auto stale = target;
    ++stale.created;
    auto staleResult = mc::CleanSelected({stale}, readForeground, cancel, silent);
    Check(staleResult.succeeded == 0 && staleResult.skipped == 1, "recycled/stale PID target skipped");
    auto protectedResult = mc::CleanSelected({target}, [&] { return target; }, cancel, silent);
    Check(protectedResult.succeeded == 0 && protectedResult.skipped == 1, "real foreground fixture skipped");
    cancel.store(true);
    auto cancelled = mc::CleanSelected({target}, readForeground, cancel, silent);
    Check(cancelled.cancelled && cancelled.succeeded == 0,
          "cancel before execution leaves process untouched");
    cancel.store(false);
    int reads = 0;
    auto switched =
        mc::CleanSelected({target}, [&] { return ++reads == 1 ? foreground : target; }, cancel, silent);
    Check(switched.succeeded == 0 && switched.skipped == 1,
          "foreground switch between scan and trim causes skip");
    PROCESS_MEMORY_COUNTERS before{};
    before.cb = sizeof(before);
    Check(GetProcessMemoryInfo(fixture.process.get(), &before, sizeof(before)) != FALSE,
          "read fixture working set before trim");
    auto result = mc::CleanSelected({target}, readForeground, cancel, silent);
    Check(result.succeeded == 1 && result.failed == 0,
          "real EmptyWorkingSet succeeds on isolated fixture only");
    PROCESS_MEMORY_COUNTERS after{};
    after.cb = sizeof(after);
    Check(GetProcessMemoryInfo(fixture.process.get(), &after, sizeof(after)) != FALSE,
          "read fixture working set after trim");
    std::cout << "Fixture working set: " << before.WorkingSetSize << " -> " << after.WorkingSetSize
              << " bytes\n";
    Check(after.WorkingSetSize < before.WorkingSetSize, "fixture resident working set decreased");
    Check(WaitForSingleObject(fixture.process.get(), 0) == WAIT_TIMEOUT, "fixture is still alive");
    SetEvent(fixture.stop.get());
    Check(WaitForSingleObject(fixture.process.get(), 10000) == WAIT_OBJECT_0,
          "fixture exits normally after rereading allocation");
    DWORD exitCode = 1;
    GetExitCodeProcess(fixture.process.get(), &exitCode);
    Check(exitCode == 0, "all 128 MB allocation contents preserved");
    auto denied = mc::CleanGlobal({mc::Standby::LowPriority, false}, cancel, silent);
    Check(!denied.error.empty() && denied.items.empty(),
          "unelevated global cleanup rejected without native call");
}
} // namespace
int wmain(int count, wchar_t** args) {
    try {
        PolicyTests();
        if (count != 3)
            throw std::runtime_error("fixture and foreground anchor paths required");
        Integration(args[1], args[2]);
        std::cout << "ALL " << passed << " CHECKS PASSED. No real application or global cache was cleaned.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
