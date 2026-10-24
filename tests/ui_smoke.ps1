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
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wp, StringBuilder lp);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr insert, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    public static IntPtr Find(uint processId) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, p) => { uint pid; GetWindowThreadProcessId(hwnd, out pid); if(pid == processId) { var name = new StringBuilder(100); GetClassNameW(hwnd, name, name.Capacity); if(name.ToString() == "memclean.Main.v1") { found = hwnd; return false; } } return true; }, IntPtr.Zero);
        return found;
    }
    public static string Text(IntPtr hwnd) {
        var text = new StringBuilder(100000);
        SendMessageW(hwnd, 0x000D, (IntPtr)text.Capacity, text);
        return text.ToString();
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
Write-Output "ALL $uiChecks UI checks passed. No user processes or global caches were cleaned."
