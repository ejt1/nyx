#pragma once

#include "nyx/base_object.h"
#include "nyx/util.h"

#include <imgui.h>
#include <string>
#include <vector>

namespace nyx {

class IsolateData;

class Canvas : public BaseObject {
 public:
  struct Primitive {
    enum class Type { Line, Rect, RectFilled, Circle, CircleFilled, Text };
    Type type;
    ImVec2 p1;
    ImVec2 p2;
    float radius = 0.f;
    ImU32 color = IM_COL32_WHITE;
    float thickness = 1.f;
    float rounding = 0.f;
    ImDrawFlags draw_flags = 0;
    int segments = 0;
    std::string text;
    float font_size = 0.f;
  };

  Canvas(Realm* realm, v8::Local<v8::Object> object);
  ~Canvas() override = default;

  static void Initialize(IsolateData* isolate_data);
  static void CreatePerContextProperties(v8::Local<v8::Object> target, v8::Local<v8::Context> context);

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);

  static void AddLine(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void AddRect(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void AddRectFilled(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void AddCircle(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void AddCircleFilled(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void AddText(const v8::FunctionCallbackInfo<v8::Value>& args);

  static void Remove(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void Clear(const v8::FunctionCallbackInfo<v8::Value>& args);

  void Render(ImDrawList* draw_list, ImVec2 offset = ImVec2(0.0f, 0.0f));

 private:
  std::unordered_map<std::string, Primitive> primitives_;
};

}  // namespace nyx
