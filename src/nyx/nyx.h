#pragma once

#include "nyx/env.h"
#include "nyx/isolate_data.h"

namespace nyx {

class GameLock;
class NyxImGui;

// Custom I/O handles for Nyx output. Default to the process stdout/stderr.
// Set before Start() to redirect all console output (console.log, console.error,
// exception reports, bootstrap debug logs, etc.) to custom streams.
void SetStdout(FILE* stream);
void SetStderr(FILE* stream);
FILE* GetStdout();
FILE* GetStderr();

// Call once at startup, initializes V8 platform.
void Initialize();

// Blocks until Shutdown() is called. Safe to call repeatedly after Initialize().
int Start(NyxImGui* nyx_imgui = nullptr, GameLock* game_lock = nullptr);

// Thread-safe. Signals the running instance to stop.
void Shutdown();

// Thread-safe. Tears down the current isolate/env and spins up a fresh one
// without returning from Start(). Safe to call from anywhere.
void Restart();

// Call once at final cleanup, disposes V8 platform.
void Teardown();

// Check if nyx is currently running (Start has been called and hasn't returned).
bool IsRunning();

void SetCwd(const std::string& path);

}  // namespace nyx
