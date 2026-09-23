# Diagnostic only: create and close the Windows pseudoconsole without node-pty.
param([switch]$WithShell)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Phase7ConptyControl {
  [StructLayout(LayoutKind.Sequential)] public struct Coord { public short X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct Startup {
    public uint cb; public IntPtr reserved,desktop,title;
    public uint x,y,width,height,xChars,yChars,fill,flags;
    public ushort show,reservedSize; public IntPtr reservedBytes,input,output,error;
  }
  [StructLayout(LayoutKind.Sequential)] struct StartupEx { public Startup startup; public IntPtr attributes; }
  [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool CreatePipe(out IntPtr read, out IntPtr write, IntPtr security, uint size);
  [DllImport("kernel32.dll")] static extern int CreatePseudoConsole(Coord size, IntPtr input, IntPtr output, uint flags, out IntPtr console);
  [DllImport("kernel32.dll")] static extern void ClosePseudoConsole(IntPtr console);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll")] static extern bool GetProcessHandleCount(IntPtr process, out uint count);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref IntPtr size);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
  [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcessW(string application, System.Text.StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity, bool inherit, uint flags, IntPtr environment, string cwd, ref StartupEx startup, out ProcessInfo info);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
  [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process, uint code);
  public static uint Count() { uint count; if (!GetProcessHandleCount(new IntPtr(-1), out count)) throw new Exception("handle count failed"); return count; }
  static void Shell(IntPtr console) {
    IntPtr size=IntPtr.Zero, list=IntPtr.Zero; bool initialized=false; ProcessInfo client=new ProcessInfo();
    try {
      InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref size);
      list=Marshal.AllocHGlobal(size);
      if(!(initialized=InitializeProcThreadAttributeList(list,1,0,ref size))) throw new Exception("attribute init failed");
      if(!UpdateProcThreadAttribute(list,0,new IntPtr(0x20016),console,new IntPtr(IntPtr.Size),IntPtr.Zero,IntPtr.Zero)) throw new Exception("attribute update failed");
      StartupEx startup=new StartupEx(); startup.startup.cb=(uint)Marshal.SizeOf(typeof(StartupEx)); startup.attributes=list;
      if(!CreateProcessW(null,new System.Text.StringBuilder(Environment.GetEnvironmentVariable("ComSpec")+" /d /c exit /b 7"),IntPtr.Zero,IntPtr.Zero,false,0x80000,IntPtr.Zero,null,ref startup,out client)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
      if(WaitForSingleObject(client.process,5000)!=0) { TerminateProcess(client.process,1); throw new Exception("shell timeout"); }
      uint code; if(!GetExitCodeProcess(client.process,out code)||code!=7) throw new Exception("unexpected shell exit");
    } finally {
      if(client.thread!=IntPtr.Zero) CloseHandle(client.thread);
      if(client.process!=IntPtr.Zero) CloseHandle(client.process);
      if(initialized) DeleteProcThreadAttributeList(list);
      if(list!=IntPtr.Zero) Marshal.FreeHGlobal(list);
    }
  }
  public static void Exercise(bool withShell) {
    IntPtr inputRead=IntPtr.Zero, inputWrite=IntPtr.Zero, outputRead=IntPtr.Zero, outputWrite=IntPtr.Zero, console=IntPtr.Zero;
    try {
      if (!CreatePipe(out inputRead,out inputWrite,IntPtr.Zero,0) || !CreatePipe(out outputRead,out outputWrite,IntPtr.Zero,0)) throw new Exception("pipe create failed");
      int status=CreatePseudoConsole(new Coord {X=80,Y=24},inputRead,outputWrite,0,out console);
      if(status!=0) Marshal.ThrowExceptionForHR(status);
      if(withShell) Shell(console);
    } finally {
      if(console!=IntPtr.Zero) ClosePseudoConsole(console);
      foreach(IntPtr handle in new[]{inputRead,inputWrite,outputRead,outputWrite}) if(handle!=IntPtr.Zero) CloseHandle(handle);
    }
  }
}
'@
for ($round=0; $round -lt 5; $round++) {
  [Phase7ConptyControl]::Exercise($WithShell.IsPresent)
  Start-Sleep -Milliseconds 2500
  [GC]::Collect()
  [GC]::WaitForPendingFinalizers()
  [pscustomobject]@{round=$round;handles=[Phase7ConptyControl]::Count()} | ConvertTo-Json -Compress
}
