/**
 * Copyright (c) 2013-2015, Christopher Jeffrey, Peter Sunde (MIT License)
 * Copyright (c) 2016, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 *
 * pty.cc:
 *   This file is responsible for starting processes
 *   with pseudo-terminal file descriptors.
 */

#define _WIN32_WINNT 0x600

#define NODE_ADDON_API_DISABLE_DEPRECATED
#include <node_api.h>
#include <assert.h>
#include <Shlwapi.h> // PathCombine, PathIsRelative
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <Windows.h>
#include <strsafe.h>
#include "path_util.h"
#include "conpty.h"

// Taken from the RS5 Windows SDK, but redefined here in case we're targeting <= 17134
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
  ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)

typedef VOID* HPCON;
typedef HRESULT (__stdcall *PFNCREATEPSEUDOCONSOLE)(COORD c, HANDLE hIn, HANDLE hOut, DWORD dwFlags, HPCON* phpcon);
typedef HRESULT (__stdcall *PFNRESIZEPSEUDOCONSOLE)(HPCON hpc, COORD newSize);
typedef HRESULT (__stdcall *PFNCLEARPSEUDOCONSOLE)(HPCON hpc);
typedef void (__stdcall *PFNCLOSEPSEUDOCONSOLE)(HPCON hpc);
typedef void (__stdcall *PFNRELEASEPSEUDOCONSOLE)(HPCON hpc);

#endif

// Temporary CI experiment: removed before the candidate is released.
static bool trace_cleanup_enabled() {
  wchar_t value[2];
  return GetEnvironmentVariableW(L"NODE_PTY_TRACE_CLEANUP", value, 2) == 1 && value[0] == L'1';
}

static void trace_cleanup(const char* phase, int id, HANDLE shell = nullptr) {
  if (trace_cleanup_enabled()) {
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    std::cerr << "cleanup-native " << phase << " id=" << id << " handles=" << count;
    if (shell) std::cerr << " shellHandle=" << reinterpret_cast<uintptr_t>(shell) << " shellPid=" << GetProcessId(shell);
    std::cerr << std::endl;
  }
}

struct pty_baton {
  int id;
  HANDLE hIn;
  HANDLE hOut;
  HPCON hpc;

  HANDLE hShell = nullptr;
  std::mutex mutex;
  PFNCLOSEPSEUDOCONSOLE closePseudoConsole;

  pty_baton(int _id, HANDLE _hIn, HANDLE _hOut, HPCON _hpc, PFNCLOSEPSEUDOCONSOLE close) :
    id(_id), hIn(_hIn), hOut(_hOut), hpc(_hpc), closePseudoConsole(close) {};
  ~pty_baton() {
    trace_cleanup("before-shell-close", id, hShell);
    if (hShell) CloseHandle(hShell);
    trace_cleanup("after-shell-close", id);
  }
};

static std::vector<std::shared_ptr<pty_baton>> ptyHandles;
static std::mutex ptyHandlesMutex;
static volatile LONG ptyCounter;

static std::shared_ptr<pty_baton> get_pty_baton(int id) {
  std::lock_guard<std::mutex> lock(ptyHandlesMutex);
  auto it = std::find_if(ptyHandles.begin(), ptyHandles.end(), [id](const auto& ptyHandle) {
    return ptyHandle->id == id;
  });
  if (it != ptyHandles.end()) {
    return *it;
  }
  return nullptr;
}

static bool remove_pty_baton(int id) {
  std::lock_guard<std::mutex> lock(ptyHandlesMutex);
  auto it = std::remove_if(ptyHandles.begin(), ptyHandles.end(), [id](const auto& ptyHandle) {
    return ptyHandle->id == id;
  });
  if (it != ptyHandles.end()) {
    ptyHandles.erase(it, ptyHandles.end());
    return true;
  }
  return false;
}

struct ExitEvent {
  int exit_code = 0;
  std::mutex mutex;
  std::condition_variable notified;
  bool delivered = false;
  void acknowledge() {
    std::lock_guard<std::mutex> lock(mutex);
    delivered = true;
    notified.notify_one();
  }
};

struct ExitDelivered {
  ExitEvent* event;
  ~ExitDelivered() {
    event->acknowledge();
  }
};

using ExitNotification = std::shared_ptr<ExitEvent>;

static void DeliverExit(Napi::Env env, Napi::Function cb, void*, ExitNotification* data) {
  std::unique_ptr<ExitNotification> notification(data);
  auto event = notification->get();
  ExitDelivered delivered{event};
  // The typed callback also runs with an empty env during Worker teardown.
  // Always release the native waiter, even when calling JS is no longer legal.
  if (env && cb) {
    napi_value code = nullptr;
    napi_value receiver = nullptr;
    napi_status status = napi_create_int32(env, event->exit_code, &code);
    if (status == napi_ok) status = napi_get_undefined(env, &receiver);
    if (status == napi_ok) status = napi_call_function(env, receiver, cb, 1, &code, nullptr);
    // A terminating Worker may still provide a non-null env. Napi::Function::Call
    // would throw a C++ Error here, then try to throw into that dying JS env.
    // Leave a pending exception with Node; termination cannot execute JS at all.
    if (status != napi_ok && status != napi_pending_exception && status != napi_cannot_run_js) {
      Napi::Error::Fatal("SetupExitCallback", "Cannot deliver process exit");
    }
  }
}

void SetupExitCallback(Napi::Env env, Napi::Function cb, std::shared_ptr<pty_baton> baton) {
  std::thread *th = new std::thread;
  auto exit_event = std::make_shared<ExitEvent>();
  // Don't use Napi::AsyncWorker which is limited by UV_THREADPOOL_SIZE.
  auto tsfn = Napi::TypedThreadSafeFunction<void, ExitNotification, DeliverExit>::New(
      env,
      cb,                           // JavaScript function called asynchronously
      "SetupExitCallback_resource", // Name
      0,                            // Unlimited queue
      1,                            // Only one thread will use this initially
      nullptr,
      [th, exit_event](Napi::Env, void*, void*) {
        // During env teardown Node finalizes before disposing queued callbacks.
        // Release the waiter first. The queued notification owns another shared
        // reference, so its later null-env callback cannot touch freed state.
        exit_event->acknowledge();
        th->join();
        delete th;
      });
  *th = std::thread([tsfn = std::move(tsfn), baton, exit_event] {
    WaitForSingleObject(baton->hShell, INFINITE);
    GetExitCodeProcess(baton->hShell, (LPDWORD)(&exit_event->exit_code));

    // Publish the exit code before closing conout can emit a JS socket close.
    auto notification = new ExitNotification(exit_event);
    auto status = tsfn.BlockingCall(notification);
    if (status != napi_ok) delete notification;
    if (status == napi_ok) {
      std::unique_lock<std::mutex> lock(exit_event->mutex);
      exit_event->notified.wait(lock, [&] { return exit_event->delivered; });
    }
    HPCON hpc;
    {
      std::lock_guard<std::mutex> lock(baton->mutex);
      hpc = baton->hpc;
      baton->hpc = nullptr;
    }
    // Close may wait for output to drain. Never block the JS reader or hold the
    // registry lock here. This thread is the sole owner of pseudoconsole close.
    baton->closePseudoConsole(hpc);
    trace_cleanup("after-pseudoconsole-close", baton->id);
    remove_pty_baton(baton->id);
    switch (status) {
      case napi_closing:
        break;

      case napi_queue_full:
        Napi::Error::Fatal("SetupExitCallback", "Queue was full");

      case napi_ok:
        if (tsfn.Release() != napi_ok) {
          Napi::Error::Fatal("SetupExitCallback", "ThreadSafeFunction.Release() failed");
        }
        break;

      default:
        Napi::Error::Fatal("SetupExitCallback", "ThreadSafeFunction.BlockingCall() failed");
    }
  });
}

Napi::Error errorWithCode(const Napi::CallbackInfo& info, const char* text) {
  std::stringstream errorText;
  errorText << text;
  errorText << ", error code: " << GetLastError();
  return Napi::Error::New(info.Env(), errorText.str());
}

// Returns a new server named pipe.  It has not yet been connected.
bool createDataServerPipe(bool write,
                          std::wstring kind,
                          HANDLE* hServer,
                          std::wstring &name,
                          const std::wstring &pipeName)
{
  *hServer = INVALID_HANDLE_VALUE;

  name = L"\\\\.\\pipe\\" + pipeName + L"-" + kind;

  const DWORD winOpenMode =  PIPE_ACCESS_INBOUND | PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE/*  | FILE_FLAG_OVERLAPPED */;

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);

  *hServer = CreateNamedPipeW(
      name.c_str(),
      /*dwOpenMode=*/winOpenMode,
      /*dwPipeMode=*/PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      /*nMaxInstances=*/1,
      /*nOutBufferSize=*/128 * 1024,
      /*nInBufferSize=*/128 * 1024,
      /*nDefaultTimeOut=*/30000,
      &sa);

  return *hServer != INVALID_HANDLE_VALUE;
}

HANDLE LoadConptyDll(const Napi::CallbackInfo& info,
                     const bool useConptyDll)
{
  if (!useConptyDll) {
    return LoadLibraryExW(L"kernel32.dll", 0, 0);
  }
  wchar_t currentDir[MAX_PATH];
  HMODULE hModule = GetModuleHandleA("conpty.node");
  if (hModule == NULL) {
    throw errorWithCode(info, "Failed to get conpty.node module handle");
  }
  DWORD result = GetModuleFileNameW(hModule, currentDir, MAX_PATH);
  if (result == 0) {
    throw errorWithCode(info, "Failed to get conpty.node module file name");
  }
  PathRemoveFileSpecW(currentDir);
  wchar_t conptyDllPath[MAX_PATH];
  PathCombineW(conptyDllPath, currentDir, L"conpty\\conpty.dll");
  if (!path_util::file_exists(conptyDllPath)) {
    std::wstring errorMessage = L"Cannot find conpty.dll at " + std::wstring(conptyDllPath);
    std::string errorMessageStr = path_util::wstring_to_string(errorMessage);
    throw errorWithCode(info, errorMessageStr.c_str());
  }

  return LoadLibraryW(conptyDllPath);
}

HRESULT CreateNamedPipesAndPseudoConsole(const Napi::CallbackInfo& info,
                                         COORD size,
                                         DWORD dwFlags,
                                         HANDLE *phInput,
                                         HANDLE *phOutput,
                                         HPCON* phPC,
                                         std::wstring& inName,
                                         std::wstring& outName,
                                         const std::wstring& pipeName,
                                         const bool useConptyDll)
{
  HANDLE hLibrary = LoadConptyDll(info, useConptyDll);
  DWORD error = GetLastError();
  bool fLoadedDll = hLibrary != nullptr;
  if (fLoadedDll)
  {
    PFNCREATEPSEUDOCONSOLE const pfnCreate = (PFNCREATEPSEUDOCONSOLE)GetProcAddress(
        (HMODULE)hLibrary,
        useConptyDll ? "ConptyCreatePseudoConsole" : "CreatePseudoConsole");
    if (pfnCreate)
    {
      if (phPC == NULL || phInput == NULL || phOutput == NULL)
      {
        return E_INVALIDARG;
      }

      bool success = createDataServerPipe(true, L"in", phInput, inName, pipeName);
      if (!success)
      {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      success = createDataServerPipe(false, L"out", phOutput, outName, pipeName);
      if (!success)
      {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      return pfnCreate(size, *phInput, *phOutput, dwFlags, phPC);
    }
    else
    {
      // Failed to find CreatePseudoConsole in kernel32. This is likely because
      //    the user is not running a build of Windows that supports that API.
      //    We should fall back to winpty in this case.
      return HRESULT_FROM_WIN32(GetLastError());
    }
  } else {
    throw errorWithCode(info, "Failed to load conpty.dll");
  }

  // Failed to find  kernel32. This is realy unlikely - honestly no idea how
  //    this is even possible to hit. But if it does happen, fall back to winpty.
  return HRESULT_FROM_WIN32(GetLastError());
}

static Napi::Value PtyStartProcess(const Napi::CallbackInfo& info) {
  Napi::Env env(info.Env());
  Napi::HandleScope scope(env);

  Napi::Object marshal;
  std::wstring inName, outName;
  BOOL fSuccess = FALSE;
  std::unique_ptr<wchar_t[]> mutableCommandline;
  PROCESS_INFORMATION _piClient{};

  if (info.Length() != 7 ||
      !info[0].IsString() ||
      !info[1].IsNumber() ||
      !info[2].IsNumber() ||
      !info[3].IsBoolean() ||
      !info[4].IsString() ||
      !info[5].IsBoolean() ||
      !info[6].IsBoolean()) {
    throw Napi::Error::New(env, "Usage: pty.startProcess(file, cols, rows, debug, pipeName, inheritCursor, useConptyDll)");
  }

  const std::wstring filename(path_util::to_wstring(info[0].As<Napi::String>()));
  const SHORT cols = static_cast<SHORT>(info[1].As<Napi::Number>().Uint32Value());
  const SHORT rows = static_cast<SHORT>(info[2].As<Napi::Number>().Uint32Value());
  const bool debug = info[3].As<Napi::Boolean>().Value();
  const std::wstring pipeName(path_util::to_wstring(info[4].As<Napi::String>()));
  const bool inheritCursor = info[5].As<Napi::Boolean>().Value();
  const bool useConptyDll = info[6].As<Napi::Boolean>().Value();

  // use environment 'Path' variable to determine location of
  // the relative path that we have recieved (e.g cmd.exe)
  std::wstring shellpath;
  if (::PathIsRelativeW(filename.c_str())) {
    shellpath = path_util::get_shell_path(filename.c_str());
  } else {
    shellpath = filename;
  }

  if (shellpath.empty() || !path_util::file_exists(shellpath)) {
    std::string why;
    why += "File not found: ";
    why += path_util::wstring_to_string(shellpath);
    throw Napi::Error::New(env, why);
  }

  HANDLE hIn, hOut;
  HPCON hpc;
  HRESULT hr = CreateNamedPipesAndPseudoConsole(info, {cols, rows}, inheritCursor ? 1/*PSEUDOCONSOLE_INHERIT_CURSOR*/ : 0, &hIn, &hOut, &hpc, inName, outName, pipeName, useConptyDll);

  // Restore default handling of ctrl+c
  SetConsoleCtrlHandler(NULL, FALSE);

  // Set return values
  marshal = Napi::Object::New(env);

  if (SUCCEEDED(hr)) {
    // We were able to instantiate a conpty
    const int ptyId = InterlockedIncrement(&ptyCounter);
    marshal.Set("pty", Napi::Number::New(env, ptyId));
    HANDLE library = LoadConptyDll(info, useConptyDll);
    auto close = reinterpret_cast<PFNCLOSEPSEUDOCONSOLE>(GetProcAddress(
      (HMODULE)library, useConptyDll ? "ConptyClosePseudoConsole" : "ClosePseudoConsole"));
    auto baton = std::make_shared<pty_baton>(ptyId, hIn, hOut, hpc, close);
    if (!close) {
      throw Napi::Error::New(env, "Cannot initialize conpty cleanup");
    }
    std::lock_guard<std::mutex> lock(ptyHandlesMutex);
    ptyHandles.emplace_back(std::move(baton));
  } else {
    throw Napi::Error::New(env, "Cannot launch conpty");
  }

  std::string inNameStr = path_util::wstring_to_string(inName);
  if (inNameStr.empty()) {
    throw Napi::Error::New(env, "Failed to initialize conpty conin");
  }
  std::string outNameStr = path_util::wstring_to_string(outName);
  if (outNameStr.empty()) {
    throw Napi::Error::New(env, "Failed to initialize conpty conout");
  }

  marshal.Set("fd", Napi::Number::New(env, -1));
  marshal.Set("conin", Napi::String::New(env, inNameStr));
  marshal.Set("conout", Napi::String::New(env, outNameStr));
  return marshal;
}

static Napi::Value PtyConnect(const Napi::CallbackInfo& info) {
  Napi::Env env(info.Env());
  Napi::HandleScope scope(env);

  // If we're working with conpty's we need to call ConnectNamedPipe here AFTER
  //    the Socket has attempted to connect to the other end, then actually
  //    spawn the process here.

  std::stringstream errorText;
  BOOL fSuccess = FALSE;

  if (info.Length() != 6 ||
      !info[0].IsNumber() ||
      !info[1].IsString() ||
      !info[2].IsString() ||
      !info[3].IsArray() ||
      !info[4].IsBoolean() ||
      !info[5].IsFunction()) {
    throw Napi::Error::New(env, "Usage: pty.connect(id, cmdline, cwd, env, useConptyDll, exitCallback)");
  }

  const int id = info[0].As<Napi::Number>().Int32Value();
  const std::wstring cmdline(path_util::to_wstring(info[1].As<Napi::String>()));
  const std::wstring cwd(path_util::to_wstring(info[2].As<Napi::String>()));
  const Napi::Array envValues = info[3].As<Napi::Array>();
  const bool useConptyDll = info[4].As<Napi::Boolean>().Value();
  Napi::Function exitCallback = info[5].As<Napi::Function>();

  // Fetch pty handle from ID and start process
  auto handle = get_pty_baton(id);
  if (!handle) {
    throw Napi::Error::New(env, "Invalid pty handle");
  }

  // Prepare command line
  std::unique_ptr<wchar_t[]> mutableCommandline = std::make_unique<wchar_t[]>(cmdline.length() + 1);
  HRESULT hr = StringCchCopyW(mutableCommandline.get(), cmdline.length() + 1, cmdline.c_str());

  // Prepare cwd
  std::unique_ptr<wchar_t[]> mutableCwd = std::make_unique<wchar_t[]>(cwd.length() + 1);
  hr = StringCchCopyW(mutableCwd.get(), cwd.length() + 1, cwd.c_str());

  // Prepare environment
  std::wstring envStr;
  if (!envValues.IsEmpty()) {
    std::wstring envBlock;
    for(uint32_t i = 0; i < envValues.Length(); i++) {
      envBlock += path_util::to_wstring(envValues.Get(i).As<Napi::String>());
      envBlock += L'\0';
    }
    envBlock += L'\0';
    envStr = std::move(envBlock);
  }
  std::vector<wchar_t> envV(envStr.cbegin(), envStr.cend());
  LPWSTR envArg = envV.empty() ? nullptr : envV.data();

  ConnectNamedPipe(handle->hIn, nullptr);
  ConnectNamedPipe(handle->hOut, nullptr);

  // Attach the pseudoconsole to the client application we're creating
  STARTUPINFOEXW siEx{0};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  siEx.StartupInfo.hStdError = nullptr;
  siEx.StartupInfo.hStdInput = nullptr;
  siEx.StartupInfo.hStdOutput = nullptr;

  SIZE_T size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &size);
  BYTE *attrList = new BYTE[size];
  siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrList);

  fSuccess = InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &size);
  if (!fSuccess) {
    throw errorWithCode(info, "InitializeProcThreadAttributeList failed");
  }
  fSuccess = UpdateProcThreadAttribute(siEx.lpAttributeList,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       handle->hpc,
                                       sizeof(HPCON),
                                       NULL,
                                       NULL);
  if (!fSuccess) {
    throw errorWithCode(info, "UpdateProcThreadAttribute failed");
  }

  PROCESS_INFORMATION piClient{};
  fSuccess = !!CreateProcessW(
      nullptr,
      mutableCommandline.get(),
      nullptr,                      // lpProcessAttributes
      nullptr,                      // lpThreadAttributes
      false,                        // bInheritHandles VERY IMPORTANT that this is false
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, // dwCreationFlags
      envArg,                       // lpEnvironment
      mutableCwd.get(),             // lpCurrentDirectory
      &siEx.StartupInfo,            // lpStartupInfo
      &piClient                     // lpProcessInformation
  );
  if (!fSuccess) {
    throw errorWithCode(info, "Cannot create process");
  }

  HANDLE hLibrary = LoadConptyDll(info, useConptyDll);
  bool fLoadedDll = hLibrary != nullptr;
  if (useConptyDll && fLoadedDll)
  {
    PFNRELEASEPSEUDOCONSOLE const pfnReleasePseudoConsole = (PFNRELEASEPSEUDOCONSOLE)GetProcAddress(
      (HMODULE)hLibrary, "ConptyReleasePseudoConsole");
    if (pfnReleasePseudoConsole)
    {
      pfnReleasePseudoConsole(handle->hpc);
    }
  }

  // Update handle
  handle->hShell = piClient.hProcess;

  // Close the thread handle to avoid resource leak
  CloseHandle(piClient.hThread);
  // Close the input read and output write handle of the pseudoconsole
  CloseHandle(handle->hIn);
  CloseHandle(handle->hOut);

  SetupExitCallback(env, exitCallback, handle);

  // Return
  auto marshal = Napi::Object::New(env);
  marshal.Set("pid", Napi::Number::New(env, piClient.dwProcessId));
  return marshal;
}

static Napi::Value PtyResize(const Napi::CallbackInfo& info) {
  Napi::Env env(info.Env());
  Napi::HandleScope scope(env);

  if (info.Length() != 4 ||
      !info[0].IsNumber() ||
      !info[1].IsNumber() ||
      !info[2].IsNumber() ||
      !info[3].IsBoolean()) {
    throw Napi::Error::New(env, "Usage: pty.resize(id, cols, rows, useConptyDll)");
  }

  int id = info[0].As<Napi::Number>().Int32Value();
  SHORT cols = static_cast<SHORT>(info[1].As<Napi::Number>().Uint32Value());
  SHORT rows = static_cast<SHORT>(info[2].As<Napi::Number>().Uint32Value());
  const bool useConptyDll = info[3].As<Napi::Boolean>().Value();

  auto handle = get_pty_baton(id);

  if (handle != nullptr) {
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->hpc) return env.Undefined();
    HANDLE hLibrary = LoadConptyDll(info, useConptyDll);
    bool fLoadedDll = hLibrary != nullptr;
    if (fLoadedDll)
    {
      PFNRESIZEPSEUDOCONSOLE const pfnResizePseudoConsole = (PFNRESIZEPSEUDOCONSOLE)GetProcAddress(
        (HMODULE)hLibrary,
        useConptyDll ? "ConptyResizePseudoConsole" : "ResizePseudoConsole");
      if (pfnResizePseudoConsole)
      {
        COORD size = {cols, rows};
        pfnResizePseudoConsole(handle->hpc, size);
      }
    }
  }

  return env.Undefined();
}

static Napi::Value PtyClear(const Napi::CallbackInfo& info) {
  Napi::Env env(info.Env());
  Napi::HandleScope scope(env);

  if (info.Length() != 2 ||
      !info[0].IsNumber() ||
      !info[1].IsBoolean()) {
    throw Napi::Error::New(env, "Usage: pty.clear(id, useConptyDll)");
  }

  int id = info[0].As<Napi::Number>().Int32Value();
  const bool useConptyDll = info[1].As<Napi::Boolean>().Value();

  // This API is only supported for conpty.dll as it was introduced in a later version of Windows.
  // We could hook it up to point at >= a version of Windows only, but the future is conpty.dll
  // anyway.
  if (!useConptyDll) {
    return env.Undefined();
  }

  auto handle = get_pty_baton(id);

  if (handle != nullptr) {
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->hpc) return env.Undefined();
    HANDLE hLibrary = LoadConptyDll(info, useConptyDll);
    bool fLoadedDll = hLibrary != nullptr;
    if (fLoadedDll)
    {
      PFNCLEARPSEUDOCONSOLE const pfnClearPseudoConsole = (PFNCLEARPSEUDOCONSOLE)GetProcAddress((HMODULE)hLibrary, "ConptyClearPseudoConsole");
      if (pfnClearPseudoConsole)
      {
        pfnClearPseudoConsole(handle->hpc);
      }
    }
  }

  return env.Undefined();
}

static Napi::Value PtyKill(const Napi::CallbackInfo& info) {
  Napi::Env env(info.Env());
  Napi::HandleScope scope(env);

  if (info.Length() != 2 ||
      !info[0].IsNumber() ||
      !info[1].IsBoolean()) {
    throw Napi::Error::New(env, "Usage: pty.kill(id, useConptyDll)");
  }

  int id = info[0].As<Napi::Number>().Int32Value();
  const bool useConptyDll = info[1].As<Napi::Boolean>().Value();

  auto handle = get_pty_baton(id);

  if (handle != nullptr) {
    // TerminateProcess does not wait for output. The exit thread owns close.
    if (handle->hShell) TerminateProcess(handle->hShell, 1);
  }

  return env.Undefined();
}

/**
* Init
*/

// Temporary diagnostic layouts from winsiderss/phnt ntpsapi.h. No handle is
// duplicated or closed by this snapshot. Query type only, never pipe/file names.
struct TraceHandleEntry {
  HANDLE value;
  SIZE_T handleCount;
  SIZE_T pointerCount;
  ACCESS_MASK access;
  ULONG type;
  ULONG attributes;
  ULONG reserved;
};
struct TraceHandleSnapshot {
  ULONG_PTR count;
  ULONG_PTR reserved;
  TraceHandleEntry entries[1];
};
struct TraceUnicodeString {
  USHORT length;
  USHORT capacity;
  PWSTR buffer;
};
static Napi::Value TraceHandles(const Napi::CallbackInfo& info) {
  using QueryProcess = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using QueryObject = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  auto module = GetModuleHandleW(L"ntdll.dll");
  auto queryProcess = reinterpret_cast<QueryProcess>(GetProcAddress(module, "NtQueryInformationProcess"));
  auto queryObject = reinterpret_cast<QueryObject>(GetProcAddress(module, "NtQueryObject"));
  std::vector<BYTE> buffer(1024 * 1024);
  ULONG needed = 0;
  if (queryProcess(GetCurrentProcess(), 51, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) < 0) {
    throw Napi::Error::New(info.Env(), "Cannot snapshot process handles");
  }
  auto snapshot = reinterpret_cast<TraceHandleSnapshot*>(buffer.data());
  auto result = Napi::Array::New(info.Env());
  for (ULONG_PTR i = 0; i < snapshot->count; i++) {
    auto handle = snapshot->entries[i].value;
    std::vector<BYTE> typeBuffer(4096);
    if (queryObject(handle, 2, typeBuffer.data(), static_cast<ULONG>(typeBuffer.size()), &needed) < 0) continue;
    auto type = reinterpret_cast<TraceUnicodeString*>(typeBuffer.data());
    std::wstring name(type->buffer, type->length / sizeof(wchar_t));
    auto item = Napi::Object::New(info.Env());
    item.Set("handle", Napi::Number::New(info.Env(), reinterpret_cast<uintptr_t>(handle)));
    item.Set("type", Napi::String::New(info.Env(), path_util::wstring_to_string(name)));
    item.Set("access", Napi::Number::New(info.Env(), snapshot->entries[i].access));
    if (name == L"Process") {
      item.Set("pid", Napi::Number::New(info.Env(), GetProcessId(handle)));
      DWORD exitCode = 0;
      if (GetExitCodeProcess(handle, &exitCode)) item.Set("exitCode", Napi::Number::New(info.Env(), exitCode));
      wchar_t imagePath[32768];
      DWORD length = 32768;
      if (QueryFullProcessImageNameW(handle, 0, imagePath, &length)) {
        item.Set("image", Napi::String::New(info.Env(), path_util::wstring_to_string(std::wstring(imagePath, length))));
      }
    }
    if (name == L"Thread") item.Set("tid", Napi::Number::New(info.Env(), GetThreadId(handle)));
    result.Set(static_cast<uint32_t>(i), item);
  }
  return result;
}

Napi::Object init(Napi::Env env, Napi::Object exports) {
  if (trace_cleanup_enabled()) exports.Set("_traceHandles", Napi::Function::New(env, TraceHandles));
  exports.Set("startProcess", Napi::Function::New(env, PtyStartProcess));
  exports.Set("connect", Napi::Function::New(env, PtyConnect));
  exports.Set("resize", Napi::Function::New(env, PtyResize));
  exports.Set("clear", Napi::Function::New(env, PtyClear));
  exports.Set("kill", Napi::Function::New(env, PtyKill));
  return exports;
};

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init);
