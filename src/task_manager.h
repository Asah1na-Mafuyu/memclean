#pragma once
#include "core.h"
#include <memory>
#include <utility>

namespace mc::task {
struct ProcessInfo {
    Identity id;
    DWORD parent = 0, session = 0, threads = 0, handles = 0, priority = 0;
    uint64_t workingSet = 0, privateBytes = 0, peakWorkingSet = 0;
    uint64_t cpuTicks = 0, readBytes = 0, writeBytes = 0;
    double cpu = -1, readRate = -1, writeRate = -1, gpu = -1;
    std::wstring name, path, user, sid, title, protection;
    HWND window = nullptr;
    bool queryable = false, elevated = false, critical = true;
    bool memoryKnown = false, timeKnown = false, ioKnown = false;
};
struct Performance {
    Memory memory;
    uint64_t commit = 0, commitLimit = 0, cache = 0, uptime = 0;
    DWORD processors = 0, processes = 0, threads = 0, handles = 0;
    double cpu = -1, diskRead = -1, diskWrite = -1, networkRead = -1, networkWrite = -1, gpu = -1;
    std::wstring processorName, graphicsName;
};
struct HistoryEntry {
    Identity id;
    std::wstring name;
    uint64_t firstCpu = 0, firstRead = 0, firstWrite = 0;
    uint64_t cpu = 0, read = 0, write = 0;
    bool running = false;
    bool cpuBaseline = false, ioBaseline = false, cpuKnown = false, ioKnown = false;
};
struct ServiceInfo {
    std::wstring name, display, path, account;
    DWORD state = 0, pid = 0, startType = SERVICE_NO_CHANGE, accepted = 0;
};
struct SessionInfo {
    DWORD id = 0, state = 0, processes = 0;
    uint64_t logon = 0, memory = 0;
    double cpu = -1;
    std::wstring name, station;
};
enum class StartupSource { UserRun, MachineRun, MachineRun32, UserFolder, CommonFolder };
struct StartupEntry {
    StartupSource source = StartupSource::UserRun;
    std::wstring name, command, location;
    DWORD type = 0;
    std::vector<BYTE> data;
    bool disabled = false;
};
struct Snapshot {
    Performance performance;
    std::vector<ProcessInfo> processes;
    std::vector<HistoryEntry> history;
    std::vector<ServiceInfo> services;
    std::vector<SessionInfo> sessions;
    std::vector<StartupEntry> startups;
    std::wstring error, servicesError, sessionsError, startupsError;
};
struct Result {
    bool success = false;
    std::wstring message;
};

std::wstring Lower(std::wstring value);
std::wstring Percent(double value);
std::wstring Rate(double value);
std::wstring Duration(uint64_t seconds);
std::wstring PriorityName(DWORD value);
std::wstring ServiceState(DWORD value);
std::wstring ServiceStartName(DWORD value);
std::wstring SessionState(DWORD value);
std::wstring StartupSourceName(StartupSource value);
std::wstring CsvCell(const std::wstring& value);
std::wstring ProcessUser(HANDLE process, std::wstring* sid = nullptr);
double CounterRate(uint64_t before, uint64_t after, double seconds);
double CpuPercent(uint64_t before, uint64_t after, double seconds, DWORD processors);
void ObserveHistory(HistoryEntry& history, const ProcessInfo& process);
bool IsProtectedProcess(const ProcessInfo& process, DWORD ownPid, const std::wstring& ownPath);
std::vector<Identity> Descendants(const std::vector<ProcessInfo>& processes, Identity root);

class Sampler {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Sampler();
    ~Sampler();
    Snapshot Sample(bool includeServices, bool includeStartup, bool includeUsers);
};

Result EndProcess(Identity target, bool tree, const std::atomic_bool& cancel);
Result ChangePriority(Identity target, DWORD priority);
Result ChangeAffinity(Identity target, DWORD_PTR mask);
Result QueryAffinity(Identity target, DWORD_PTR& current, DWORD_PTR& allowed);
Result SetEcoQos(Identity target, bool enabled);
Result CloseProcessWindow(Identity target);
Result ActivateProcessWindow(Identity target);
std::wstring ProcessDetails(Identity target);
std::vector<ServiceInfo> ReadServices(std::wstring& error);
enum class ServiceAction { Start, Stop, Restart, Pause, Continue, Automatic, Manual, Disabled };
Result ChangeService(const ServiceInfo& service, ServiceAction action, const std::atomic_bool& cancel);
std::vector<SessionInfo> ReadSessions(std::wstring& error);
Result ChangeSession(const SessionInfo& session, bool logoff);
std::vector<StartupEntry> ReadStartup(std::wstring& error);
Result ChangeStartup(const StartupEntry& entry);

// Shared implementation used with an isolated registry key in tests; no test-mode switch in the UI.
Result MoveRegistryValue(HKEY hive, REGSAM view, const std::wstring& from, const std::wstring& to,
                         const std::wstring& name, DWORD type, const std::vector<BYTE>& expected);
int RunWindow(HINSTANCE instance);
} // namespace mc::task
