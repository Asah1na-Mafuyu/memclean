# 开发说明

## 环境

- Windows x64；桌面应用运行目标为 Windows 10 1703 及以上 / Windows 11。
- Visual Studio 2022 或 Build Tools，安装“使用 C++ 的桌面开发”和 Windows SDK。
- CMake 3.24 或以上版本，`cmake` 和 `ctest` 可从终端调用。
- PowerShell。测试使用普通权限终端，避免管理员身份改变保护规则。

当前已验证 MSVC 19.44。CMake 保留了 MinGW 的静态链接选项，但该工具链不在当前验证范围内。

## 构建与测试

在仓库根目录运行：

```powershell
.\build.ps1
.\build.ps1 -UiTests
```

第一条构建 Release x64 并运行核心测试；第二条额外执行界面检查。成功后均复制应用到 `dist/memclean.exe`，用于单文件分发。

只编译应用可以使用 `build.ps1 -SkipTests`，此时 CMake 的 `BUILD_TESTING` 设为 `OFF`。该参数不能与 `-UiTests` 同时使用。

如不使用脚本，可以执行等价的工具命令：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

这里的可执行文件位于 `build/Release/memclean.exe`。手动命令不会将文件复制到 `dist/`。程序不依赖构建目录中的其他 EXE 或 DLL。

## 测试范围

- `tests/core_tests.cpp` 验证保护策略、身份变化、取消、互斥和统计边界，并通过真实 Windows API 回收隔离进程的工作集。
- `tests/fixture.cpp` 创建带离屏窗口的测试进程，申请 128 MB 内存；回收后逐页验证内容并正常退出。前台锚点使用不同路径的同一测试程序，避免误命中同路径保护。
- `tests/ui_smoke.ps1` 检查窗口、列表、默认选项和退出流程；测试窗口放在可视桌面之外，不点击清理或提权按钮。截图仅保存在被 Git 忽略的 `work/`。

单独运行界面测试：

```powershell
.\tests\ui_smoke.ps1
```

可以通过 `-ExePath` 指定待测 EXE，通过 `-ScreenshotDirectory` 指定截图目录。构建脚本在独立 PowerShell 进程中启动界面测试，避免重复运行时 `Add-Type` 类型重定义。

实际测试不会回收用户正在使用的应用，也不会实际清理全局缓存。管理员全局操作需要在专用测试环境中另外验证。详见 [验证记录](VALIDATION.md)。

## 代码约定

- `src/core.h` 定义核心数据和接口，`src/core.cpp` 实现 Windows 操作，`src/main.cpp` 实现 Win32 界面。
- 资源位于 `resources/`，版本号在 `CMakeLists.txt`、`app.rc` 和 `app.manifest` 中保持一致。
- C++17，UTF-8，LF 换行，4 空格缩进；按仓库 `.clang-format` 格式化。
- `.editorconfig` 和 `.gitattributes` 统一编辑器与 Git 的文本格式。
- 进程无法确认、权限不足或保护状态变化时，应跳过或报告失败，不放宽保护规则以让测试通过。

如果 clang-format 已加入 PATH：

```powershell
clang-format -i src/core.h src/core.cpp src/main.cpp tests/core_tests.cpp tests/fixture.cpp
```

## 提交与分发

Git 跟踪源码、资源、文档和测试。`build/`、`dist/`、`work/`、本机编辑器配置及二进制和压缩包不提交。

便携版只需分发 `dist/memclean.exe`。构建中生成的测试进程仅供开发使用。发布源码可以从已提交的 Git 内容创建压缩包，避免带入本地测试截图或编译缓存：

```powershell
git archive --format=zip --prefix=memclean/ --output=dist/memclean-source.zip HEAD
```

程序没有额外第三方运行时依赖，也没有内置更新或联网逻辑。参考资料和历史问题处理记录见 [SOURCES.md](SOURCES.md)。
