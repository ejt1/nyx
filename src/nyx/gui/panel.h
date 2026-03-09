#pragma once

#include "nyx/gui/canvas.h"
#include "nyx/gui/widget.h"

/** Implements:
 * + ImGui::Begin
 * + ImGui::End
 * - ImGui::IsWindowAppearing
 * - ImGui::IsWindowCollapsed
 * - ImGui::IsWindowFocused
 * - ImGui::IsWindowHovered
 * + ImGui::GetWindowDrawList (using canvas)
 * - ImGui::GetWindowPos
 * - ImGui::GetWindowSize
 * - ImGui::GetWindowWidth
 * - ImGui::GetWindowHeight
 * - ImGui::SetNextWindowPos
 * - ImGui::SetNextWindowSize
 * - ImGui::SetNextWindowSizeConstraints
 * - ImGui::SetNextWindowContentSize
 * - ImGui::SetNextWindowCollapsed
 * - ImGui::SetNextWindowFocus
 * - ImGui::SetNextWindowScroll
 * - ImGui::SetNextWindowBgAlpha
 * - ImGui::SetWindowPos
 * - ImGui::SetWindowSize
 * - ImGui::SetWindowCollapsed
 * - ImGui::SetWindowFocus
 * - ImGui::SetWindowFontScale
 * - ImGui::GetContentRegionAvail
 * - ImGui::GetContentRegionMax
 * - ImGui::GetWindowContentRegionMin
 * - ImGui::GetWindowContentRegionMax
 * - ImGui::GetScrollX
 * - ImGui::GetScrollY
 * - ImGui::SetScrollX
 * - ImGui::SetScrollY
 * - ImGui::GetScrollMaxX
 * - ImGui::GetScrollMaxY
 * - ImGui::SetScrollHereX
 * - ImGui::SetScrollHereY
 * - ImGui::SetScrollFromPosX
 * - ImGui::SetScrollFromPosY
 */

namespace nyx {

class PanelWidget : public Widget {
 public:
  PanelWidget(Realm* realm, v8::Local<v8::Object> object, const std::string& title, ImGuiWindowFlags flags);
  ~PanelWidget();

  static void Initialize(IsolateData* isolate_data, v8::Local<v8::ObjectTemplate> target);

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  static void GetOpen(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void SetOpen(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void GetTitle(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void SetTitle(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void GetFlags(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void SetFlags(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void GetVisible(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void GetCanvas(const v8::FunctionCallbackInfo<v8::Value>& args);

  void Render() override;
  bool IsContainer() const override { return true; }

  const std::string& title() const { return title_; }
  void set_title(const std::string& t) { title_ = t; }
  bool open() const { return open_; }
  void set_open(bool o) { open_ = o; }
  bool panel_visible() const { return panel_visible_; }
  ImGuiWindowFlags flags() const { return flags_; }
  void set_flags(ImGuiWindowFlags f) { flags_ = f; }
  Canvas* canvas() const { return canvas_; }

 private:
  std::string title_;
  bool open_ = true;
  bool panel_visible_ = false;
  ImGuiWindowFlags flags_ = 0;
  Canvas* canvas_ = nullptr;
};

}  // namespace nyx
