# 实现依据与历史问题处理

这是独立实现，以下项目用于研究接口、行为和历史问题。未将其应用代码作为依赖或复制进本项目。以下链接固定了研究所用版本，便于复核。

## PCL-CE 三份历史文件

- [MemSwapService.cs](https://github.com/PCL-Community/PCL-CE/blob/7b5c912ffa29ef5554d069e7605d3faa99238c4a/PCL.Core/App/Tools/MemSwapService.cs)
- [MemSwapWorks.cs](https://github.com/PCL-Community/PCL-CE/blob/7b5c912ffa29ef5554d069e7605d3faa99238c4a/PCL.Core/App/Tools/MemSwapWorks.cs)
- [NtInterop.cs](https://github.com/PCL-Community/PCL-CE/blob/7b5c912ffa29ef5554d069e7605d3faa99238c4a/PCL.Core/Utils/OS/NtInterop.cs)

| 历史问题或设计取舍 | 本项目处理 |
| --- | --- |
| 无符号 `after - before` 可能下溢 | 先比较大小，再按正确方向相减，显示“增加”或“减少” |
| x64 文件缓存结构体包含错误的指针宽度字段 | 使用公开 API，完全绕开该私有结构体 |
| 用 `uint.MaxValue` 表示 x64 的 `SIZE_T(-1)` | 使用 `std::numeric_limits<SIZE_T>::max()` |
| 原生 NTSTATUS 与 Win32 错误混用 | `LONG` 保存 NTSTATUS，按 `status >= 0` 判断；用返回 `ULONG` 的 RtlNtStatusToDosError 转换并保留原值 |
| 权限启用后长期保留 | 每项使用 RAII 保存并恢复原权限状态；检查 ERROR_NOT_ALL_ASSIGNED |
| 一键默认全套全局操作 | 默认仅清理经过筛选的后台桌面进程；全局操作另开管理员窗口，全部默认关闭 |
| 进程内锁不能阻止多个实例并发清理 | 以会话内命名互斥量覆盖普通与高级进程，失败即停止 |
| 请求受理不等于实际清理完成 | 工作线程结束后汇总逐项结果，不将启动请求当作成功 |
| 外部提权调用的状态恢复复杂 | 不做清理命令 IPC。提权只打开高级界面；用户在其中重新选择并执行，取消授权不改变普通模式任务状态 |
| 单项失败影响后续项的结果解释 | 每项单独记录成功/失败；可取消未开始的项，已执行的全局动作不伪装成可回滚 |

## Windows 接口资料

- [EmptyWorkingSet：参数、最低权限与返回值](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-emptyworkingset)
- [Working Set：驻留页面与再次访问](https://learn.microsoft.com/en-us/windows/win32/memory/working-set)
- [MEMORYSTATUSEX：可用内存包含哪些页面](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex)
- [SetSystemFileCacheSize：SIZE_T(-1) 和所需权限](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setsystemfilecachesize)
- [AdjustTokenPrivileges：检查 ERROR_NOT_ALL_ASSIGNED 与恢复原状态](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges)
- [RtlNtStatusToDosError：ULONG 返回值](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-rtlntstatustodoserror)
- [MSVC /MT 静态运行库](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library)
- [phnt 的原生枚举与结构定义](https://github.com/winsiderss/phnt/blob/53fbbdc5b5d2b08761db1c7b26bfa8c820924356/ntexapi.h)

高级待机列表使用信息类 80，低优先级命令 5 或全部命令 4。这个接口并非稳定的公开 Win32 契约，故仅在高级选项中提供并动态加载。没有把历史项目的七项清理全数搬入。

## 其他研究项目

- [Mem Reduct 原生实现](https://github.com/henrypp/memreduct/blob/04402de8b6a8ff9d1c5517c675d87c346655323c/src/main.c)
- [WinMemoryCleaner 进程排除与清理实现](https://github.com/IgorMundstein/WinMemoryCleaner/blob/a19f25719c9b9a58a0acea99cb8adb4737d2d318/src/Service/ComputerService.cs)

这些项目的代码具有各自的许可证。本项目没有捆绑其源码或二进制。

## 任务管理器使用的 Windows 文档

2.0 的任务管理器采用独立的 C++ / Win32 实现，没有引入其他任务管理工具的代码或内核驱动。主要依据：

- [PdhAddEnglishCounterW：使用语言无关的英文计数器路径](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
- [PdhGetFormattedCounterArrayW：读取通配计数器实例](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw)
- [SetPriorityClass：进程调度优先级](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass)
- [EnumServicesStatusExW：服务状态枚举](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexw)
- [WTSEnumerateSessionsW：用户会话枚举](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsenumeratesessionsw)
- [Run / RunOnce 注册表键](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

计数器值、接口权限和兼容性仍由当前 Windows 决定。对于系统任务管理器未公开的启动状态格式，本项目没有推断二进制格式后直接改写；只对公开 Run 配置进行原始值备份、校验和恢复。功能边界见 [TASK_MANAGER.md](TASK_MANAGER.md)。
