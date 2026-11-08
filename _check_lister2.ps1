$ErrorActionPreference='Stop'
Get-Process POSCommander2 -ErrorAction SilentlyContinue | Stop-Process -Force
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class TL3 {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr GetText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public class W { public IntPtr H; public string Cls, Txt; }
  public static List<W> Tops(uint procId, IntPtr except) {
    var l = new List<W>();
    EnumWindows((h,p) => { uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp==procId && h!=except && IsWindowVisible(h)) { var w = new W { H = h };
        var c = new StringBuilder(256); GetClassNameW(h,c,256); w.Cls = c.ToString();
        var t = new StringBuilder(512); GetWindowTextW(h,t,512); w.Txt = t.ToString(); l.Add(w); }
      return true; }, IntPtr.Zero); return l; }
  public static IntPtr Child(IntPtr root, string cls) {
    IntPtr f = IntPtr.Zero;
    EnumChildWindows(root, (h,p) => { var c = new StringBuilder(256); GetClassNameW(h,c,256);
      if (c.ToString() == cls && f == IntPtr.Zero) f = h; return true; }, IntPtr.Zero); return f; }
  public static string Head(IntPtr e, int n) { var sb = new StringBuilder(n+1); GetText(e, 0x000D, (IntPtr)n, sb); return sb.ToString().Replace("\r","  ").Replace("\n","  "); }
}
"@
function Tap([int]$vk){ [TL3]::keybd_event([byte]$vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 70; [TL3]::keybd_event([byte]$vk,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 250 }
$dir='E:\posc2-perf\view'
$p = Start-Process 'E:\Work\VSC\POSCommander2\build\POSCommander2.exe' -WorkingDirectory $dir -PassThru
$sw=[Diagnostics.Stopwatch]::StartNew()
while(-not $p.HasExited -and $p.MainWindowHandle -eq 0 -and $sw.ElapsedMilliseconds -lt 10000){ Start-Sleep -Milliseconds 10; $p.Refresh() }
[void][TL3]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 900
try {
  Tap 0x28; Tap 0x72
  Start-Sleep -Milliseconds 600
  $w = [TL3]::Tops($p.Id,$p.MainWindowHandle) | Where-Object { $_.Cls -eq 'POSLister2' } | Select-Object -First 1
  "bin:      '$($w.Txt)'"
  "bin head: '" + [TL3]::Head([TL3]::Child($w.H,'Edit'), 40) + "'"
  Tap 0x1B; Start-Sleep -Milliseconds 400
  Tap 0x28; Tap 0x72   # второй файл — текст.txt
  Start-Sleep -Milliseconds 600
  $w2 = [TL3]::Tops($p.Id,$p.MainWindowHandle) | Where-Object { $_.Cls -eq 'POSLister2' } | Select-Object -First 1
  "txt:      '$($w2.Txt)'"
  "txt head: '" + [TL3]::Head([TL3]::Child($w2.H,'Edit'), 60) + "'"
  Tap 0x1B
}
finally { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
