#pragma once

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <string>

namespace dolos {

// custom spdlog sink that writes to a named pipe
template <typename Mutex>
class pipe_sink : public spdlog::sinks::base_sink<Mutex> {
 public:
  explicit pipe_sink(const std::string& pipe_name = "\\\\.\\pipe\\dolos_log")
      : pipe_name_(pipe_name), pipe_(INVALID_HANDLE_VALUE), connected_(false) {}

  ~pipe_sink() override { disconnect(); }

  bool connect() {
    if (connected_) {
      return true;
    }

    pipe_ = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe_ == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_BUSY) {
        if (WaitNamedPipeA(pipe_name_.c_str(), 5000)) {
          pipe_ = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
      }
    }

    if (pipe_ == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
      return false;
    }

    connected_ = true;
    return true;
  }

  void disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
    connected_ = false;
  }

  bool is_connected() const { return connected_; }

  // Non-blocking read of a single command sent by the injector.
  // Returns true and sets `cmd` (trimmed) when a message is available.
  bool PollCommand(std::string& cmd) {
    if (!connected_ || pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD available = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
      return false;
    }
    char buf[256]{};
    DWORD read = 0;
    if (!ReadFile(pipe_, buf, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buf) - 1)),
                  &read, nullptr) || read == 0) {
      return false;
    }
    cmd.assign(buf, read);
    while (!cmd.empty() &&
           (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
      cmd.pop_back();
    }
    return !cmd.empty();
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (!connected_ || pipe_ == INVALID_HANDLE_VALUE) {
      return;
    }

    spdlog::memory_buf_t formatted;
    spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);

    DWORD written = 0;
    BOOL success = WriteFile(pipe_, formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr);

    if (!success) {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
        connected_ = false;
      }
    }
  }

  void flush_() override {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(pipe_);
    }
  }

 private:
  std::string pipe_name_;
  HANDLE pipe_;
  std::atomic<bool> connected_;
};

using pipe_sink_mt = pipe_sink<std::mutex>;
using pipe_sink_st = pipe_sink<spdlog::details::null_mutex>;

bool InitializePipeLog(const std::string& logger_name = "dolos");
void ShutdownPipeLog();

bool IsPipeLogConnected();

// Non-blocking poll: returns true and fills `cmd` when the injector has sent a command.
bool PollPipeCommand(std::string& cmd);

}  // namespace dolos

#define PIPE_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define PIPE_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define PIPE_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define PIPE_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define PIPE_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define PIPE_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

#define PIPE_LOG(...) SPDLOG_INFO(__VA_ARGS__)
