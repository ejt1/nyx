#include "dolos/dolos.h"

#include "dolos/backend/d3d12_renderer.h"
#include "dolos/backend/win32_window.h"
#include "dolos/dolos_binding.h"
#include "dolos/pipe_log.h"
#include "dolos_builtins.h"

#include <nyx/game_lock.h>
#include <nyx/nyx.h>
#include <nyx/nyx_imgui.h>

#include <MinHook.h>

#include <Shlwapi.h>
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <thread>

namespace dolos {

static std::string module_cwd_;

// redirects nyx stdout/stderr through pipe logging.
struct NyxPipeRedirect {
  int stdout_read_fd = -1;
  int stderr_read_fd = -1;
  FILE* stdout_write = nullptr;
  FILE* stderr_write = nullptr;
  std::thread stdout_thread;
  std::thread stderr_thread;
  std::atomic<bool> running{false};

  bool Start() {
    int stdout_fds[2], stderr_fds[2];

    if (_pipe(stdout_fds, 8192, _O_BINARY) != 0) return false;
    if (_pipe(stderr_fds, 8192, _O_BINARY) != 0) {
      _close(stdout_fds[0]);
      _close(stdout_fds[1]);
      return false;
    }

    stdout_read_fd = stdout_fds[0];
    stderr_read_fd = stderr_fds[0];
    stdout_write = _fdopen(stdout_fds[1], "w");
    stderr_write = _fdopen(stderr_fds[1], "w");

    if (!stdout_write || !stderr_write) {
      Cleanup();
      return false;
    }

    setvbuf(stdout_write, nullptr, _IONBF, 0);
    setvbuf(stderr_write, nullptr, _IONBF, 0);

    nyx::SetStdout(stdout_write);
    nyx::SetStderr(stderr_write);

    running = true;

    stdout_thread = std::thread([this]() { ReaderLoop(stdout_read_fd, false); });
    stderr_thread = std::thread([this]() { ReaderLoop(stderr_read_fd, true); });

    return true;
  }

  void Stop() {
    running = false;

    nyx::SetStdout(nullptr);
    nyx::SetStderr(nullptr);

    if (stdout_write) {
      fclose(stdout_write);
      stdout_write = nullptr;
    }
    if (stderr_write) {
      fclose(stderr_write);
      stderr_write = nullptr;
    }

    if (stdout_thread.joinable()) stdout_thread.join();
    if (stderr_thread.joinable()) stderr_thread.join();

    if (stdout_read_fd != -1) {
      _close(stdout_read_fd);
      stdout_read_fd = -1;
    }
    if (stderr_read_fd != -1) {
      _close(stderr_read_fd);
      stderr_read_fd = -1;
    }
  }

 private:
  void ReaderLoop(int fd, bool is_stderr) {
    char buf[4096];
    std::string line;

    while (running) {
      int n = _read(fd, buf, sizeof(buf));
      if (n <= 0) break;

      for (int i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (is_stderr) {
            PIPE_LOG_ERROR("[nyx] {}", line);
          } else {
            PIPE_LOG("[nyx] {}", line);
          }
          line.clear();
        } else {
          line += buf[i];
        }
      }
    }

    if (!line.empty()) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (is_stderr) {
        PIPE_LOG_ERROR("[nyx] {}", line);
      } else {
        PIPE_LOG("[nyx] {}", line);
      }
    }
  }

  void Cleanup() {
    if (stdout_write) {
      fclose(stdout_write);
      stdout_write = nullptr;
    } else if (stdout_read_fd != -1) {
      _close(stdout_read_fd);
    }
    if (stderr_write) {
      fclose(stderr_write);
      stderr_write = nullptr;
    } else if (stderr_read_fd != -1) {
      _close(stderr_read_fd);
    }
    stdout_read_fd = -1;
    stderr_read_fd = -1;
  }
};

DWORD CALLBACK ThreadEntry(LPVOID lpParameter) {
  HMODULE hModule = static_cast<HMODULE>(lpParameter);

#if defined(_DEBUG)
  spdlog::set_level(spdlog::level::debug);
#endif

  if (InitializePipeLog()) {
    PIPE_LOG("[dolos] Connected to log server");
  }

  MH_Initialize();

  if (lpParameter) {
    HMODULE hModule = static_cast<HMODULE>(lpParameter);
    char cwd[MAX_PATH]{};
    GetModuleFileName(hModule, cwd, MAX_PATH);
    PathRemoveFileSpecA(cwd);
    module_cwd_ = cwd;
  }

  Game* game = Game::Create();
  PIPE_LOG("[dolos] Game created");

  if (!game->OnInitialize()) {
    PIPE_LOG_WARN("[dolos] Game::OnInitialize failed");
  }

  nyx::SetCwd(module_cwd_);
  nyx::RegisterBinding("dolos", InitDolosBinding);
  dolos_builtins::RegisterBuiltins();

  nyx::NyxImGui nyx_imgui;
  nyx::GameLock game_lock;

  PIPE_LOG("[dolos] Initializing hooks...");
  Win32Window window(game, &nyx_imgui, &game_lock);
  D3D12Renderer renderer(&window, &nyx_imgui);

  ImGuiContext* game_ctx = ImGui::CreateContext(nyx_imgui.font_atlas());
  nyx_imgui.set_game_context(game_ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.FrameBorderSize = 0.0f;

  nyx::Initialize();

  NyxPipeRedirect pipe_redirect;
  if (IsPipeLogConnected()) {
    if (pipe_redirect.Start()) {
      PIPE_LOG("[dolos] Nyx console output redirected to pipe log");
    }
  }

  if (renderer.Initialize() && window.Initialize()) {
    PIPE_LOG("[dolos] Starting nyx runtime...");
    nyx::Start(&nyx_imgui, &game_lock);
  }

  pipe_redirect.Stop();

  PIPE_LOG("[dolos] Shutting down...");
  nyx::Teardown();
  window.Shutdown();
  renderer.Shutdown();

  if (ImGui::GetCurrentContext()) {
    ImGui::DestroyContext();
  }

  MH_Uninitialize();

  game->OnShutdown();
  delete game;

  ShutdownPipeLog();

  if (hModule) {
    FreeLibraryAndExitThread(hModule, 0);
  }
  return 0;
}

HANDLE SpawnWorkerThread() {
  return CreateThread(nullptr, 0, ThreadEntry, nullptr, 0, nullptr);
}

std::string get_module_cwd() {
  return module_cwd_;
}

}  // namespace dolos
