#include "gpu/d3d12_debug.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <plume_render_interface.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#endif

#include "gpu/config.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

#ifdef _WIN32
namespace {

bool g_enabled = false;
ID3D12InfoQueue* g_info_queue = nullptr;
bool g_callback_registered = false;

std::mutex g_message_mutex;
std::unordered_map<int, uint32_t> g_message_counts;
// Each message id is logged this many times and then counted silently, since one bad call repeats every frame.
constexpr uint32_t kRepeatsLogged = 5;

void LogMessage(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char* text) {
  if (severity > D3D12_MESSAGE_SEVERITY_WARNING) return;  // INFO and MESSAGE are chatter
  uint32_t seen;
  {
    std::lock_guard lock(g_message_mutex);
    seen = ++g_message_counts[int(id)];
  }
  if (seen > kRepeatsLogged) return;
  const char* last = seen == kRepeatsLogged ? " (repeats from here on are not logged)" : "";
  if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
    KGFX_WARN("d3d12 debug layer warning #{}: {}{}", int(id), text ? text : "", last);
  } else {
    KGFX_ERROR("d3d12 debug layer {} #{}: {}{}", severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "ERROR", int(id), text ? text : "", last);
  }
}

void CALLBACK OnMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
  LogMessage(severity, id, text);
}

const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  static const char* const kNames[] = {
      "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
      "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
      "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
      "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame",
      "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
      "WriteBufferImmediate",
  };
  return size_t(op) < std::size(kNames) ? kNames[op] : nullptr;
}

}  // namespace
#endif

void EnableD3D12Diagnostics() {
#ifdef _WIN32
  if (!REXCVAR_GET(kameo_gfx_d3d12_debug)) return;
  g_enabled = true;
  // DRED ships with the OS, so the breadcrumbs work even where the debug layer is not installed.
  ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))) && dred) {
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->Release();
    KGFX_INFO("d3d12 DRED breadcrumbs and page-fault reporting enabled");
  } else {
    KGFX_WARN("d3d12 DRED unavailable");
  }
  ID3D12Debug* debug = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
    debug->EnableDebugLayer();
    debug->Release();
    KGFX_INFO("d3d12 debug layer enabled");
  } else {
    KGFX_WARN("d3d12 debug layer unavailable: install Windows' optional \"Graphics Tools\" feature for it (DRED still reports a device removal)");
  }
#endif
}

void AttachD3D12DebugMessages(plume::RenderDevice* device) {
#ifdef _WIN32
  if (!g_enabled || !device) return;
  ID3D12Device* native = static_cast<plume::D3D12Device*>(device)->d3d;
  if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&g_info_queue)))) {
    g_info_queue = nullptr;
    return;  // the layer did not load
  }
  // Windows 11's info queue calls back at the offending API call itself; Windows 10's only stores messages, which are drained each frame.
  ID3D12InfoQueue1* queue1 = nullptr;
  if (SUCCEEDED(g_info_queue->QueryInterface(IID_PPV_ARGS(&queue1))) && queue1) {
    DWORD cookie = 0;
    g_callback_registered = SUCCEEDED(queue1->RegisterMessageCallback(OnMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie));
    queue1->Release();
  }
  if (g_callback_registered) {
    // The callback has them all; storing them too would only grow the queue.
    g_info_queue->SetMuteDebugOutput(TRUE);
  }
  KGFX_INFO("d3d12 debug messages go to this log ({})", g_callback_registered ? "as they happen" : "collected each frame");
#else
  (void)device;
#endif
}

void DrainD3D12DebugMessages() {
#ifdef _WIN32
  if (!g_info_queue || g_callback_registered) return;
  const UINT64 count = g_info_queue->GetNumStoredMessages();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T size = 0;
    if (FAILED(g_info_queue->GetMessage(i, nullptr, &size)) || size == 0) continue;
    auto* message = static_cast<D3D12_MESSAGE*>(::operator new(size));
    if (SUCCEEDED(g_info_queue->GetMessage(i, message, &size))) {
      LogMessage(message->Severity, message->ID, message->pDescription);
    }
    ::operator delete(message);
  }
  g_info_queue->ClearStoredMessages();
#endif
}

void LogD3D12RemovalDetails(plume::RenderDevice* device) {
#ifdef _WIN32
  DrainD3D12DebugMessages();
  if (!g_enabled || !device) return;
  ID3D12Device* native = static_cast<plume::D3D12Device*>(device)->d3d;
  ID3D12DeviceRemovedExtendedData* dred = nullptr;
  if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
    KGFX_WARN("d3d12 DRED: no removal data");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    uint32_t lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
      const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      // A list that finished every operation is not where the GPU stopped.
      if (done >= node->BreadcrumbCount) continue;
      ++lists;
      KGFX_ERROR("d3d12 DRED: command list {} stopped after {} of {} operations", static_cast<const void*>(node->pCommandList), done, node->BreadcrumbCount);
      const uint32_t first = done > 6 ? done - 6 : 0;
      const uint32_t end = std::min(node->BreadcrumbCount, done + 4);
      for (uint32_t i = first; i < end; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
        const char* name = BreadcrumbOpName(op);
        const char* where = i < done ? "done" : i == done ? "STOPPED HERE" : "not reached";
        if (name) {
          KGFX_ERROR("d3d12 DRED:   [{}] {} ({})", i, name, where);
        } else {
          KGFX_ERROR("d3d12 DRED:   [{}] op {} ({})", i, int(op), where);
        }
      }
    }
    if (lists == 0) KGFX_ERROR("d3d12 DRED: every recorded command list completed");
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA != 0) {
    KGFX_ERROR("d3d12 DRED: GPU page fault at 0x{:016X}", uint64_t(fault.PageFaultVA));
  }
  dred->Release();
#else
  (void)device;
#endif
}

}  // namespace kameo::gpu::detail
