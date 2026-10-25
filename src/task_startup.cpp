#include "task_manager.h"
#include <shlobj.h>
#include <algorithm>

namespace mc::task {
namespace {
constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kBackup[] = L"Software\\memclean\\DisabledStartup\\Run";
class Key {
    HKEY value_ = nullptr;

public:
    ~Key() {
        if (value_)
            RegCloseKey(value_);
    }
    HKEY* put() { return &value_; }
    HKEY get() const { return value_; }
};
bool RegistrySource(StartupSource source) {
    return source == StartupSource::UserRun || source == StartupSource::MachineRun ||
           source == StartupSource::MachineRun32;
}
HKEY Hive(StartupSource source) {
    return source == StartupSource::UserRun ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}
REGSAM View(StartupSource source) {
    return source == StartupSource::MachineRun32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
}
LSTATUS ReadValue(HKEY key, const std::wstring& name, DWORD& type, std::vector<BYTE>& data) {
    DWORD size = 0;
    LSTATUS status = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS)
        return status;
    if (size > 1024 * 1024)
        return ERROR_FILE_TOO_LARGE;
    data.resize(size);
    status = RegQueryValueExW(key, name.c_str(), nullptr, &type, data.data(), &size);
    if (status == ERROR_SUCCESS)
        data.resize(size);
    return status;
}
void RegistryEntries(StartupSource source, bool disabled, std::vector<StartupEntry>& result,
                     std::wstring& error) {
    Key key;
    auto status =
        RegOpenKeyExW(Hive(source), disabled ? kBackup : kRun, 0, KEY_QUERY_VALUE | View(source), key.put());
    if (status == ERROR_FILE_NOT_FOUND)
        return;
    if (status != ERROR_SUCCESS) {
        error += StartupSourceName(source) + L"：" + WinError(status) + L"  ";
        return;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[16384]{};
        DWORD nameSize = 16384;
        status = RegEnumValueW(key.get(), index, name, &nameSize, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS) {
            error += WinError(status);
            break;
        }
        StartupEntry entry;
        entry.source = source;
        entry.name.assign(name, nameSize);
        entry.disabled = disabled;
        entry.location = std::wstring(Hive(source) == HKEY_CURRENT_USER ? L"HKCU\\" : L"HKLM\\") +
                         (disabled ? kBackup : kRun);
        if (ReadValue(key.get(), entry.name, entry.type, entry.data) != ERROR_SUCCESS)
            continue;
        if (entry.type != REG_SZ && entry.type != REG_EXPAND_SZ)
            continue;
        if (entry.data.size() % sizeof(wchar_t))
            continue;
        if (!entry.data.empty())
            entry.command.assign(reinterpret_cast<const wchar_t*>(entry.data.data()),
                                 entry.data.size() / sizeof(wchar_t));
        while (!entry.command.empty() && entry.command.back() == L'\0')
            entry.command.pop_back();
        result.push_back(std::move(entry));
    }
}
void FolderEntries(REFKNOWNFOLDERID id, StartupSource source, std::vector<StartupEntry>& result) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)))
        return;
    std::wstring directory = raw;
    CoTaskMemFree(raw);
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (_wcsicmp(data.cFileName, L"desktop.ini") == 0)
            continue;
        StartupEntry entry;
        entry.source = source;
        entry.name = data.cFileName;
        entry.location = directory;
        entry.command = directory + L"\\" + data.cFileName;
        result.push_back(std::move(entry));
    } while (FindNextFileW(search, &data));
    FindClose(search);
}
} // namespace
std::wstring StartupSourceName(StartupSource source) {
    switch (source) {
    case StartupSource::UserRun:
        return L"当前用户 · Run";
    case StartupSource::MachineRun:
        return L"所有用户 · Run 64 位";
    case StartupSource::MachineRun32:
        return L"所有用户 · Run 32 位";
    case StartupSource::UserFolder:
        return L"当前用户 · 启动目录";
    case StartupSource::CommonFolder:
        return L"所有用户 · 启动目录";
    default:
        return L"未知来源";
    }
}
std::vector<StartupEntry> ReadStartup(std::wstring& error) {
    std::vector<StartupEntry> result;
    for (auto source : {StartupSource::UserRun, StartupSource::MachineRun, StartupSource::MachineRun32}) {
        RegistryEntries(source, false, result, error);
        RegistryEntries(source, true, result, error);
    }
    FolderEntries(FOLDERID_Startup, StartupSource::UserFolder, result);
    FolderEntries(FOLDERID_CommonStartup, StartupSource::CommonFolder, result);
    return result;
}
Result MoveRegistryValue(HKEY hive, REGSAM view, const std::wstring& from, const std::wstring& to,
                         const std::wstring& name, DWORD type, const std::vector<BYTE>& expected) {
    if (from == to || expected.size() > 1024 * 1024 || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {false, L"不支持的启动项数据。"};
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\memclean.StartupConfiguration.v1"));
    if (!mutex)
        return {false, WinError(GetLastError())};
    DWORD wait = WaitForSingleObject(mutex.get(), 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        return {false, L"另一个窗口正在修改启动项。"};
    struct Release {
        HANDLE h;
        ~Release() { ReleaseMutex(h); }
    } release{mutex.get()};
    Key source, destination;
    LSTATUS status =
        RegOpenKeyExW(hive, from.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view, source.put());
    if (status != ERROR_SUCCESS)
        return {false, WinError(status)};
    DWORD actualType = 0;
    std::vector<BYTE> actual;
    status = ReadValue(source.get(), name, actualType, actual);
    if (status != ERROR_SUCCESS || actualType != type || actual != expected)
        return {false, L"启动项已变化或无法读取，请刷新后重试。"};
    status = RegCreateKeyExW(hive, to.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr,
                             destination.put(), nullptr);
    if (status != ERROR_SUCCESS)
        return {false, WinError(status)};
    std::vector<BYTE> existing;
    DWORD existingType = 0;
    status = ReadValue(destination.get(), name, existingType, existing);
    if (status == ERROR_SUCCESS) {
        if (existingType != type || existing != expected)
            return {false, L"目标位置存在同名但内容不同的项目，未覆盖任何数据。"};
    } else if (status == ERROR_FILE_NOT_FOUND) {
        status = RegSetValueExW(destination.get(), name.c_str(), 0, type, expected.data(),
                                static_cast<DWORD>(expected.size()));
        if (status != ERROR_SUCCESS)
            return {false, WinError(status)};
    } else
        return {false, WinError(status)};
    if (ReadValue(destination.get(), name, existingType, existing) != ERROR_SUCCESS || existingType != type ||
        existing != expected)
        return {false, L"备份校验失败，原项目已保留。"};
    if (ReadValue(source.get(), name, actualType, actual) != ERROR_SUCCESS || actualType != type ||
        actual != expected)
        return {false, L"源项目在操作中发生变化，已保留备份，请刷新核对。"};
    status = RegDeleteValueW(source.get(), name.c_str());
    if (status != ERROR_SUCCESS)
        return {false, L"副本已保留，但原项目未移除：" + WinError(status)};
    return {true, L"启动配置已更新；不会影响当前已经运行的程序。"};
}
Result ChangeStartup(const StartupEntry& entry) {
    if (!RegistrySource(entry.source))
        return {false, L"启动目录项目请通过“打开位置”或 Windows 启动设置管理。"};
    return MoveRegistryValue(Hive(entry.source), View(entry.source), entry.disabled ? kBackup : kRun,
                             entry.disabled ? kRun : kBackup, entry.name, entry.type, entry.data);
}
} // namespace mc::task
