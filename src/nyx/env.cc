#include "nyx/env.h"

#include "nyx/gui/widget_manager.h"
#include "nyx/module_wrap.h"
#include "nyx/nyx_imgui.h"
#include "nyx/timers.h"

namespace nyx {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Value;

Environment::Environment(IsolateData* isolate_data,
                         v8::Isolate* isolate,
                         const std::string_view nyx_cwd,
                         NyxImGui* nyx_imgui,
                         GameLock* game_lock)
    : isolate_data_(isolate_data),
      isolate_(isolate),
      nyx_imgui_(nyx_imgui),
      game_lock_(game_lock),
      nyx_cwd_(nyx_cwd) {
  Isolate::Scope isolate_scope(isolate_);

  // Must exist before bootstrapping so timer binding callbacks can access it.
  timer_registry_ = std::make_unique<TimerRegistry>(this);

  if (nyx_imgui_) {
    draw_context_ = std::make_unique<ImGuiDrawContext>(nyx_imgui_);
    widget_manager_ = std::make_unique<WidgetManager>();
  }

  principal_realm_ = std::make_unique<PrincipalRealm>(this);
  principal_realm_->RunBootstrapping();  // should check IsEmpty to handle error, how though in constructor?
}

Environment::~Environment() {
  timer_registry_->CloseAll();
  widget_manager_.reset();
  draw_context_.reset();
  principal_realm_.reset();
}

Environment* Environment::GetCurrent(v8::Isolate* isolate) {
  if (!isolate->InContext()) [[unlikely]] {
    return nullptr;
  }
  return GetCurrent(isolate->GetCurrentContext());
}

Environment* Environment::GetCurrent(Local<Context> context) {
  return static_cast<Environment*>(context->GetAlignedPointerFromEmbedderData(ContextEmbedderIndex::kEnvironment));
}

Environment* Environment::GetCurrent(const FunctionCallbackInfo<Value>& args) {
  return GetCurrent(args.GetIsolate());
}

Local<Context> Environment::context() const {
  return principal_realm_->context();
}

void Environment::RegisterModule(int identity_hash, ModuleWrap* wrap) {
  module_registry_[identity_hash] = wrap;
}

void Environment::UnregisterModule(int identity_hash) {
  module_registry_.erase(identity_hash);
}

ModuleWrap* Environment::GetModuleWrap(int identity_hash) const {
  auto it = module_registry_.find(identity_hash);
  if (it == module_registry_.end()) {
    return nullptr;
  }
  return it->second;
}

ModuleWrap* Environment::GetModuleWrap(v8::Local<v8::Module> module) const {
  return GetModuleWrap(module->GetIdentityHash());
}

#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName)                                                                                      \
  v8::Local<TypeName> Environment::PropertyName() const {                                                              \
    return isolate_data()->PropertyName();                                                                             \
  }
PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
PER_ISOLATE_SYMBOL_PROPERTIES(VY)
PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VS
#undef VY
#undef VP

#define V(PropertyName, TypeName)                                                                                      \
  v8::Local<TypeName> Environment::PropertyName() const {                                                              \
    return isolate_data()->PropertyName();                                                                             \
  }                                                                                                                    \
  void Environment::set_##PropertyName(v8::Local<TypeName> value) {                                                    \
    isolate_data()->set_##PropertyName(value);                                                                         \
  }
PER_ISOLATE_TEMPLATE_PROPERTIES(V)
#undef V

}  // namespace nyx
