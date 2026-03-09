#include "nyx/gui/canvas.h"

#include "nyx/env.h"
#include "nyx/gui/widget_manager.h"
#include "nyx/isolate_data.h"
#include "nyx/realm.h"

namespace nyx {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::ObjectTemplate;
using v8::Value;

Canvas::Canvas(Realm* realm, Local<Object> object) : BaseObject(realm, object) {
  ClearWeak();
}

void Canvas::Initialize(IsolateData* isolate_data) {
  Isolate* isolate = isolate_data->isolate();
  Local<FunctionTemplate> tmpl = FunctionTemplate::New(isolate, Canvas::New);
  tmpl->SetClassName(FixedOneByteString(isolate, "Canvas"));
  tmpl->InstanceTemplate()->SetInternalFieldCount(BaseObject::kInternalFieldCount);

  SetProtoMethod(isolate, tmpl, "addLine", AddLine);
  SetProtoMethod(isolate, tmpl, "addRect", AddRect);
  SetProtoMethod(isolate, tmpl, "addRectFilled", AddRectFilled);
  SetProtoMethod(isolate, tmpl, "addCircle", AddCircle);
  SetProtoMethod(isolate, tmpl, "addCircleFilled", AddCircleFilled);
  SetProtoMethod(isolate, tmpl, "addText", AddText);

  SetProtoMethod(isolate, tmpl, "remove", Remove);
  SetProtoMethod(isolate, tmpl, "clear", Clear);

  isolate_data->set_canvas_constructor_template(tmpl);
}

void Canvas::CreatePerContextProperties(Local<Object> target, Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  if (!env || !env->widget_manager()) {
    return;
  }

  Isolate* isolate = context->GetIsolate();
  HandleScope scope(isolate);

  Local<ObjectTemplate> inst = env->isolate_data()->canvas_constructor_template()->InstanceTemplate();

  auto create_canvas = [&](const char* name) -> Canvas* {
    Local<Object> obj = inst->NewInstance(context).ToLocalChecked();
    Canvas* canvas = new Canvas(env->principal_realm(), obj);
    target->Set(context, OneByteString(isolate, name), obj).Check();
    return canvas;
  };

  env->widget_manager()->set_background_canvas(create_canvas("background"));
  env->widget_manager()->set_foreground_canvas(create_canvas("foreground"));
}

void Canvas::New(const FunctionCallbackInfo<Value>& args) {
  args.GetIsolate()->ThrowError("Canvas is not constructable");
}

// addLine(key, p1, p2, color, thickness = 1.0)
void Canvas::AddLine(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::Line;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.p2 = ImVec2(isolate, args[2]);
  prim.color = args[3]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  prim.thickness = args.Length() > 4 ? static_cast<float>(args[4]->NumberValue(ctx).FromMaybe(1.0)) : 1.0f;

  self->primitives_[*key] = prim;
}

// addRect(key, min, max, color, rounding = 0, flags = 0, thickness = 1.0)
void Canvas::AddRect(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::Rect;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.p2 = ImVec2(isolate, args[2]);
  prim.color = args[3]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  prim.rounding = args.Length() > 4 ? static_cast<float>(args[4]->NumberValue(ctx).FromMaybe(0.0)) : 0.0f;
  prim.draw_flags = args.Length() > 5 ? static_cast<ImDrawFlags>(args[5]->Uint32Value(ctx).FromMaybe(0)) : 0;
  prim.thickness = args.Length() > 6 ? static_cast<float>(args[6]->NumberValue(ctx).FromMaybe(1.0)) : 1.0f;

  self->primitives_[*key] = prim;
}

// addRectFilled(key, min, max, color, rounding = 0, flags = 0)
void Canvas::AddRectFilled(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::RectFilled;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.p2 = ImVec2(isolate, args[2]);
  prim.color = args[3]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  prim.rounding = args.Length() > 4 ? static_cast<float>(args[4]->NumberValue(ctx).FromMaybe(0.0)) : 0.0f;
  prim.draw_flags = args.Length() > 5 ? static_cast<ImDrawFlags>(args[5]->Uint32Value(ctx).FromMaybe(0)) : 0;

  self->primitives_[*key] = prim;
}

// addCircle(key, center, radius, color, segments = 0, thickness = 1.0)
void Canvas::AddCircle(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::Circle;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.radius = static_cast<float>(args[2]->NumberValue(ctx).FromMaybe(0.0));
  prim.color = args[3]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  prim.segments = args.Length() > 4 ? static_cast<int>(args[4]->Int32Value(ctx).FromMaybe(0)) : 0;
  prim.thickness = args.Length() > 5 ? static_cast<float>(args[5]->NumberValue(ctx).FromMaybe(1.0)) : 1.0f;

  self->primitives_[*key] = prim;
}

// addCircleFilled(key, center, radius, color, segments = 0)
void Canvas::AddCircleFilled(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::CircleFilled;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.radius = static_cast<float>(args[2]->NumberValue(ctx).FromMaybe(0.0));
  prim.color = args[3]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  prim.segments = args.Length() > 4 ? static_cast<int>(args[4]->Int32Value(ctx).FromMaybe(0)) : 0;

  self->primitives_[*key] = prim;
}

// addText(key, pos, color, text)
void Canvas::AddText(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 4) return;

  Isolate* isolate = args.GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();

  Utf8Value key(isolate, args[0]);
  Primitive prim;
  prim.type = Primitive::Type::Text;
  prim.p1 = ImVec2(isolate, args[1]);
  prim.color = args[2]->Uint32Value(ctx).FromMaybe(IM_COL32_WHITE);
  Utf8Value text(isolate, args[3]);
  prim.text = *text;
  if (args.Length() > 4) {
    prim.font_size = static_cast<float>(args[4]->NumberValue(ctx).FromMaybe(0.0));
  }

  self->primitives_[*key] = prim;
}

// remove(key)
void Canvas::Remove(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self || args.Length() < 1) return;

  Utf8Value key(args.GetIsolate(), args[0]);
  if (self->primitives_.contains(*key)) {
    self->primitives_.erase(*key);
  }
}

// clear()
void Canvas::Clear(const FunctionCallbackInfo<Value>& args) {
  Canvas* self = BaseObject::Unwrap<Canvas>(args.This());
  if (!self) return;
  self->primitives_.clear();
}

void Canvas::Render(ImDrawList* draw_list, ImVec2 offset) {
  if (!draw_list) return;
  const float ox = offset.x;
  const float oy = offset.y;
  for (const auto& [key, p] : primitives_) {
    const ImVec2 p1(p.p1.x + ox, p.p1.y + oy);
    const ImVec2 p2(p.p2.x + ox, p.p2.y + oy);
    switch (p.type) {
      case Primitive::Type::Line:
        draw_list->AddLine(p1, p2, p.color, p.thickness);
        break;
      case Primitive::Type::Rect:
        draw_list->AddRect(p1, p2, p.color, p.rounding, p.draw_flags, p.thickness);
        break;
      case Primitive::Type::RectFilled:
        draw_list->AddRectFilled(p1, p2, p.color, p.rounding, p.draw_flags);
        break;
      case Primitive::Type::Circle:
        draw_list->AddCircle(p1, p.radius, p.color, p.segments, p.thickness);
        break;
      case Primitive::Type::CircleFilled:
        draw_list->AddCircleFilled(p1, p.radius, p.color, p.segments);
        break;
      case Primitive::Type::Text:
        if (p.font_size > 0.f) {
          draw_list->AddText(nullptr, p.font_size, p1, p.color, p.text.c_str());
        } else {
          draw_list->AddText(p1, p.color, p.text.c_str());
        }
        break;
    }
  }
}

}  // namespace nyx
