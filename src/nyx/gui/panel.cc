#include "nyx/gui/panel.h"

#include "nyx/env.h"
#include "nyx/gui/widget_manager.h"
#include "nyx/isolate_data.h"

namespace nyx {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::ObjectTemplate;
using v8::String;
using v8::Value;

PanelWidget::PanelWidget(Realm* realm, Local<Object> object, const std::string& title, ImGuiWindowFlags flags)
    : Widget(realm, object), title_(title), flags_(flags) {}

PanelWidget::~PanelWidget() {
  if (canvas_) canvas_->MakeWeak();
}

void PanelWidget::Initialize(IsolateData* isolate_data, Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();
  Local<FunctionTemplate> tmpl = FunctionTemplate::New(isolate, New);
  tmpl->InstanceTemplate()->SetInternalFieldCount(BaseObject::kInternalFieldCount);
  tmpl->Inherit(Widget::GetConstructorTemplate(isolate_data));

  SetProtoProperty(isolate, tmpl, "open", GetOpen, SetOpen);
  SetProtoProperty(isolate, tmpl, "title", GetTitle, SetTitle);
  SetProtoProperty(isolate, tmpl, "flags", GetFlags, SetFlags);
  SetProtoProperty(isolate, tmpl, "visible", GetVisible, nullptr);
  SetProtoProperty(isolate, tmpl, "canvas", GetCanvas, nullptr);

  tmpl->SetClassName(FixedOneByteString(isolate, "Panel"));
  target->Set(FixedOneByteString(isolate, "Panel"), tmpl);
}

void PanelWidget::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Environment* env = Environment::GetCurrent(isolate);
  Local<Context> context = env->context();

  Utf8Value title(isolate, args[0]);
  ImGuiWindowFlags flags =
      args.Length() > 1 ? static_cast<ImGuiWindowFlags>(args[1]->Uint32Value(context).FromMaybe(0)) : 0;

  PanelWidget* panel = new PanelWidget(env->principal_realm(), args.This(), *title, flags);
  if (env->widget_manager()) {
    env->widget_manager()->AddRoot(panel);
  }
}

void PanelWidget::GetOpen(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) args.GetReturnValue().Set(self->open());
}

void PanelWidget::SetOpen(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) self->set_open(args[0]->BooleanValue(args.GetIsolate()));
}

void PanelWidget::GetTitle(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) {
    Isolate* isolate = args.GetIsolate();
    args.GetReturnValue().Set(OneByteString(isolate, self->title().c_str()));
  }
}

void PanelWidget::SetTitle(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) {
    Utf8Value title(args.GetIsolate(), args[0]);
    self->set_title(*title);
  }
}

void PanelWidget::GetFlags(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) args.GetReturnValue().Set(static_cast<uint32_t>(self->flags()));
}

void PanelWidget::SetFlags(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) {
    Local<Context> ctx = args.GetIsolate()->GetCurrentContext();
    self->set_flags(static_cast<ImGuiWindowFlags>(args[0]->Uint32Value(ctx).FromMaybe(0)));
  }
}

void PanelWidget::GetVisible(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (self) args.GetReturnValue().Set(self->panel_visible());
}

void PanelWidget::GetCanvas(const FunctionCallbackInfo<Value>& args) {
  PanelWidget* self = BaseObject::Unwrap<PanelWidget>(args.This());
  if (!self) return;

  if (!self->canvas_) {
    Environment* env = self->env();
    Local<Context> ctx = env->context();
    Local<ObjectTemplate> inst =
        env->isolate_data()->canvas_constructor_template()->InstanceTemplate();
    Local<Object> obj = inst->NewInstance(ctx).ToLocalChecked();
    self->canvas_ = new Canvas(env->principal_realm(), obj);
  }

  args.GetReturnValue().Set(self->canvas_->object());
}

void PanelWidget::Render() {
  if (!open_) {
    panel_visible_ = false;
    return;
  }

  bool was_open = open_;
  panel_visible_ = ImGui::Begin(title_.c_str(), &open_, flags_);
  if (panel_visible_) {
    RenderChildren();
    if (canvas_) {
      ImVec2 wpos = ImGui::GetWindowPos();
      ImVec2 cmin = ImGui::GetWindowContentRegionMin();
      ImVec2 offset(wpos.x + cmin.x, wpos.y + cmin.y);
      canvas_->Render(ImGui::GetWindowDrawList(), offset);
    }
  }
  ImGui::End();

  if (was_open && !open_) {
    EmitEvent("close");
  }
}

}  // namespace nyx
