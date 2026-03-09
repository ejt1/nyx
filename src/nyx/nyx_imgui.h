#pragma once

#include "nyx/imgui_draw_data_store.h"
#include "nyx/imgui_input_event.h"

#include <imgui.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace nyx {

class NyxImGui {
 public:
  NyxImGui();
  ~NyxImGui();

  NyxImGui(const NyxImGui&) = delete;
  NyxImGui& operator=(const NyxImGui&) = delete;

  ImGuiDrawDataStore* foreground() { return &foreground_; }
  ImGuiDrawDataStore* background() { return &background_; }

  // Clear all draw data from both stores (thread-safe).
  void ClearDrawData();

  void PushInputEvent(const ImGuiInputEvent& event);
  void PushInputEvents(const ImGuiInputEvent* events, int count);
  std::vector<ImGuiInputEvent> DrainInputEvents();
  bool HasPendingInputEvents();

  // Shared font atlas for all ImGui contexts
  ImFontAtlas* font_atlas() { return font_atlas_; }

  void set_game_context(ImGuiContext* ctx) { game_context_ = ctx; }
  ImGuiContext* game_context() const { return game_context_; }
  void EnsureGameContext();

  void SetWantCapture(bool mouse, bool keyboard);
  bool WantCaptureMouse() const;
  bool WantCaptureKeyboard() const;

  // Controls whether imgui draw data is rendered. When false the renderer skips
  // all imgui submission so background instances have no visible overlay.
  void set_visible(bool v) { visible_.store(v, std::memory_order_relaxed); }
  bool visible() const { return visible_.load(std::memory_order_relaxed); }

 private:
  ImGuiDrawDataStore foreground_;
  ImGuiDrawDataStore background_;
  std::vector<ImGuiInputEvent> input_queue_;
  std::mutex input_mutex_;
  ImFontAtlas* font_atlas_;
  ImGuiContext* game_context_ = nullptr;
  std::atomic<bool> want_capture_mouse_{false};
  std::atomic<bool> want_capture_keyboard_{false};
  std::atomic<bool> visible_{true};
};

}  // namespace nyx
