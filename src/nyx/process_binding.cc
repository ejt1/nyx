#include "nyx/process_binding.h"

#include <uv.h>

#include "nyx/env.h"
#include "nyx/errors.h"
#include "nyx/util.h"

namespace nyx {

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::ObjectTemplate;
using v8::String;
using v8::Value;

static void Cwd(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  char buffer[4096];
  size_t size = sizeof(buffer);
  int result = uv_cwd(buffer, &size);

  if (result != 0) {
    THROW_ERR_OPERATION_FAILED(isolate, uv_strerror(result));
    return;
  }

  Local<String> cwd = OneByteString(isolate, buffer, static_cast<int>(size));
  args.GetReturnValue().Set(cwd);
}

static void Chdir(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 1 || !args[0]->IsString()) {
    THROW_ERR_INVALID_ARG_TYPE(isolate, "path must be a string");
    return;
  }

  String::Utf8Value path(isolate, args[0]);
  int result = uv_chdir(*path);

  if (result != 0) {
    THROW_ERR_OPERATION_FAILED(isolate, uv_strerror(result));
    return;
  }

  args.GetReturnValue().SetUndefined();
}

static void ScriptsRoot(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);

  const std::string& root = env->scripts_root();
  if (root.empty()) {
    args.GetReturnValue().SetUndefined();
    return;
  }

  Local<String> result = OneByteString(isolate, root.c_str(), static_cast<int>(root.size()));
  args.GetReturnValue().Set(result);
}

static void SetScriptsRoot(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  if (args.Length() < 1 || !args[0]->IsString()) {
    THROW_ERR_INVALID_ARG_TYPE(isolate, "path must be a string");
    return;
  }

  Environment* env = Environment::GetCurrent(context);
  String::Utf8Value path(isolate, args[0]);
  env->set_scripts_root(std::string(*path, path.length()));

  args.GetReturnValue().SetUndefined();
}

static void NyxCwd(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Environment* env = Environment::GetCurrent(isolate);

  const std::string& cwd = env->nyx_cwd();
  if (cwd.empty()) {
    args.GetReturnValue().SetUndefined();
    return;
  }

  Local<String> result = OneByteString(isolate, cwd.c_str(), static_cast<int>(cwd.size()));
  args.GetReturnValue().Set(result);
}

static void CreatePerIsolateProperties(IsolateData* isolate_data, Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();

  SetMethod(isolate, target, "cwd", Cwd);
  SetMethod(isolate, target, "chdir", Chdir);
  SetMethod(isolate, target, "nyxCwd", NyxCwd);
  SetMethod(isolate, target, "scriptsRoot", ScriptsRoot);
  SetMethod(isolate, target, "setScriptsRoot", SetScriptsRoot);
}

static void CreatePerContextProperties(Local<Object> target, Local<Context> context) {}

NYX_BINDING_PER_ISOLATE_INIT(process, CreatePerIsolateProperties)
NYX_BINDING_CONTEXT_AWARE(process, CreatePerContextProperties)

}  // namespace nyx
