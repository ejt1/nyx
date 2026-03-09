#include "dolos/pipe_log.h"

#include <spdlog/pattern_formatter.h>

namespace dolos {

static std::shared_ptr<pipe_sink_mt> g_pipe_sink;

bool InitializePipeLog(const std::string& logger_name) {
  const std::string pid_pipe_name = "\\\\.\\pipe\\dolos_log_" + std::to_string(GetCurrentProcessId());
  const std::string legacy_pipe_name = "\\\\.\\pipe\\dolos_log";

  g_pipe_sink = std::make_shared<pipe_sink_mt>(pid_pipe_name);
  bool connected = g_pipe_sink->connect();
  bool used_legacy_pipe = false;
  if (!connected) {
    g_pipe_sink = std::make_shared<pipe_sink_mt>(legacy_pipe_name);
    connected = g_pipe_sink->connect();
    used_legacy_pipe = connected;
  }

  auto logger = std::make_shared<spdlog::logger>(logger_name, g_pipe_sink);
  logger->set_pattern("[%l] %v");
  logger->set_level(spdlog::level::trace);
  logger->flush_on(spdlog::level::trace);
  spdlog::set_default_logger(logger);

  if (connected) {
    if (used_legacy_pipe) {
      SPDLOG_INFO("[dolos] Pipe log connected to legacy server {}", legacy_pipe_name);
    } else {
      SPDLOG_INFO("[dolos] Pipe log connected to pid server {}", pid_pipe_name);
    }
  } else {
    SPDLOG_WARN("[dolos] Pipe log server not available (tried {} and {})", pid_pipe_name, legacy_pipe_name);
  }

  return connected;
}

void ShutdownPipeLog() {
  if (auto logger = spdlog::default_logger()) {
    logger->flush();
    spdlog::drop(logger->name());
  }

  if (g_pipe_sink) {
    g_pipe_sink->disconnect();
    g_pipe_sink.reset();
  }
}

bool IsPipeLogConnected() {
  return g_pipe_sink && g_pipe_sink->is_connected();
}

bool PollPipeCommand(std::string& cmd) {
  if (!g_pipe_sink) return false;
  return g_pipe_sink->PollCommand(cmd);
}

}  // namespace dolos
