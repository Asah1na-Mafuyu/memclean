param(
    [string]$ExePath = (Join-Path $PSScriptRoot '..\build\Release\memclean.exe'),
    [string]$ScreenshotDirectory = (Join-Path $PSScriptRoot '..\work\screenshots')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class memcleanUi {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder value, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wp, StringBuilder lp);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr insert, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    public static IntPtr Find(uint processId, string className = "memclean.Main.v1") {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, p) => { uint pid; GetWindowThreadProcessId(hwnd, out pid); if(pid == processId) { var name = new StringBuilder(100); GetClassNameW(hwnd, name, name.Capacity); if(name.ToString() == className) { found = hwnd; return false; } } return true; }, IntPtr.Zero);
        return found;
    }
    public static string Text(IntPtr hwnd) {
        var text = new StringBuilder(100000);
        SendMessageW(hwnd, 0x000D, (IntPtr)text.Capacity, text);
        return text.ToString();
    }
    public static bool SetText(IntPtr hwnd, string text) {
        // Cross-process edit controls need WM_SETTEXT; SetWindowText only updates their cached caption.
        return SendMessageW(hwnd, 0x000C, IntPtr.Zero, new StringBuilder(text)).ToInt64() != 0;
    }
}
'@
[void][memcleanUi]::SetProcessDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Path $ScreenshotDirectory -Force | Out-Null
$uiChecks = 0
function Assert-Ui([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL $Message" }
    $script:uiChecks++
    Write-Output "PASS $Message"
}
function Send-Ui([IntPtr]$Window, [uint32]$Message, [long]$WParam = 0, [long]$LParam = 0) {
    return [memcleanUi]::SendMessageW($Window, $Message, [IntPtr]$WParam, [IntPtr]$LParam).ToInt64()
}
function Save-Window([IntPtr]$Window, [string]$Name) {
    # Make our test window drawable outside the visible desktop, without activation.
    [void][memcleanUi]::SetWindowPos($Window, [IntPtr]1, -30000, -30000, 0, 0, 0x51)
    $rect = New-Object memcleanUi+Rect
    [void][memcleanUi]::GetWindowRect($Window, [ref]$rect)
    $bitmap = New-Object System.Drawing.Bitmap(($rect.right - $rect.left), ($rect.bottom - $rect.top))
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { [void][memcleanUi]::PrintWindow($Window, $dc, 0) }
    finally { $graphics.ReleaseHdc($dc) }
    Assert-Ui ($bitmap.GetPixel(40,100).GetBrightness() -gt 0.1) "window render $Name"
    $bitmap.Save((Join-Path $ScreenshotDirectory $Name), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
foreach ($mode in @('normal', 'advanced')) {
    $arguments = if ($mode -eq 'advanced') { @('--advanced') } else { @() }
    $process = if ($arguments.Count) { Start-Process -FilePath $ExePath -ArgumentList $arguments -WindowStyle Hidden -PassThru } else { Start-Process -FilePath $ExePath -WindowStyle Hidden -PassThru }
    $window = [IntPtr]::Zero
    try {
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            $window = [memcleanUi]::Find($process.Id)
            if ($window -ne [IntPtr]::Zero -and [memcleanUi]::GetDlgItem($window, 101) -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        }
        Assert-Ui ($window -ne [IntPtr]::Zero) "$mode window created"
        $log = [memcleanUi]::GetDlgItem($window, 105)
        $cancel = [memcleanUi]::GetDlgItem($window, 102)
        Assert-Ui (-not [memcleanUi]::IsWindowEnabled($cancel)) "$mode no automatic operation on launch"
        if ($mode -eq 'normal') {
            $refresh = [memcleanUi]::GetDlgItem($window, 100)
            for ($attempt = 0; $attempt -lt 100 -and -not [memcleanUi]::IsWindowEnabled($refresh); $attempt++) { Start-Sleep -Milliseconds 100 }
            Assert-Ui ([memcleanUi]::IsWindowEnabled($refresh)) 'initial process scan completed'
            $list = [memcleanUi]::GetDlgItem($window, 104)
            $count = Send-Ui $list 0x1004
            Assert-Ui ($count -gt 0) 'process list populated'
            [void](Send-Ui ([memcleanUi]::GetDlgItem($window, 118)) 0x00F5)
            $checked = 0
            for ($i = 0; $i -lt $count; $i++) { if (((Send-Ui $list 0x102C $i 0xF000) -band 0xF000) -eq 0x2000) { $checked++ } }
            Assert-Ui ($checked -eq 0) 'deselect all clears every candidate'
            Assert-Ui (-not [memcleanUi]::IsWindowEnabled([memcleanUi]::GetDlgItem($window, 101))) 'clean disabled when selection is empty'
            [void](Send-Ui ([memcleanUi]::GetDlgItem($window, 117)) 0x00F5)
            Save-Window $window 'normal.png'
            [void](Send-Ui $refresh 0x00F5)
            for ($attempt = 0; $attempt -lt 100 -and -not [memcleanUi]::IsWindowEnabled($refresh); $attempt++) { Start-Sleep -Milliseconds 100 }
            Assert-Ui ([memcleanUi]::IsWindowEnabled($refresh)) 'manual refresh completed'
        } else {
            Assert-Ui ((Send-Ui ([memcleanUi]::GetDlgItem($window, 109)) 0x00F0) -eq 1) 'standby defaults to none'
            foreach ($id in @(106,107,108)) { Assert-Ui ((Send-Ui ([memcleanUi]::GetDlgItem($window, $id)) 0x00F0) -eq 0) 'global action unchecked by default' }
            [void](Send-Ui ([memcleanUi]::GetDlgItem($window, 106)) 0x00F5)
            [void](Send-Ui ([memcleanUi]::GetDlgItem($window, 107)) 0x00F5)
            Assert-Ui ((Send-Ui ([memcleanUi]::GetDlgItem($window, 106)) 0x00F0) -eq 0) 'standby choices are mutually exclusive'
            [void](Send-Ui ([memcleanUi]::GetDlgItem($window, 109)) 0x00F5)
            Save-Window $window 'advanced.png'
        }
        # Never click a cleanup or elevation button in the UI smoke test.
        [void][memcleanUi]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Assert-Ui ($process.WaitForExit(10000)) "$mode closes normally"
        Assert-Ui ($process.ExitCode -eq 0) "$mode successful exit"
    } finally {
        if (-not $process.HasExited -and $window -ne [IntPtr]::Zero) {
            [void][memcleanUi]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            [void]$process.WaitForExit(10000)
        }
        $process.Dispose()
    }
}
function Wait-TaskReady([IntPtr]$Window) {
    $refresh = [memcleanUi]::GetDlgItem($Window, 1002)
    for ($attempt = 0; $attempt -lt 200 -and -not [memcleanUi]::IsWindowEnabled($refresh); $attempt++) { Start-Sleep -Milliseconds 100 }
    Assert-Ui ([memcleanUi]::IsWindowEnabled($refresh)) 'task manager background sample completed'
}
$taskProcess = Start-Process -FilePath $ExePath -ArgumentList '--taskmgr' -WindowStyle Hidden -PassThru
$taskWindow = [IntPtr]::Zero
try {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $taskWindow = [memcleanUi]::Find($taskProcess.Id, 'memclean.TaskManager.v1')
        if ($taskWindow -ne [IntPtr]::Zero -and [memcleanUi]::GetDlgItem($taskWindow, 1002) -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    Assert-Ui ($taskWindow -ne [IntPtr]::Zero) 'task manager opens through command-line entry'
    Wait-TaskReady $taskWindow
    $taskList = [memcleanUi]::GetDlgItem($taskWindow, 1100)
    $taskTabs = [memcleanUi]::GetDlgItem($taskWindow, 1000)
    $taskSearch = [memcleanUi]::GetDlgItem($taskWindow, 1001)
    Assert-Ui ((Send-Ui $taskTabs 0x1304) -eq 7) 'seven task manager pages available'
    for ($attempt = 0; $attempt -lt 100 -and (Send-Ui $taskList 0x1004) -eq 0; $attempt++) { Start-Sleep -Milliseconds 100 }
    if ((Send-Ui $taskList 0x1004) -eq 0) { Write-Output ([memcleanUi]::Text([memcleanUi]::GetDlgItem($taskWindow, 1012))) }
    Assert-Ui ((Send-Ui $taskList 0x1004) -gt 0) 'task manager process table populated'
    Assert-Ui (-not [memcleanUi]::IsWindowEnabled([memcleanUi]::GetDlgItem($taskWindow, 1008))) 'no process action without selection'
    [void](Send-Ui ([memcleanUi]::GetDlgItem($taskWindow, 1004)) 0x00F5)
    Assert-Ui ([memcleanUi]::SetText($taskSearch, '__memclean_nonexistent_process_test__')) 'search accepts text'
    for ($attempt = 0; $attempt -lt 30 -and (Send-Ui $taskList 0x1004) -ne 0; $attempt++) { Start-Sleep -Milliseconds 100 }
    Assert-Ui ((Send-Ui $taskList 0x1004) -eq 0) 'search excludes nonmatching processes'
    [void][memcleanUi]::SetText($taskSearch, 'memclean.exe')
    Assert-Ui ((Send-Ui $taskList 0x1004) -ge 1) 'search finds task manager itself'
    [void](Send-Ui $taskList 0x0100 0x24)
    Assert-Ui (-not [memcleanUi]::IsWindowEnabled([memcleanUi]::GetDlgItem($taskWindow, 1008))) 'self process end action disabled'
    [void][memcleanUi]::PostMessageW($taskWindow, 0x0111, [IntPtr]1009, [IntPtr]::Zero)
    $detailsDialog = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $detailsDialog = [memcleanUi]::Find($taskProcess.Id, '#32770')
        if ($detailsDialog -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    Assert-Ui ($detailsDialog -ne [IntPtr]::Zero) 'process details dialog opens'
    $detailsText = [memcleanUi]::Text([memcleanUi]::GetDlgItem($detailsDialog, 2101))
    Assert-Ui ($detailsText.Contains('memclean.exe')) 'process details show executable information'
    [void][memcleanUi]::PostMessageW($detailsDialog, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 100
    [void][memcleanUi]::SetText($taskSearch, '')
    Save-Window $taskWindow 'task-processes.png'
    $expectedColumns = @{ 2 = 12; 3 = 5; 4 = 7; 5 = 6; 6 = 6 }
    foreach ($tabIndex in @(2,3,4,5,6,1)) {
        # TCM_SETCURFOCUS updates selection and notifies the parent like keyboard navigation.
        [void](Send-Ui $taskTabs 0x1330 $tabIndex)
        Wait-TaskReady $taskWindow
        Assert-Ui ((Send-Ui $taskTabs 0x130B) -eq $tabIndex) "task manager page $tabIndex selected"
        if ($tabIndex -eq 1) {
            Assert-Ui ([memcleanUi]::IsWindowVisible([memcleanUi]::GetDlgItem($taskWindow, 1014))) 'performance graph panel visible'
            Save-Window $taskWindow 'task-performance.png'
        } else {
            $header = [IntPtr](Send-Ui $taskList 0x101F)
            Assert-Ui ((Send-Ui $header 0x1200) -eq $expectedColumns[$tabIndex]) "page $tabIndex has its own columns"
            if ($tabIndex -in @(2,4,5,6)) { Assert-Ui ((Send-Ui $taskList 0x1004) -gt 0) "page $tabIndex contains live data" }
            if ($tabIndex -eq 3) { Save-Window $taskWindow 'task-startup.png' }
            if ($tabIndex -eq 5) { Save-Window $taskWindow 'task-services.png' }
        }
    }
    [void][memcleanUi]::PostMessageW($taskWindow, 0x0111, [IntPtr]1006, [IntPtr]::Zero)
    $runDialog = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $runDialog = [memcleanUi]::Find($taskProcess.Id, '#32770')
        if ($runDialog -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    Assert-Ui ($runDialog -ne [IntPtr]::Zero) 'run-new-task dialog opens without launching a command'
    [void][memcleanUi]::PostMessageW($runDialog, 0x0111, [IntPtr]2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 100
    [void][memcleanUi]::PostMessageW($taskWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Assert-Ui ($taskProcess.WaitForExit(10000)) 'task manager closes normally'
    Assert-Ui ($taskProcess.ExitCode -eq 0) 'task manager successful exit'
} finally {
    if (-not $taskProcess.HasExited -and $taskWindow -ne [IntPtr]::Zero) {
        $dialog = [memcleanUi]::Find($taskProcess.Id, '#32770')
        if ($dialog -ne [IntPtr]::Zero) { [void][memcleanUi]::PostMessageW($dialog, 0x0111, [IntPtr]2, [IntPtr]::Zero) }
        [void][memcleanUi]::PostMessageW($taskWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        [void]$taskProcess.WaitForExit(10000)
    }
    $taskProcess.Dispose()
}
Write-Output "ALL $uiChecks UI checks passed. No user processes, services, startup items or global caches were changed."
