#pragma once

#include "nyx/builtins.h"
#include "nyx/imgui_draw_context.h"
#include "nyx/isolate_data.h"
#include "nyx/realm.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace nyx {

class GameLock;
class ModuleWrap;
class NyxImGui;
class TimerRegistry;
class WidgetManager;

enum ContextEmbedderIndex {
  kEnvironment = 1 << 0,
  kRealm = 1 << 1,
};

class Environment {
 public:
  Environment(IsolateData* isolate_data,
              v8::Isolate* isolate,
              const std::string_view nyx_cwd,
              NyxImGui* nyx_imgui,
              GameLock* game_lock = nullptr);
  ~Environment();

  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;
  Environment(Environment&&) = delete;
  Environment& operator=(Environment&&) = delete;

  static Environment* GetCurrent(v8::Isolate* isolate);
  static Environment* GetCurrent(v8::Local<v8::Context> context);
  static Environment* GetCurrent(const v8::FunctionCallbackInfo<v8::Value>& args);
  template <typename T>
  inline static Environment* GetCurrent(const v8::PropertyCallbackInfo<T>& args) {
    return GetCurrent(args.GetIsolate()->GetCurrentContext());
  }

  IsolateData* isolate_data() const { return isolate_data_; }
  v8::Isolate* isolate() const { return isolate_; }
  BuiltinLoader* builtin_loader() { return &builtin_loader_; }
  uv_loop_t* event_loop() const { return isolate_data_->event_loop(); }

  PrincipalRealm* principal_realm() const { return principal_realm_.get(); }
  v8::Local<v8::Context> context() const;

  NyxImGui* nyx_imgui() const { return nyx_imgui_; }
  ImGuiDrawContext* draw_context() const { return draw_context_.get(); }
  GameLock* game_lock() const { return game_lock_; }
  WidgetManager* widget_manager() const { return widget_manager_.get(); }
  TimerRegistry& timer_registry() { return *timer_registry_; }

  void RegisterModule(int identity_hash, ModuleWrap* wrap);
  void UnregisterModule(int identity_hash);
  ModuleWrap* GetModuleWrap(int identity_hash) const;
  ModuleWrap* GetModuleWrap(v8::Local<v8::Module> module) const;

  const std::string& nyx_cwd() const { return nyx_cwd_; }
  const std::string& scripts_root() const { return scripts_root_; }
  void set_scripts_root(const std::string& root) { scripts_root_ = root; }

  // Primitive values are shared across realms.
  // The getters simply proxy to the per-isolate primitive.
#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName) v8::Local<TypeName> PropertyName() const;
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
  PER_ISOLATE_SYMBOL_PROPERTIES(VY)
  PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VS
#undef VY
#undef VP

#define V(PropertyName, TypeName)                                                                                      \
  v8::Local<TypeName> PropertyName() const;                                                                            \
  void set_##PropertyName(v8::Local<TypeName> value);
  PER_ISOLATE_TEMPLATE_PROPERTIES(V)
#undef V

  v8::Global<v8::Module> temporary_required_module_facade_original;

 private:
  IsolateData* isolate_data_;
  v8::Isolate* isolate_;
  NyxImGui* nyx_imgui_;
  GameLock* game_lock_;
  BuiltinLoader builtin_loader_;
  std::unique_ptr<TimerRegistry> timer_registry_;
  std::unique_ptr<PrincipalRealm> principal_realm_;
  std::unique_ptr<ImGuiDrawContext> draw_context_;
  std::unique_ptr<WidgetManager> widget_manager_;
  std::unordered_map<int, ModuleWrap*> module_registry_;
  std::string nyx_cwd_;
  std::string scripts_root_;
};

}  // namespace nyx
