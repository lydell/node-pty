/**
 * Copyright (c) 2019, Microsoft Corporation (MIT License).
 */

#include <napi.h>
#include <uv.h>
#include <windows.h>

static Napi::Value ApiConsoleProcessList(const Napi::CallbackInfo& info) {
  if (info.Length() != 1 ||
      !info[0].IsNumber()) {
    Napi::Error::New(env, "Usage: getConsoleProcessList(shellPid)").ThrowAsJavaScriptException();
    return env.Null();
  }

  const SHORT pid = info[0].Uint32Value(Napi::GetCurrentContext());

  if (!FreeConsole()) {
    Napi::Error::New(env, "FreeConsole failed").ThrowAsJavaScriptException();

  }
  if (!AttachConsole(pid)) {
    Napi::Error::New(env, "AttachConsole failed").ThrowAsJavaScriptException();

  }
  auto processList = std::vector<DWORD>(64);
  auto processCount = GetConsoleProcessList(&processList[0], processList.size());
  if (processList.size() < processCount) {
      processList.resize(processCount);
      processCount = GetConsoleProcessList(&processList[0], processList.size());
  }
  FreeConsole();

  Napi::Array result = Napi::Array::New(env);
  for (DWORD i = 0; i < processCount; i++) {
    (result).Set(i, Napi::Number::New(env, processList[i]));
  }
  return result;
}

extern "C" void init(Napi::Object target) {
  Napi::HandleScope scope(env);
  exports.Set(Napi::String::New(env, "getConsoleProcessList"), Napi::Function::New(env, ApiConsoleProcessList));
};

NODE_API_MODULE(pty, init);
