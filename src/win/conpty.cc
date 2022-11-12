/**
 * Copyright (c) 2013-2015, Christopher Jeffrey, Peter Sunde (MIT License)
 * Copyright (c) 2016, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 *
 * pty.cc:
 *   This file is responsible for starting processes
 *   with pseudo-terminal file descriptors.
 */

// node versions lower than 10 define this as 0x502 which disables many of the definitions needed to compile
#include <node_version.h>
#if NODE_API_MODULE_VERSION <= 57
  #define _WIN32_WINNT 0x600
#endif

#include <iostream>
#include <napi.h>
#include <uv.h>
#include <Shlwapi.h> // PathCombine, PathIsRelative
#include <sstream>
#include <string>
#include <vector>
#include <Windows.h>
#include <strsafe.h>
#include "path_util.h"

extern "C" void init(Napi::Object);

// Taken from the RS5 Windows SDK, but redefined here in case we're targeting <= 17134
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
  ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)

typedef VOID* HPCON;
typedef HRESULT (__stdcall *PFNCREATEPSEUDOCONSOLE)(COORD c, HANDLE hIn, HANDLE hOut, DWORD dwFlags, HPCON* phpcon);
typedef HRESULT (__stdcall *PFNRESIZEPSEUDOCONSOLE)(HPCON hpc, COORD newSize);
typedef void (__stdcall *PFNCLOSEPSEUDOCONSOLE)(HPCON hpc);

#endif

struct pty_baton {
  int id;
  HANDLE hIn;
  HANDLE hOut;
  HPCON hpc;

  HANDLE hShell;
  HANDLE hWait;
  Napi::FunctionReference cb;
  uv_async_t async;
  uv_thread_t tid;

  pty_baton(int _id, HANDLE _hIn, HANDLE _hOut, HPCON _hpc) : id(_id), hIn(_hIn), hOut(_hOut), hpc(_hpc) {};
};

static std::vector<pty_baton*> ptyHandles;
static volatile LONG ptyCounter;

static pty_baton* get_pty_baton(int id) {
  for (size_t i = 0; i < ptyHandles.size(); ++i) {
    pty_baton* ptyHandle = ptyHandles[i];
    if (ptyHandle->id == id) {
      return ptyHandle;
    }
  }
  return nullptr;
}

template <typename T>
std::vector<T> vectorFromString(const std::basic_string<T> &str) {
    return std::vector<T>(str.begin(), str.end());
}

void throwNanError(const Napi::FunctionCallbackInfo<v8::Value>* info, const char* text, const bool getLastError) {
  std::stringstream errorText;
  errorText << text;
  if (getLastError) {
    errorText << ", error code: " << GetLastError();
  }
  Napi::Error::New(env, errorText.str().c_str()).ThrowAsJavaScriptException();

  (*info).GetReturnValue().SetUndefined();
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
      /*nOutBufferSize=*/0,
      /*nInBufferSize=*/0,
      /*nDefaultTimeOut=*/30000,
      &sa);

  return *hServer != INVALID_HANDLE_VALUE;
}

HRESULT CreateNamedPipesAndPseudoConsole(COORD size,
                                         DWORD dwFlags,
                                         HANDLE *phInput,
                                         HANDLE *phOutput,
                                         HPCON* phPC,
                                         std::wstring& inName,
                                         std::wstring& outName,
                                         const std::wstring& pipeName)
{
  HANDLE hLibrary = LoadLibraryExW(L"kernel32.dll", 0, 0);
  bool fLoadedDll = hLibrary != nullptr;
  if (fLoadedDll)
  {
    PFNCREATEPSEUDOCONSOLE const pfnCreate = (PFNCREATEPSEUDOCONSOLE)GetProcAddress((HMODULE)hLibrary, "CreatePseudoConsole");
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
  }

  // Failed to find  kernel32. This is realy unlikely - honestly no idea how
  //    this is even possible to hit. But if it does happen, fall back to winpty.
  return HRESULT_FROM_WIN32(GetLastError());
}

static Napi::Value PtyStartProcess(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Object marshal;
  std::wstring inName, outName;
  BOOL fSuccess = FALSE;
  std::unique_ptr<wchar_t[]> mutableCommandline;
  PROCESS_INFORMATION _piClient{};

  if (info.Length() != 6 ||
      !info[0].IsString() ||
      !info[1].IsNumber() ||
      !info[2].IsNumber() ||
      !info[3].IsBoolean() ||
      !info[4].IsString() ||
      !info[5].IsBoolean()) {
    Napi::Error::New(env, "Usage: pty.startProcess(file, cols, rows, debug, pipeName, inheritCursor)").ThrowAsJavaScriptException();
    return env.Null();
  }

  const std::wstring filename(path_util::to_wstring(std::string(info[0])));
  const SHORT cols = info[1].Uint32Value(Napi::GetCurrentContext());
  const SHORT rows = info[2].Uint32Value(Napi::GetCurrentContext());
  const bool debug = info[3].As<Napi::Boolean>().Value();
  const std::wstring pipeName(path_util::to_wstring(std::string(info[4])));
  const bool inheritCursor = info[5].As<Napi::Boolean>().Value();

  // use environment 'Path' variable to determine location of
  // the relative path that we have recieved (e.g cmd.exe)
  std::wstring shellpath;
  if (::PathIsRelativeW(filename.c_str())) {
    shellpath = path_util::get_shell_path(filename.c_str());
  } else {
    shellpath = filename;
  }

  std::string shellpath_(shellpath.begin(), shellpath.end());

  if (shellpath.empty() || !path_util::file_exists(shellpath)) {
    std::stringstream why;
    why << "File not found: " << shellpath_;
    Napi::Error::New(env, why.str().c_str()).ThrowAsJavaScriptException();
    return env.Null();
  }

  HANDLE hIn, hOut;
  HPCON hpc;
  HRESULT hr = CreateNamedPipesAndPseudoConsole({cols, rows}, inheritCursor ? 1/*PSEUDOCONSOLE_INHERIT_CURSOR*/ : 0, &hIn, &hOut, &hpc, inName, outName, pipeName);

  // Restore default handling of ctrl+c
  SetConsoleCtrlHandler(NULL, FALSE);

  // Set return values
  marshal = Napi::Object::New(env);

  if (SUCCEEDED(hr)) {
    // We were able to instantiate a conpty
    const int ptyId = InterlockedIncrement(&ptyCounter);
    (marshal).Set(Napi::String::New(env, "pty"), Napi::Number::New(env, ptyId));
    ptyHandles.insert(ptyHandles.end(), new pty_baton(ptyId, hIn, hOut, hpc));
  } else {
    Napi::Error::New(env, "Cannot launch conpty").ThrowAsJavaScriptException();
    return env.Null();
  }

  (marshal).Set(Napi::String::New(env, "fd"), Napi::Number::New(env, -1));
  {
    std::string coninPipeNameStr(inName.begin(), inName.end());
    (marshal).Set(Napi::String::New(env, "conin"), Napi::String::New(env, coninPipeNameStr));

    std::string conoutPipeNameStr(outName.begin(), outName.end());
    (marshal).Set(Napi::String::New(env, "conout"), Napi::String::New(env, conoutPipeNameStr));
  }
  return marshal;
}

VOID CALLBACK OnProcessExitWinEvent(
    _In_ PVOID context,
    _In_ BOOLEAN TimerOrWaitFired) {
  pty_baton *baton = static_cast<pty_baton*>(context);

  // Fire OnProcessExit
  uv_async_send(&baton->async);
}

static void OnProcessExit(uv_async_t *async) {
  Napi::HandleScope scope(env);
  pty_baton *baton = static_cast<pty_baton*>(async->data);

  UnregisterWait(baton->hWait);

  // Get exit code
  DWORD exitCode = 0;
  GetExitCodeProcess(baton->hShell, &exitCode);

  // Call function
  Napi::Value args[1] = {
    Napi::Number::New(env, exitCode)
  };

  Napi::AsyncResource asyncResource("node-pty.callback");
  baton->cb.Call(1, args, &asyncResource);
  // Clean up
  baton->cb.Reset();
}

static Napi::Value PtyConnect(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  // If we're working with conpty's we need to call ConnectNamedPipe here AFTER
  //    the Socket has attempted to connect to the other end, then actually
  //    spawn the process here.

  std::stringstream errorText;
  BOOL fSuccess = FALSE;

  if (info.Length() != 5 ||
      !info[0].IsNumber() ||
      !info[1].IsString() ||
      !info[2].IsString() ||
      !info[3].IsArray() ||
      !info[4].IsFunction()) {
    Napi::Error::New(env, "Usage: pty.connect(id, cmdline, cwd, env, exitCallback)").ThrowAsJavaScriptException();
    return env.Null();
  }

  const int id = info[0].Int32Value(Napi::GetCurrentContext());
  const std::wstring cmdline(path_util::to_wstring(std::string(info[1])));
  const std::wstring cwd(path_util::to_wstring(std::string(info[2])));
  const Napi::Array envValues = info[3].As<Napi::Array>();
  const Napi::Function exitCallback = info[4].As<Napi::Function>();

  // Fetch pty handle from ID and start process
  pty_baton* handle = get_pty_baton(id);
  if (!handle) {
    Napi::Error::New(env, "Invalid pty handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Prepare command line
  std::unique_ptr<wchar_t[]> mutableCommandline = std::make_unique<wchar_t[]>(cmdline.Length() + 1);
  HRESULT hr = StringCchCopyW(mutableCommandline.get(), cmdline.Length() + 1, cmdline.c_str());

  // Prepare cwd
  std::unique_ptr<wchar_t[]> mutableCwd = std::make_unique<wchar_t[]>(cwd.Length() + 1);
  hr = StringCchCopyW(mutableCwd.get(), cwd.Length() + 1, cwd.c_str());

  // Prepare environment
  std::wstring env;
  if (!envValues.IsEmpty()) {
    std::wstringstream envBlock;
    for(uint32_t i = 0; i < envValues->Length(); i++) {
      std::wstring envValue(path_util::to_wstring(std::string((envValues).Get(i))));
      envBlock << envValue << L'\0';
    }
    envBlock << L'\0';
    env = envBlock.str();
  }
  auto envV = vectorFromString(env);
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
    return throwNanError(&info, "InitializeProcThreadAttributeList failed", true);
  }
  fSuccess = UpdateProcThreadAttribute(siEx.lpAttributeList,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       handle->hpc,
                                       sizeof(HPCON),
                                       NULL,
                                       NULL);
  if (!fSuccess) {
    return throwNanError(&info, "UpdateProcThreadAttribute failed", true);
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
    return throwNanError(&info, "Cannot create process", true);
  }

  // Update handle
  handle->hShell = piClient.hProcess;
  handle->cb.Reset(exitCallback);
  handle->async.data = handle;

  // Setup OnProcessExit callback
  uv_async_init(uv_default_loop(), &handle->async, OnProcessExit);

  // Setup Windows wait for process exit event
  RegisterWaitForSingleObject(&handle->hWait, piClient.hProcess, OnProcessExitWinEvent, (PVOID)handle, INFINITE, WT_EXECUTEONLYONCE);

  // Return
  Napi::Object marshal = Napi::Object::New(env);
  (marshal).Set(Napi::String::New(env, "pid"), Napi::Number::New(env, piClient.dwProcessId));
  return marshal;
}

static Napi::Value PtyResize(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  if (info.Length() != 3 ||
      !info[0].IsNumber() ||
      !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::Error::New(env, "Usage: pty.resize(id, cols, rows)").ThrowAsJavaScriptException();
    return env.Null();
  }

  int id = info[0].Int32Value(Napi::GetCurrentContext());
  SHORT cols = info[1].Uint32Value(Napi::GetCurrentContext());
  SHORT rows = info[2].Uint32Value(Napi::GetCurrentContext());

  const pty_baton* handle = get_pty_baton(id);

  if (handle != nullptr) {
    HANDLE hLibrary = LoadLibraryExW(L"kernel32.dll", 0, 0);
    bool fLoadedDll = hLibrary != nullptr;
    if (fLoadedDll)
    {
      PFNRESIZEPSEUDOCONSOLE const pfnResizePseudoConsole = (PFNRESIZEPSEUDOCONSOLE)GetProcAddress((HMODULE)hLibrary, "ResizePseudoConsole");
      if (pfnResizePseudoConsole)
      {
        COORD size = {cols, rows};
        pfnResizePseudoConsole(handle->hpc, size);
      }
    }
  }

  return return env.Undefined();
}

static Napi::Value PtyKill(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  if (info.Length() != 1 ||
      !info[0].IsNumber()) {
    Napi::Error::New(env, "Usage: pty.kill(id)").ThrowAsJavaScriptException();
    return env.Null();
  }

  int id = info[0].Int32Value(Napi::GetCurrentContext());

  const pty_baton* handle = get_pty_baton(id);

  if (handle != nullptr) {
    HANDLE hLibrary = LoadLibraryExW(L"kernel32.dll", 0, 0);
    bool fLoadedDll = hLibrary != nullptr;
    if (fLoadedDll)
    {
      PFNCLOSEPSEUDOCONSOLE const pfnClosePseudoConsole = (PFNCLOSEPSEUDOCONSOLE)GetProcAddress((HMODULE)hLibrary, "ClosePseudoConsole");
      if (pfnClosePseudoConsole)
      {
        pfnClosePseudoConsole(handle->hpc);
      }
    }

    DisconnectNamedPipe(handle->hIn);
    DisconnectNamedPipe(handle->hOut);
    CloseHandle(handle->hIn);
    CloseHandle(handle->hOut);
    CloseHandle(handle->hShell);
  }

  return return env.Undefined();
}

/**
* Init
*/

extern "C" void init(Napi::Object target) {
  Napi::HandleScope scope(env);
  exports.Set(Napi::String::New(env, "startProcess"), Napi::Function::New(env, PtyStartProcess));
  exports.Set(Napi::String::New(env, "connect"), Napi::Function::New(env, PtyConnect));
  exports.Set(Napi::String::New(env, "resize"), Napi::Function::New(env, PtyResize));
  exports.Set(Napi::String::New(env, "kill"), Napi::Function::New(env, PtyKill));
};

NODE_API_MODULE(pty, init);
