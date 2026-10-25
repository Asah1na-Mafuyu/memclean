#include "task_manager.h"
#include <wtsapi32.h>
#include <algorithm>

namespace mc::task {
namespace {
class ServiceHandle {
    SC_HANDLE value_ = nullptr;

public:
    explicit ServiceHandle(SC_HANDLE value) : value_(value) {}
    ~ServiceHandle() {
        if (value_)
            CloseServiceHandle(value_);
    }
    ServiceHandle(const ServiceHandle&) = delete;
    SC_HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
};
bool ServiceConfig(SC_HANDLE service, ServiceInfo& info) {
    DWORD size = 0;
    QueryServiceConfigW(service, nullptr, 0, &size);
    if (!size || size > 1024 * 1024)
        return false;
    std::vector<BYTE> data(size);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data());
    if (!QueryServiceConfigW(service, config, size, &size))
        return false;
    info.startType = config->dwStartType;
    info.path = config->lpBinaryPathName;
    info.account = config->lpServiceStartName;
    return true;
}
std::wstring WtsText(DWORD session, WTS_INFO_CLASS kind) {
    LPWSTR text = nullptr;
    DWORD size = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, kind, &text, &size))
        return {};
    std::wstring value = text && size >= sizeof(wchar_t) ? text : L"";
    WTSFreeMemory(text);
    return value;
}
Result WaitService(SC_HANDLE service, DWORD wanted, const std::atomic_bool& cancel) {
    auto start = GetTickCount64();
    while (GetTickCount64() - start < 15000) {
        SERVICE_STATUS_PROCESS state{};
        DWORD size = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&state),
                                  sizeof(state), &size))
            return {false, WinError(GetLastError())};
        if (state.dwCurrentState == wanted)
            return {true, L"服务状态已更新。"};
        if (cancel.load())
            return {false, L"已停止等待；先前发出的服务请求仍可能完成。"};
        Sleep(200);
    }
    return {false, L"请求已经发出，但 15 秒内未达到目标状态；请刷新检查。"};
}
} // namespace
std::wstring ServiceState(DWORD value) {
    switch (value) {
    case SERVICE_STOPPED:
        return L"已停止";
    case SERVICE_START_PENDING:
        return L"正在启动";
    case SERVICE_STOP_PENDING:
        return L"正在停止";
    case SERVICE_RUNNING:
        return L"正在运行";
    case SERVICE_CONTINUE_PENDING:
        return L"正在继续";
    case SERVICE_PAUSE_PENDING:
        return L"正在暂停";
    case SERVICE_PAUSED:
        return L"已暂停";
    default:
        return L"未知";
    }
}
std::wstring ServiceStartName(DWORD value) {
    switch (value) {
    case SERVICE_AUTO_START:
        return L"自动";
    case SERVICE_DEMAND_START:
        return L"手动 / 触发启动";
    case SERVICE_DISABLED:
        return L"禁用";
    case SERVICE_BOOT_START:
        return L"引导";
    case SERVICE_SYSTEM_START:
        return L"系统";
    default:
        return L"—";
    }
}
std::wstring SessionState(DWORD value) {
    switch (value) {
    case WTSActive:
        return L"活动";
    case WTSConnected:
        return L"已连接";
    case WTSDisconnected:
        return L"已断开";
    case WTSIdle:
        return L"空闲";
    case WTSListen:
        return L"侦听";
    default:
        return L"其他状态";
    }
}
std::vector<ServiceInfo> ReadServices(std::wstring& error) {
    std::vector<ServiceInfo> result;
    ServiceHandle manager(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!manager) {
        error = WinError(GetLastError());
        return result;
    }
    std::vector<BYTE> buffer(256 * 1024);
    DWORD resume = 0;
    for (unsigned page = 0; page < 32; ++page) {
        DWORD needed = 0, count = 0;
        BOOL success = EnumServicesStatusExW(
            manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(),
            static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr);
        DWORD status = success ? ERROR_SUCCESS : GetLastError();
        if (!success && status != ERROR_MORE_DATA) {
            error = WinError(status);
            break;
        }
        auto services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            ServiceInfo info;
            info.name = services[i].lpServiceName;
            info.display = services[i].lpDisplayName;
            info.state = services[i].ServiceStatusProcess.dwCurrentState;
            info.pid = services[i].ServiceStatusProcess.dwProcessId;
            info.accepted = services[i].ServiceStatusProcess.dwControlsAccepted;
            ServiceHandle service(OpenServiceW(manager.get(), info.name.c_str(), SERVICE_QUERY_CONFIG));
            if (service)
                ServiceConfig(service.get(), info);
            result.push_back(std::move(info));
        }
        if (success)
            break;
        if (!count) {
            error = L"服务列表未能完整读取。";
            break;
        }
    }
    return result;
}
Result ChangeService(const ServiceInfo& expected, ServiceAction action, const std::atomic_bool& cancel) {
    if (expected.name.empty())
        return {false, L"未选择服务。"};
    DWORD rights = SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG;
    switch (action) {
    case ServiceAction::Start:
        rights |= SERVICE_START;
        break;
    case ServiceAction::Stop:
        rights |= SERVICE_STOP;
        break;
    case ServiceAction::Restart:
        rights |= SERVICE_STOP | SERVICE_START;
        break;
    case ServiceAction::Pause:
    case ServiceAction::Continue:
        rights |= SERVICE_PAUSE_CONTINUE;
        break;
    case ServiceAction::Automatic:
    case ServiceAction::Manual:
    case ServiceAction::Disabled:
        rights |= SERVICE_CHANGE_CONFIG;
        break;
    default:
        return {false, L"不支持的服务操作。"};
    }
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager)
        return {false, WinError(GetLastError())};
    ServiceHandle service(OpenServiceW(manager.get(), expected.name.c_str(), rights));
    if (!service)
        return {false, L"无法打开服务，可能需要管理员权限：" + WinError(GetLastError())};
    ServiceInfo fresh;
    if (!ServiceConfig(service.get(), fresh) || fresh.path != expected.path ||
        fresh.account != expected.account)
        return {false, L"服务配置已变化或无法确认，请刷新后重试。"};
    if (cancel.load())
        return {false, L"操作已取消。"};
    if (action == ServiceAction::Automatic || action == ServiceAction::Manual ||
        action == ServiceAction::Disabled) {
        DWORD start = action == ServiceAction::Automatic ? SERVICE_AUTO_START
                      : action == ServiceAction::Manual  ? SERVICE_DEMAND_START
                                                         : SERVICE_DISABLED;
        if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return {false, WinError(GetLastError())};
        return {true, L"启动类型已更新，不会立即改变当前运行状态。"};
    }
    SERVICE_STATUS_PROCESS state{};
    DWORD size = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&state),
                              sizeof(state), &size))
        return {false, WinError(GetLastError())};
    if ((action == ServiceAction::Stop || action == ServiceAction::Restart) &&
        state.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS status{};
        if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status))
            return {false, WinError(GetLastError())};
        auto wait = WaitService(service.get(), SERVICE_STOPPED, cancel);
        if (!wait.success || action == ServiceAction::Stop)
            return wait;
    }
    if (action == ServiceAction::Start || action == ServiceAction::Restart) {
        if (cancel.load())
            return {false, L"后续启动已取消；已停止的服务不会自动恢复。"};
        if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            return {false, WinError(GetLastError())};
        return WaitService(service.get(), SERVICE_RUNNING, cancel);
    }
    if (action == ServiceAction::Pause || action == ServiceAction::Continue) {
        SERVICE_STATUS status{};
        if (!ControlService(service.get(),
                            action == ServiceAction::Pause ? SERVICE_CONTROL_PAUSE : SERVICE_CONTROL_CONTINUE,
                            &status))
            return {false, WinError(GetLastError())};
        return WaitService(service.get(), action == ServiceAction::Pause ? SERVICE_PAUSED : SERVICE_RUNNING,
                           cancel);
    }
    return {true, L"服务已停止。"};
}
std::vector<SessionInfo> ReadSessions(std::wstring& error) {
    std::vector<SessionInfo> result;
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        error = WinError(GetLastError());
        return result;
    }
    for (DWORD i = 0; i < count; ++i) {
        auto user = WtsText(sessions[i].SessionId, WTSUserName);
        if (user.empty())
            continue;
        auto domain = WtsText(sessions[i].SessionId, WTSDomainName);
        SessionInfo info;
        info.id = sessions[i].SessionId;
        info.state = sessions[i].State;
        info.name = domain.empty() ? user : domain + L"\\" + user;
        info.station = sessions[i].pWinStationName;
        LPWSTR data = nullptr;
        DWORD size = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, info.id, WTSSessionInfoEx, &data, &size)) {
            if (size >= sizeof(WTSINFOEXW)) {
                auto details = reinterpret_cast<WTSINFOEXW*>(data);
                if (details->Level == 1)
                    info.logon = details->Data.WTSInfoExLevel1.LogonTime.QuadPart;
            }
            WTSFreeMemory(data);
        }
        result.push_back(std::move(info));
    }
    WTSFreeMemory(sessions);
    return result;
}
Result ChangeSession(const SessionInfo& expected, bool logoff) {
    DWORD current = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current);
    if (!expected.id || expected.id == current || !expected.logon)
        return {false, L"本窗口只管理其他已确认身份的用户会话；当前会话请使用 Windows 的注销菜单。"};
    std::wstring error;
    auto sessions = ReadSessions(error);
    auto found = std::find_if(sessions.begin(), sessions.end(), [&](const SessionInfo& session) {
        return session.id == expected.id && session.logon == expected.logon && session.name == expected.name;
    });
    if (found == sessions.end())
        return {false, L"会话已经变化或无法确认，请刷新后重试。"};
    BOOL result = logoff ? WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, expected.id, FALSE)
                         : WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, expected.id, FALSE);
    return result ? Result{true, L"已发送会话操作请求。"} : Result{false, WinError(GetLastError())};
}
} // namespace mc::task
