#include "nyx/gui/widget.h"

#include "nyx/env.h"
#include "nyx/gui/widget_manager.h"

#include <algorithm>

namespace nyx {

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::String;
using v8::Value;

Widget::Widget(Realm* realm, Local<Object> object, std::string_view label)
    : BaseObject(realm, object), label_(label) {
  ClearWeak();  // prevent GC by default; destroy() makes it weak again
}

Widget::~Widget() {
  ClearChildren();
}

void Widget::Add(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  Widget* child;
  ASSIGN_OR_RETURN_UNWRAP(&child, args[0]);
  self->AddChild(child);
  args.GetReturnValue().Set(args[0]);
}

void Widget::Remove(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  Widget* child;
  ASSIGN_OR_RETURN_UNWRAP(&child, args[0]);
  self->RemoveChild(child);
}

void Widget::On(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  Isolate* isolate = args.GetIsolate();
  Utf8Value event(isolate, args[0]);
  if (args.Length() > 1 && args[1]->IsFunction()) {
    self->_On(*event, args[1].As<Function>());
  }
}

void Widget::Off(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  Isolate* isolate = args.GetIsolate();
  Utf8Value event(isolate, args[0]);
  if (!args[1]->IsFunction()) {
    isolate->ThrowError("Expected function");
    return;
  }
  Local<Function> fun(args[1].As<Function>());
  self->_Off(*event, fun);
}

void Widget::Destroy(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());

  // Remove from parent or widget manager
  if (self->parent()) {
    self->parent()->RemoveChild(self);
  } else {
    Environment* env = Environment::GetCurrent(args);
    if (env->widget_manager()) {
      env->widget_manager()->RemoveRoot(self);
    }
  }
  self->ClearChildren();
  self->MakeWeak();
}

void Widget::VisibleGetter(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  args.GetReturnValue().Set(self->visible());
}

void Widget::VisibleSetter(const FunctionCallbackInfo<Value>& args) {
  Widget* self;
  ASSIGN_OR_RETURN_UNWRAP(&self, args.This());
  self->set_visible(args[0]->BooleanValue(args.GetIsolate()));
}

void Widget::LabelGetter(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Widget* self = BaseObject::Unwrap<Widget>(args.This());
  if (self) {
    args.GetReturnValue().Set(String::NewFromUtf8(args.GetIsolate(), self->label().c_str()).ToLocalChecked());
  }
}

void Widget::LabelSetter(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Widget* self = BaseObject::Unwrap<Widget>(args.This());
  if (self) {
    Utf8Value label(args.GetIsolate(), args[0]);
    self->set_label(*label);
  }
}

Local<FunctionTemplate> Widget::GetConstructorTemplate(IsolateData* isolate_data) {
  Local<FunctionTemplate> tmpl = isolate_data->widget_constructor_template();
  if (tmpl.IsEmpty()) {
    Isolate* isolate = isolate_data->isolate();

    tmpl = FunctionTemplate::New(isolate, nullptr);
    tmpl->InstanceTemplate()->SetInternalFieldCount(BaseObject::kInternalFieldCount);
    tmpl->SetClassName(FixedOneByteString(isolate, "Widget"));

    SetProtoMethod(isolate, tmpl, "add", Add);
    SetProtoMethod(isolate, tmpl, "remove", Remove);
    SetProtoMethod(isolate, tmpl, "on", On);
    SetProtoMethod(isolate, tmpl, "off", Off);
    SetProtoMethod(isolate, tmpl, "destroy", Destroy);
    SetProtoProperty(isolate, tmpl, "visible", VisibleGetter, VisibleSetter);
    SetProtoProperty(isolate, tmpl, "label", LabelGetter, LabelSetter);

    isolate_data->set_widget_constructor_template(tmpl);
  }
  return tmpl;
}

void Widget::AddChild(Widget* child) {
  if (child->parent_) {
    child->parent_->RemoveChild(child);
  }
  child->parent_ = this;
  child->ClearWeak();
  children_.push_back(child);
}

void Widget::RemoveChild(Widget* child) {
  auto it = std::find(children_.begin(), children_.end(), child);
  if (it != children_.end()) {
    children_.erase(it);
    child->parent_ = nullptr;
    child->MakeWeak();
  }
}

void Widget::ClearChildren() {
  for (Widget* child : children_) {
    child->parent_ = nullptr;
    child->MakeWeak();
  }
  children_.clear();
}

void Widget::_On(const std::string& event, Local<Function> callback) {
  event_handlers_[event].emplace_back().Reset(isolate(), callback);
}

void Widget::_Off(const std::string& event, Local<Function> callback) {
  HandleScope scope(isolate());
  Local<Context> context = realm()->context();
  auto it = event_handlers_.find(event);
  if (it != event_handlers_.end()) {
    auto fn_it = it->second.begin();
    while (fn_it != it->second.end()) {
      Local<Function> fn = (*fn_it).Get(isolate());
      if (callback->Equals(context, fn).FromMaybe(false)) {
        (*fn_it).Reset();
        fn_it = it->second.erase(fn_it);
      } else {
        fn_it++;
      }
    }
  }
}

void Widget::UpdateChildren() {
  auto snapshot = children_;
  for (Widget* child : snapshot) {
    child->Update();
  }
}

void Widget::RenderChildren() {
  // Copy in case event handlers mutate the children vector
  auto snapshot = children_;
  for (Widget* child : snapshot) {
    if (child->visible_) {
      child->Render();
    }
  }
}

void Widget::EmitEvent(const std::string& event) {
  auto it = event_handlers_.find(event);
  if (it == event_handlers_.end()) return;

  Isolate* iso = isolate();
  HandleScope scope(iso);
  Local<Context> ctx = env()->context();
  for (const Global<Function>& it : it->second) {
    Local<Function> fn = it.Get(iso);
    if (fn.IsEmpty()) continue;

    TryCatchScope try_catch(iso);
    fn->Call(ctx, object(), 0, nullptr).IsEmpty();
  }
}

void Widget::EmitEvent(const std::string& event, Local<Value> arg) {
  auto it = event_handlers_.find(event);
  if (it == event_handlers_.end()) return;

  Isolate* iso = isolate();
  HandleScope scope(iso);
  Local<Context> ctx = env()->context();
  for (const Global<Function>& it : it->second) {
    Local<Function> fn = it.Get(iso);
    if (fn.IsEmpty()) continue;

    TryCatchScope try_catch(iso);
    Local<Value> args[] = {arg};
    fn->Call(ctx, object(), 1, args).IsEmpty();
  }
}

ChildWidget::ChildWidget(Realm* realm,
                         Local<Object> object,
                         const std::string& id,
                         float width,
                         float height,
                         ImGuiChildFlags child_flags,
                         ImGuiWindowFlags window_flags)
    : Widget(realm, object, id),
      width_(width),
      height_(height),
      child_flags_(child_flags),
      window_flags_(window_flags) {}

void ChildWidget::Initialize(IsolateData* isolate_data, Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();
  Local<FunctionTemplate> tmpl = FunctionTemplate::New(isolate, New);
  tmpl->InstanceTemplate()->SetInternalFieldCount(BaseObject::kInternalFieldCount);
  tmpl->Inherit(Widget::GetConstructorTemplate(isolate_data));

  tmpl->SetClassName(FixedOneByteString(isolate, "Child"));
  target->Set(FixedOneByteString(isolate, "Child"), tmpl);
}

void ChildWidget::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);
  Utf8Value id(isolate, args[0]);
  float width = args.Length() > 1 ? static_cast<float>(args[1]->NumberValue(context).FromMaybe(0.0)) : 0.0f;
  float height = args.Length() > 2 ? static_cast<float>(args[2]->NumberValue(context).FromMaybe(0.0)) : 0.0f;
  ImGuiChildFlags child_flags =
      args.Length() > 3 ? static_cast<ImGuiChildFlags>(args[3]->Uint32Value(context).FromMaybe(0)) : 0;
  ImGuiWindowFlags window_flags =
      args.Length() > 4 ? static_cast<ImGuiWindowFlags>(args[4]->Uint32Value(context).FromMaybe(0)) : 0;
  new ChildWidget(env->principal_realm(), args.This(), *id, width, height, child_flags, window_flags);
}

void ChildWidget::Render() {
  if (ImGui::BeginChild(label_.c_str(), ImVec2(width_, height_), child_flags_, window_flags_)) {
    RenderChildren();
  }
  ImGui::EndChild();
}

void DemoWindowWidget::Render() {
  bool show = true;
  ImGui::ShowDemoWindow(&show);
}

}  // namespace nyx
