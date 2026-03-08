#include "nyx/nyx.h"

#include <uv.h>

#include <atomic>
#include <memory>

#include <libplatform/libplatform.h>

#include "nyx/builtins.h"
#include "nyx/gui/widget_manager.h"
#include "nyx/imgui_draw_context.h"
#include "nyx/nyx_imgui.h"
#include "nyx/util.h"

namespace nyx {

using v8::ArrayBuffer;
using v8::Context;
using v8::EscapableHandleScope;
using v8::FunctionCallback;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Object;
using v8::ObjectTemplate;
using v8::Script;
using v8::String;
using v8::TryCatch;
using v8::Value;

static std::unique_ptr<v8::Platform> platform_;
static std::atomic<uv_async_t*> shutdown_handle_{nullptr};
static std::atomic<bool> running_{false};
static std::atomic<bool> restart_requested_{false};
static std::string nyx_cwd_;

static FILE* g_stdout_{stdout};
static FILE* g_stderr_{stderr};

void SetStdout(FILE* stream) {
  g_stdout_ = stream ? stream : stdout;
}
void SetStderr(FILE* stream) {
  g_stderr_ = stream ? stream : stderr;
}
FILE* GetStdout() {
  return g_stdout_;
}
FILE* GetStderr() {
  return g_stderr_;
}

static void SpinEventLoop(Environment* env) {
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env->context();
  Context::Scope context_scope(context);

  ImGuiDrawContext* draw_ctx = env->draw_context();

  if (draw_ctx) {
    draw_ctx->BeginFrame();
  }

  bool more = true;
  while (more) {
    isolate->PerformMicrotaskCheckpoint();
    more = uv_run(env->event_loop(), UV_RUN_ONCE) != 0;

    if (isolate->HasPendingBackgroundTasks()) {
      more = true;
    }

    if (env->widget_manager()) {
      env->widget_manager()->UpdateAll();
      env->widget_manager()->RenderAll();
    }

    if (draw_ctx) {
      draw_ctx->EndFrame();
      draw_ctx->BeginFrame();
    }
  }

  if (draw_ctx && draw_ctx->frame_active()) {
    draw_ctx->EndFrame();
  }
}

static void CloseWalkCallback(uv_handle_t* handle, void* arg) {
  if (!uv_is_closing(handle)) {
    uv_close(handle, nullptr);
  }
}

static void CloseEventLoop(uv_loop_t* loop) {
  uv_walk(loop, CloseWalkCallback, nullptr);

  while (uv_loop_alive(loop)) {
    uv_run(loop, UV_RUN_ONCE);
  }
}

static void OnShutdownSignal(uv_async_t* handle) {
  shutdown_handle_.store(nullptr, std::memory_order_release);
  uv_loop_t* loop = handle->loop;
  uv_walk(loop, CloseWalkCallback, nullptr);
}

void Initialize() {
  platform_ = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform_.get());
  v8::V8::Initialize();
}

int Start(NyxImGui* nyx_imgui, GameLock* game_lock) {
  running_.store(true, std::memory_order_release);

  do {
    restart_requested_.store(false, std::memory_order_release);

    uv_loop_t event_loop;
    int uv_err = uv_loop_init(&event_loop);
    if (uv_err != 0) {
      fprintf(stderr, "Failed to initialize event loop: %s\n", uv_strerror(uv_err));
      running_.store(false, std::memory_order_release);
      return 1;
    }

    uv_async_t shutdown_async;
    uv_async_init(&event_loop, &shutdown_async, OnShutdownSignal);
    shutdown_handle_.store(&shutdown_async, std::memory_order_release);

    RegisterBuiltinBindings();

    {
      Isolate::CreateParams create_params;
      create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
      Isolate* isolate = Isolate::New(create_params);
      // fixme: isolate data is created here but it should really be created by Environment
      // with the current order CreateProperties does not have access to Environment which it should
      //  -> pass event_loop to Environment and move IsolateData ownership there
      IsolateData* isolate_data = new IsolateData(isolate, &event_loop);

      {
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Environment env(isolate_data, isolate, nyx_cwd_, nyx_imgui, game_lock);
        Realm* realm = env.principal_realm();
        realm->ExecuteBootstrapper("internal/main/run_nyx");
        SpinEventLoop(&env);

        nyx_imgui->ClearDrawData();
        CloseEventLoop(&event_loop);
      }

      delete isolate_data;
      isolate->Dispose();
      delete create_params.array_buffer_allocator;
    }

    shutdown_handle_.store(nullptr, std::memory_order_release);

    uv_loop_close(&event_loop);

  } while (restart_requested_.load(std::memory_order_acquire));

  running_.store(false, std::memory_order_release);
  return 0;
}

void Shutdown() {
  uv_async_t* handle = shutdown_handle_.load(std::memory_order_acquire);
  if (handle) {
    uv_async_send(handle);
  }
}

void Restart() {
  restart_requested_.store(true, std::memory_order_release);
  Shutdown();
}

void Teardown() {
  uv_library_shutdown();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  platform_.reset();
}

bool IsRunning() {
  return running_.load(std::memory_order_acquire);
}

void SetCwd(const std::string& path) {
  nyx_cwd_ = path;
}

}  // namespace nyx
