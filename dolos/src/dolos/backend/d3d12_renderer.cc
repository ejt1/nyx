#include "dolos/backend/d3d12_renderer.h"

#include "dolos/pipe_log.h"
#include "nyx/nyx_imgui.h"
#include "nyx/util.h"

#include <lazy_importer.hpp>

namespace dolos {

enum IUnknownVTable : size_t {
  IUnknown_QueryInterface,
  IUnknown_AddRef,
  IUnknown_Release,

  IUnknown_IndexCount,
};

enum ID3D12ObjectVTable : size_t {
  ID3D12Object_GetPrivateData = IUnknown_IndexCount,
  ID3D12Object_SetPrivateData,
  ID3D12Object_SetPrivateDataInterface,
  ID3D12Object_SetName,

  ID3D12Object_IndexCount,
};

enum ID3D12DeviceChild : size_t {
  ID3D12DeviceChild_GetDevice = ID3D12Object_IndexCount,

  ID3D12DeviceChild_IndexCount,
};

enum ID3D12CommandQueueVTable : size_t {
  ID3D12CommandQueue_UpdateTileMappings = ID3D12DeviceChild_IndexCount,
  ID3D12CommandQueue_CopyTileMappings,
  ID3D12CommandQueue_ExecuteCommandLists,
  ID3D12CommandQueue_SetMarker,
  ID3D12CommandQueue_BeginEvent,
  ID3D12CommandQueue_EndEvent,
  ID3D12CommandQueue_Signal,
  ID3D12CommandQueue_Wait,
  ID3D12CommandQueue_GetTimestampFrequency,
  ID3D12CommandQueue_GetClockCalibration,
  ID3D12CommandQueue_GetDesc,
};

enum IDXGIObjectVTable : size_t {
  IDXGIObject_SetPrivateData = IUnknown_IndexCount,
  IDXGIObject_SetPrivateDataInterface,
  IDXGIObject_GetPrivateData,
  IDXGIObject_GetParent,

  IDXGIObject_IndexCount,
};

enum IDXGIDeviceSubObject : size_t {
  IDXGIDeviceSubObject_GetDevice = IDXGIObject_IndexCount,

  IDXGIDeviceSubObject_IndexCount,
};

enum IDXGISwapChainVTable : size_t {
  IDXGISwapChain_Present = IDXGIDeviceSubObject_IndexCount,
  IDXGISwapChain_GetBuffer,
  IDXGISwapChain_SetFullscreenState,
  IDXGISwapChain_GetFullscreenState,
  IDXGISwapChain_GetDesc,
  IDXGISwapChain_ResizeBuffers,
  IDXGISwapChain_ResizeTarget,
  IDXGISwapChain_GetContainingOutput,
  IDXGISwapChain_GetFrameStatistics,
  IDXGISwapChain_GetLastPresentCount,
};

D3D12Renderer::D3D12Renderer(Win32Window* window, nyx::NyxImGui* nyx_imgui) noexcept
    : window_(window),
      nyx_imgui_(nyx_imgui),
      present_(&D3D12Renderer::Present, this),
      resize_buffers_(&D3D12Renderer::ResizeBuffers, this),
      execute_command_lists_(&D3D12Renderer::ExecuteCommandLists, this),
      rtv_heap_(nullptr),
      srv_heap_(nullptr),
      command_list_(nullptr),
      command_queue_(nullptr),
      fixme_shutdown_ticks_(0) {}

D3D12Renderer::~D3D12Renderer() noexcept {}

bool D3D12Renderer::Initialize() {
  HRESULT hr = S_OK;

  CHECK_EQ(state_, kUninitialized);
  state_ = kInitializing;

  const auto mod_dxgi = LI_MODULE("dxgi.dll").safe();
  const auto mod_d3d12 = LI_MODULE("d3d12.dll").safe();
  if (!mod_dxgi) {
    return false;  // NYX_ERR_MODULE_DXGI_DLL_MISSING;
  }
  if (!mod_d3d12) {
    return false;  // NYX_ERR_MODULE_D3D12_DLL_MISSING;
  }

  const auto d3d12_create_device = LI_FN(D3D12CreateDevice).in_safe(mod_d3d12);
  if (!d3d12_create_device) {
    return false;  // NYX_ERR_PROC_CREATE_DEVICE_MISSING;
  }

  const auto d3d12_create_dxgi_factory1 = LI_FN(CreateDXGIFactory2).in_safe(mod_dxgi);
  if (!d3d12_create_dxgi_factory1) {
    return false;  // NYX_ERR_PROC_CREATE_DXGI_FACTORY1_MISSING;
  }

  IDXGIFactory1* factory = nullptr;
  hr = d3d12_create_dxgi_factory1(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_FACTORY;
  }

  ComPtr<ID3D12Device> device;
  hr = d3d12_create_device(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_DEVICE;
  }

  // Create command queue
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

  ComPtr<ID3D12CommandQueue> cmd_queue;
  hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmd_queue));
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_COMMAND_QUEUE;
  }

  // Create command allocator
  ComPtr<ID3D12CommandAllocator> cmd_allocator;
  hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator));
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_COMMAND_ALLOCATOR;
  }

  // Create command list
  ComPtr<ID3D12GraphicsCommandList> cmd_list;
  hr = device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&cmd_list));
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_COMMAND_LIST;
  }

  TemporaryWindow tmp_window;
  if (!tmp_window.hwnd) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_WINDOW;
  }

  // Create swapchain
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = tmp_window.hwnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  ComPtr<IDXGISwapChain> swapchain;
  hr = factory->CreateSwapChain(cmd_queue.Get(), &sd, &swapchain);
  if (FAILED(hr)) {
    return false;  // NYX_ERR_D3D12_COULD_NOT_CREATE_SWAP_CHAIN;
  }

  present_.Install(swapchain.Get(), IDXGISwapChain_Present);
  resize_buffers_.Install(swapchain.Get(), IDXGISwapChain_ResizeBuffers);

  return true;  // NYX_ERR_SUCCESS;
}

void D3D12Renderer::Shutdown() {
  if (state_ == kInitialized) {
    state_ = kShutdown;
    state_.wait(kShutdown);
  }
  present_.Uninstall();
  resize_buffers_.Uninstall();
  execute_command_lists_.Uninstall();
}

HRESULT D3D12Renderer::Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
  nyx_imgui_->EnsureGameContext();

  switch (state_) {
    case kInitializing: {
      PIPE_LOG("[Renderer] Present called for the first time");
      CreateRenderTargets(self);
      ID3D12Device* device;
      self->GetDevice(IID_PPV_ARGS(&device));
      imgui_.Initialize(device,
                        static_cast<int>(frames_.size()),
                        DXGI_FORMAT_R8G8B8A8_UNORM,
                        srv_heap_.Get(),
                        srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                        srv_heap_->GetGPUDescriptorHandleForHeapStart());

      state_ = kInitialized;
      state_.notify_all();
    } break;

    case kRecreate:
      CreateRenderTargets(self);
      state_ = kInitialized;
      break;

    case kInitialized: {
      if (!command_queue_) {
        break;
      }

      imgui_.NewFrame();
      IDXGISwapChain3* self3 = static_cast<IDXGISwapChain3*>(self);
      const auto buffer_idx = self3->GetCurrentBackBufferIndex();
      const auto& frame = frames_[buffer_idx];
      frame.allocator->Reset();

      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barrier.Transition.pResource = frame.rtv.Get();
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_list_->Reset(frame.allocator.Get(), nullptr);
      command_list_->ResourceBarrier(1, &barrier);

      command_list_->OMSetRenderTargets(1, &frame.rtd, FALSE, nullptr);
      command_list_->SetDescriptorHeaps(1, srv_heap_.GetAddressOf());

      // TODO: move background drawlist to a game hook instead to be drawn under GUI
      if (nyx_imgui_->visible()) {
        {
          auto* bg = nyx_imgui_->background();
          ImDrawData* data = bg->Acquire();
          if (data) {
            if (data->Valid) {
              imgui_.RenderDrawData(data, command_list_.Get());
            }
            bg->Release();
          }
        }

        {
          auto* fg = nyx_imgui_->foreground();
          ImDrawData* data = fg->Acquire();
          if (data) {
            if (data->Valid) {
              imgui_.RenderDrawData(data, command_list_.Get());
            }
            fg->Release();
          }
        }
      }

      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
      command_list_->ResourceBarrier(1, &barrier);
      command_list_->Close();
      command_queue_->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(command_list_.GetAddressOf()));
    } break;

    case kShutdown:
      // Not sure why but shutting down on the first frame after its requested always results in a crash (POE2).
      // This simple work-around waits for 30 frames before we actually shuts down. It could be that we have device
      // resources in flight and when we reset them the game accesses them on the next frame
      if (++fixme_shutdown_ticks_ < 30) {
        break;
      }
      PIPE_LOG("[Renderer] Present called for the last time");
      ResetRenderTargets();
      imgui_.Shutdown();
      PIPE_LOG("[Renderer] Renderer cleaned up");
      state_ = kUninitialized;
      state_.notify_all();
      break;
  }

  return present_.CallOriginal(self, SyncInterval, Flags);
}

HRESULT D3D12Renderer::ResizeBuffers(
    IDXGISwapChain* self, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
  if (state_ == kInitialized) {
    ResetRenderTargets();
    nyx_imgui_->ClearDrawData();
    HRESULT hr = resize_buffers_.CallOriginal(self, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    state_ = kRecreate;
    return hr;
  }
  return resize_buffers_.CallOriginal(self, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

void D3D12Renderer::ExecuteCommandLists(ID3D12CommandQueue* self,
                                        UINT NumCommandLists,
                                        ID3D12CommandList* const* ppCommandLists) {
  if (state_ == kInitialized && !command_queue_) {
    D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
      PIPE_LOG("[Renderer] Found suitable command queue at {:p}", static_cast<void*>(self));
      command_queue_ = self;
    } else {
      PIPE_LOG("[Renderer] Skipping command queue of type {}", static_cast<int>(desc.Type));
    }
  }
  execute_command_lists_.CallOriginal(self, NumCommandLists, ppCommandLists);
}

HRESULT D3D12Renderer::CreateRenderTargets(IDXGISwapChain* pSwapChain) {
  ID3D12Device* device;
  pSwapChain->GetDevice(IID_PPV_ARGS(&device));

  DXGI_SWAP_CHAIN_DESC sd;
  pSwapChain->GetDesc(&sd);
  window_->SetActiveWindow(sd.OutputWindow);
  frames_.resize(sd.BufferCount);

  // XXX: Due to POE2 using a bundled D3D12Core.dll we hook ExecuteCommandLists here instead of at initialization. This
  // causes us to hook the correct VTable method instead of possibly the system D3D12Core.dll method.
  // Not sure if this method will work for games not bundling D3D12Core.dll or if it's fail-safe.
  // Could be moved out of CreateRenderTargets to avoid having to check if hook is enabled and possibly unintentionally
  // re-enabling it during shutdown.
  if (!execute_command_lists_.IsEnabled()) {
    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ComPtr<ID3D12CommandQueue> cmd_queue;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmd_queue));
    execute_command_lists_.Install(cmd_queue.Get(), ID3D12CommandQueue_ExecuteCommandLists);
  }

  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
  srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_desc.NumDescriptors = 1;
  srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_));

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = static_cast<uint32_t>(frames_.size());
  rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtv_desc.NodeMask = 1;
  device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_));

  const auto rtv_desc_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtd = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

  for (size_t i = 0; i < frames_.size(); ++i) {
    ID3D12Resource* backbuffer = nullptr;
    pSwapChain->GetBuffer(static_cast<uint32_t>(i), IID_PPV_ARGS(&backbuffer));

    device->CreateRenderTargetView(backbuffer, nullptr, rtd);
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames_[i].allocator));

    frames_[i].rtd = rtd;
    frames_[i].rtv.Attach(backbuffer);
    rtd.ptr += rtv_desc_size;
  }

  device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&command_list_));

  command_list_->Close();

  PIPE_LOG("[Renderer] Render targets created");
  PIPE_LOG("[Renderer]   device: {:p}", static_cast<void*>(device));
  PIPE_LOG("[Renderer]   rtv heap: {:p}", static_cast<void*>(rtv_heap_.Get()));
  PIPE_LOG("[Renderer]   srv heap: {:p}", static_cast<void*>(srv_heap_.Get()));
  PIPE_LOG("[Renderer]   cmd list: {:p}", static_cast<void*>(command_list_.Get()));
  PIPE_LOG("[Renderer]   cmd queue: {:p}", static_cast<void*>(command_queue_));
  for (size_t i = 0; i < frames_.size(); ++i) {
    FrameContext& framectx = frames_[i];
    PIPE_LOG("[Renderer]   Frame[{}] allocator: {:p}", i, static_cast<void*>(framectx.allocator.Get()));
    PIPE_LOG("[Renderer]   Frame[{}] rtd: {}", i, framectx.rtd.ptr);
    PIPE_LOG("[Renderer]   Frame[{}] rtv: {:p}", i, static_cast<void*>(framectx.rtv.Get()));
  }

  return S_OK;
}

void D3D12Renderer::ResetRenderTargets() {
  PIPE_LOG("[Renderer] Resetting render targets");
  imgui_.InvalidateDeviceObjects();

  for (FrameContext& ctx : frames_) {
    ctx.rtv.Reset();
    ctx.allocator.Reset();
  }
  frames_.clear();

  command_list_.Reset();
  rtv_heap_.Reset();
  srv_heap_.Reset();

  command_queue_ = nullptr;
}

}  // namespace dolos
