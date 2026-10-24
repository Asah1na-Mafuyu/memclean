#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc {
class Handle {
    HANDLE value_ = nullptr;

public:
    explicit Handle(HANDLE h = nullptr) : value_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value_(h.release()) {}
    Handle& operator=(Handle&& h) noexcept {
        if (this != &h)
            reset(h.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        HANDLE h = value_;
        value_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) {
        if (*this)
            CloseHandle(value_);
        value_ = h;
    }
};

struct Identity {
    DWORD pid = 0;
    uint64_t created = 0;
};
bool operator==(const Identity& a, const Identity& b);
Identity Identify(HANDLE process, DWORD pid);
Identity Identify(DWORD pid);
std::wstring ImagePath(HANDLE process);
std::wstring ExecutablePath();
std::wstring WinError(DWORD code);
std::wstring Bytes(uint64_t bytes);
std::wstring AvailableChange(uint64_t before, uint64_t after);
bool IsMinecraftOrJava(const std::wstring& name, const std::wstring& path);
bool IsAdministrator();

struct Memory {
    uint64_t total = 0, available = 0;
    DWORD load = 0;
    bool valid = false;
};
Memory ReadMemory();

struct Process {
    Identity id;
    DWORD parent = 0;
    std::wstring name, path, reason;
    uint64_t workingSet = 0;
    bool verified = false, sameUser = false, sameSession = false;
    bool elevated = true, critical = true, desktopTree = false, eligible = false;
};
struct Snapshot {
    std::vector<Process> processes;
    bool valid = false;
    std::wstring error;
};
// The foreground identity must be the last application outside memclean.
Snapshot Inspect(Identity foreground);
void ApplyPolicy(std::vector<Process>& processes, Identity foreground, DWORD ownPid,
                 const std::wstring& ownPath, const std::wstring& windowsPath);

enum class ResultKind { Success, Skipped, Failed };
struct ItemResult {
    ResultKind kind = ResultKind::Skipped;
    DWORD pid = 0;
    std::wstring name, detail;
};
struct RunResult {
    Memory before, after;
    unsigned succeeded = 0, skipped = 0, failed = 0;
    bool cancelled = false;
    std::wstring error;
    std::vector<ItemResult> items;
};
using ForegroundReader = std::function<Identity()>;
using Reporter = std::function<void(const std::wstring&)>;
RunResult CleanSelected(const std::vector<Identity>& targets, const ForegroundReader& foreground,
                        const std::atomic_bool& cancel, const Reporter& report);

// Standby operations use the native API. Only these two command values are allowed.
enum class Standby { None, LowPriority, All };
struct GlobalOptions {
    Standby standby = Standby::None;
    bool fileCache = false;
};
bool ValidGlobalOptions(GlobalOptions options);
RunResult CleanGlobal(GlobalOptions options, const std::atomic_bool& cancel, const Reporter& report);

// Shared by ordinary and elevated windows. Owns the mutex on the worker thread.
class OperationGuard {
    Handle mutex_;
    bool owned_ = false;

public:
    OperationGuard();
    ~OperationGuard();
    bool acquired() const { return owned_; }
};
} // namespace mc
