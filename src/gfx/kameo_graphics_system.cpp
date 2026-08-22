#include "kameo_graphics_system.h"

#include <plume_render_interface.h>

#include "shaders/bink_shaders.h"
#include "shader_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(kameo_gfx_debug_layer, false, "Kameo",
                    "Enable the D3D12 debug layer for the native renderer. Reports why a "
                    "pipeline or resource was rejected, which is otherwise silent. Needs the "
                    "Windows optional feature \"Graphics Tools\".");

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

// The X_STATUS_* macros cast to an unqualified `X_STATUS`, so they only compile
// where that name is visible without the namespace.
using rex::X_STATUS;

namespace kameo::gfx {

KameoGraphicsSystem* KameoGraphicsSystem::s_instance = nullptr;

KameoGraphicsSystem* KameoGraphicsSystem::instance() { return s_instance; }

namespace {

constexpr uint32_t kSwapChainTextureCount = 3;
constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT;

const char* VendorName(plume::RenderDeviceVendor vendor) {
  switch (vendor) {
    case plume::RenderDeviceVendor::AMD: return "AMD";
    case plume::RenderDeviceVendor::NVIDIA: return "NVIDIA";
    case plume::RenderDeviceVendor::INTEL: return "Intel";
    default: return "unknown";
  }
}

}  // namespace

KameoGraphicsSystem::KameoGraphicsSystem(std::function<void*()> get_native_window, Backend backend)
    : get_native_window_(std::move(get_native_window)), backend_(backend) {}

KameoGraphicsSystem::~KameoGraphicsSystem() { Shutdown(); }

bool KameoGraphicsSystem::CreateInterface() {
  // Mirrors UnleashedRecomp's policy: D3D12 is the Windows default and Vulkan
  // is the portable path, with a fallback to the other if the first fails.
  // Both compile from the same plume sources, so this is a runtime choice.
  //
  // D3D12 first on Windows also keeps bring-up comparable against the xenos
  // plugin, which creates a D3D12 device -- same API means the same captures
  // and debug layer when diffing our output against known-good.
  auto try_d3d12 = [&]() -> bool {
#ifdef _WIN32
    interface_ = plume::CreateD3D12Interface();
    if (interface_) {
      using_vulkan_ = false;
      return true;
    }
#endif
    return false;
  };

  auto try_vulkan = [&]() -> bool {
    interface_ = plume::CreateVulkanInterface();
    if (interface_) {
      using_vulkan_ = true;
      return true;
    }
    return false;
  };

  switch (backend_) {
    case Backend::D3D12:
      return try_d3d12();
    case Backend::Vulkan:
      return try_vulkan();
    case Backend::Auto:
    default:
#ifdef _WIN32
      return try_d3d12() || try_vulkan();
#else
      return try_vulkan();
#endif
  }
}

rex::X_STATUS KameoGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  (void)app_context;

  if (swap_chain_) {
    return X_STATUS_SUCCESS;  // idempotent, as the interface requires
  }

  // Must precede device creation, so it cannot live behind the interface.
  if (REXCVAR_GET(kameo_gfx_debug_layer)) {
    EnableD3D12DebugLayer();
  }

  if (!CreateInterface()) {
    REXLOG_ERROR("[kameo-gfx] no usable graphics backend");
    return X_STATUS_UNSUCCESSFUL;
  }

  device_ = interface_->createDevice();
  if (!device_) {
    REXLOG_ERROR("[kameo-gfx] createDevice failed ({})", using_vulkan_ ? "vulkan" : "d3d12");
    return X_STATUS_UNSUCCESSFUL;
  }

  const plume::RenderDeviceDescription& desc = device_->getDescription();
  REXLOG_INFO("[kameo-gfx] backend={} device=\"{}\" vendor={} vram={} MiB",
              using_vulkan_ ? "vulkan" : "d3d12", desc.name, VendorName(desc.vendor),
              desc.dedicatedVideoMemory / (1024 * 1024));

  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!queue_) {
    REXLOG_ERROR("[kameo-gfx] createCommandQueue failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  // The swap chain CANNOT be built here: ReXApp::SetupPresentation calls this
  // before it creates the window (rex_app.cpp -- graphics at line 328, window
  // at 336, Open() at 352). That ordering is deliberate in the SDK, where
  // SetupPresentation builds the provider and the presenter is bound to the
  // window afterwards. So defer to the first present, by which time the window
  // is open.
  s_instance = this;
  REXLOG_INFO("[kameo-gfx] device ready; swap chain deferred until the window exists");
  return X_STATUS_SUCCESS;
}

bool KameoGraphicsSystem::EnsureSwapChain() {
  if (swap_chain_) {
    return true;
  }
  if (swap_chain_failed_) {
    return false;  // do not retry every frame
  }

  void* native_window = get_native_window_ ? get_native_window_() : nullptr;
  if (!native_window) {
    return false;  // window not open yet; try again next frame
  }

  plume::RenderSwapChainDesc swap_desc;
  swap_desc.renderWindow = reinterpret_cast<plume::RenderWindow>(native_window);
  swap_desc.format = plume::RenderFormat::B8G8R8A8_UNORM;
  swap_desc.textureCount = kSwapChainTextureCount;
  swap_chain_ = queue_->createSwapChain(swap_desc);
  if (!swap_chain_ || !CreateFrameResources()) {
    REXLOG_ERROR("[kameo-gfx] swap chain creation failed");
    swap_chain_.reset();
    swap_chain_failed_ = true;
    return false;
  }

  REXLOG_INFO("[kameo-gfx] swap chain ready ({}x{}, {} buffers)", swap_chain_->getWidth(),
              swap_chain_->getHeight(), swap_chain_->getTextureCount());
  return true;
}

void KameoGraphicsSystem::WaitForGpu() {
  // plume's D3D12 waitForCommandFence is WaitForSingleObjectEx(event, INFINITE)
  // on an event that executeCommandLists signals and the wait consumes. It does
  // NOT check whether a signal is actually outstanding, so a second wait for the
  // same submission blocks the calling thread forever.
  //
  // That is what froze the game on window resize: PresentClear waits at the end
  // of every frame, then the resize path waited again with no submission in
  // between, and the guest thread driving presentation never came back.
  if (!fence_pending_) {
    return;
  }
  fence_pending_ = false;
  queue_->waitForCommandFence(fence_.get());
}

bool KameoGraphicsSystem::CreateFrameResources() {
  // The fence is replaced below, so any signal the old one was carrying is gone.
  fence_pending_ = false;
  command_list_ = queue_->createCommandList();
  fence_ = device_->createCommandFence();
  acquire_semaphore_ = device_->createCommandSemaphore();
  present_semaphore_ = device_->createCommandSemaphore();
  if (!command_list_ || !fence_ || !acquire_semaphore_ || !present_semaphore_) {
    REXLOG_ERROR("[kameo-gfx] failed to create per-frame objects");
    return false;
  }

  // One depth buffer shared by every swap chain image. The guest depth-tests
  // against EDRAM; collapsing its tiled passes to a single full-resolution one
  // means a single host depth target of the same size.
  depth_view_.reset();
  depth_texture_ = device_->createTexture(plume::RenderTextureDesc::DepthTarget(
      swap_chain_->getWidth(), swap_chain_->getHeight(), kDepthFormat));
  if (!depth_texture_) {
    REXLOG_ERROR("[kameo-gfx] depth target creation failed");
    return false;
  }
  depth_view_ = depth_texture_->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(kDepthFormat));

  framebuffers_.clear();
  for (uint32_t i = 0; i < swap_chain_->getTextureCount(); ++i) {
    const plume::RenderTexture* color = swap_chain_->getTexture(i);
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth_texture_.get();
    desc.depthAttachmentView = depth_view_.get();
    auto fb = device_->createFramebuffer(desc);
    if (!fb) {
      REXLOG_ERROR("[kameo-gfx] createFramebuffer failed for buffer {}", i);
      return false;
    }
    framebuffers_.push_back(std::move(fb));
  }
  return true;
}

void KameoGraphicsSystem::PresentClear(float r, float g, float b) {
  if (!EnsureSwapChain() || swap_chain_->isEmpty()) {
    return;
  }

  if (swap_chain_->needsResize()) {
    // Framebuffers reference the old swap chain textures, so they have to go
    // before the resize and be rebuilt against the new ones.
    WaitForGpu();
    framebuffers_.clear();
    if (!swap_chain_->resize()) {
      return;
    }
    if (!CreateFrameResources()) {
      return;
    }
  }

  // Build any newly-resolved shader modules on this thread; the creation hooks
  // run on guest threads and only queue them.
  EnsureShaderModules();
  EnsureDrawPathResources();

  GfxTrace("present:acquire");
  uint32_t index = 0;
  if (!swap_chain_->acquireTexture(acquire_semaphore_.get(), &index)) {
    return;
  }

  plume::RenderTexture* texture = swap_chain_->getTexture(index);
  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();

  GfxTrace("present:record");
  command_list_->begin();
  command_list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                          plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE));
  command_list_->setFramebuffer(framebuffers_[index].get());
  command_list_->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  command_list_->setScissors(plume::RenderRect(0, 0, width, height));
  command_list_->clearColor(0, plume::RenderColor(r, g, b, 1.0f));
  command_list_->clearDepth(true, 1.0f);
  // The scene stream carries the video's position in guest order, so it is
  // drawn from inside DrawCapturedScene. The call afterwards is a fallback for
  // a frame no marker consumed (the draw path being off, say); DrawBink takes
  // the pending frame, so only one of the two ever draws it.
  DrawCapturedScene(*this, command_list_.get(), texture, framebuffers_[index].get(), width,
                    height);
  DrawBink(command_list_.get());
  DrawOverlay(command_list_.get());
  command_list_->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::PRESENT));
  command_list_->end();

  const plume::RenderCommandList* lists = command_list_.get();
  plume::RenderCommandSemaphore* wait = acquire_semaphore_.get();
  plume::RenderCommandSemaphore* signal = present_semaphore_.get();
  GfxTrace("present:submit");
  queue_->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, fence_.get());
  fence_pending_ = true;

  GfxTrace("present:present");
  swap_chain_->present(index, &signal, 1);

  // Block until the frame is done. Correct but not fast -- there is no
  // pipelining and no per-frame resource rotation yet. Revisit once real draw
  // submission exists, since that is when the cost starts to matter.
  GfxTrace("present:fence-wait");
  WaitForGpu();
  GfxTrace("present:done");

  if ((frame_index_++ % 300) == 0) {
    REXLOG_INFO("[kameo-gfx] presented frame {} ({}x{})", frame_index_, width, height);
  }
}

rex::X_STATUS KameoGraphicsSystem::SetupGuestGpu(
    rex::runtime::FunctionDispatcher* function_dispatcher, rex::system::KernelState* kernel_state) {
  (void)function_dispatcher;
  (void)kernel_state;

  // Deliberately empty, and it stays that way.
  //
  // The obvious fix for the first-frame stall is a vsync worker firing the
  // guest's interrupt callback at 60 Hz -- but that is a piece of Xenia's
  // GraphicsSystem, and rebuilding the emulator underneath a "native" renderer
  // defeats the point. The whole reason to intercept at the D3D API is that
  // nothing below it has to exist.
  //
  // So instead of simulating GPU progress so the game's D3D keeps waiting
  // happily, we replace the functions that do the waiting. See
  // kameo_gfx_hooks.cpp.
  REXLOG_INFO("[kameo-gfx] guest GPU not emulated (no ring buffer, no interrupts)");
  return X_STATUS_SUCCESS;
}

void KameoGraphicsSystem::Shutdown() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
  // Reverse construction order; plume objects hold device references.
  ReleaseDrawPathCaches();
  shader_modules_.clear();
  overlay_atlases_.clear();
  depth_view_.reset();
  depth_texture_.reset();
  framebuffers_.clear();
  present_semaphore_.reset();
  acquire_semaphore_.reset();
  fence_.reset();
  command_list_.reset();
  swap_chain_.reset();
  queue_.reset();
  device_.reset();
  interface_.reset();
}

}  // namespace kameo::gfx

// ============================================================================
// Bink video blit
// ============================================================================
//
// First real rendering in the native path. Kameo decodes Bink frames on the CPU
// into three or four D3DFMT_LIN_L8 planes and blits them with a YUV->RGB pixel
// shader (Draw_Bink_textures, 0x82265558). We capture that call and reproduce it
// natively: upload the planes, draw one quad, convert in our own shader using
// the game's own conversion matrix.

namespace kameo::gfx {

void KameoGraphicsSystem::DrawPendingBink(plume::RenderCommandList* list) { DrawBink(list); }

void KameoGraphicsSystem::SubmitBinkFrame(const BinkFrame& frame) {
  std::lock_guard<std::mutex> lock(bink_mutex_);
  bink_pending_ = frame;
}

bool KameoGraphicsSystem::EnsureBinkResources() {
  if (bink_pipeline_) {
    return true;
  }
  if (bink_failed_ || !device_) {
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  bink_sampler_ = device_->createSampler(sampler_desc);

  // Bindings are flat across types and must match the HLSL register numbers.
  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 1, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 2, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 3, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 4, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 5, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  bink_descriptors_ = device_->createDescriptorSet(set_desc);

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  bink_layout_ = device_->createPipelineLayout(layout_desc);

  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
  if (format == plume::RenderShaderFormat::SPIRV) {
    vs = device_->createShader(shaders::g_binkVSSpirv, sizeof(shaders::g_binkVSSpirv), "VSMain", format);
    ps = device_->createShader(shaders::g_binkPSSpirv, sizeof(shaders::g_binkPSSpirv), "PSMain", format);
  } else {
    vs = device_->createShader(shaders::g_binkVSDxil, sizeof(shaders::g_binkVSDxil), "VSMain", format);
    ps = device_->createShader(shaders::g_binkPSDxil, sizeof(shaders::g_binkPSDxil), "PSMain", format);
  }
  if (!vs || !ps) {
    REXLOG_ERROR("[kameo-gfx] bink shader creation failed");
    bink_failed_ = true;
    return false;
  }

  // x,y,z + u,v
  static const plume::RenderInputSlot slot(0, sizeof(float) * 5);
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, sizeof(float) * 3),
  };

  plume::RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.inputSlots = &slot;
  pipeline_desc.inputSlotsCount = 1;
  pipeline_desc.inputElements = elements;
  pipeline_desc.inputElementsCount = uint32_t(std::size(elements));
  pipeline_desc.pipelineLayout = bink_layout_.get();
  pipeline_desc.vertexShader = vs.get();
  pipeline_desc.pixelShader = ps.get();
  pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  bink_pipeline_ = device_->createGraphicsPipeline(pipeline_desc);
  if (!bink_pipeline_) {
    REXLOG_ERROR("[kameo-gfx] bink pipeline creation failed");
    bink_failed_ = true;
    return false;
  }

  bink_vertices_ = device_->createBuffer(
      plume::RenderBufferDesc::VertexBuffer(sizeof(float) * 5 * 4, plume::RenderHeapType::UPLOAD));
  bink_constants_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(float) * 20));
  bink_descriptors_->setSampler(4, bink_sampler_.get());
  bink_descriptors_->setBuffer(5, bink_constants_.get(), sizeof(float) * 20);

  REXLOG_INFO("[kameo-gfx] bink blit pipeline ready ({})",
              format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

namespace {
// D3D12 placed footprints need 256-byte aligned row pitch, so guest rows are
// repacked rather than copied wholesale. The guest pitch comes from
// D3D::LockSurface and is NOT simply the width.
constexpr uint32_t kRowAlign = 256;
uint32_t AlignRow(uint32_t bytes) { return (bytes + kRowAlign - 1) & ~(kRowAlign - 1); }

// Report contiguous column ranges that are zero down essentially every row.
//
// This exists because a PGM dump CANNOT settle the question it was used for.
// The screen shows both chroma planes reading exactly 0 across texel columns
// 128..255 (a 256px green band at screen x=256..511, whose colour solves
// exactly to cr=cb=0 through the guest's own matrix at 0x8273A5B0). A dump read
// back through the same pointer and pitch renders any constant byte offset as a
// perfectly plausible image, so "the planes look clean" did not rule out the
// upload being phase-shifted against the data.
//
// Running this on the guest plane AND on the staging buffer localises the fault
// with one observation: zeros in the guest scan mean the pointer/pitch we were
// handed is wrong; zeros only in the staging scan mean our repack introduced
// them.
void ScanZeroColumns(const char* label, const uint8_t* data, uint32_t width, uint32_t height,
                     uint32_t pitch) {
  if (!data || !width || !height || !pitch) return;

  std::string runs;
  uint32_t run_start = 0;
  bool in_run = false;
  for (uint32_t x = 0; x <= width; ++x) {
    bool zero_column = false;
    if (x < width) {
      uint32_t zeros = 0;
      for (uint32_t y = 0; y < height; ++y) {
        if (data[size_t(y) * pitch + x] == 0) ++zeros;
      }
      zero_column = zeros * 10 >= height * 9;
    }
    if (zero_column && !in_run) {
      in_run = true;
      run_start = x;
    } else if (!zero_column && in_run) {
      in_run = false;
      runs += fmt::format(" [{}..{}]", run_start, x - 1);
    }
  }
  REXLOG_INFO("[kameo-gfx] zero-column scan {} ({}x{} pitch {}):{}", label, width, height, pitch,
              runs.empty() ? std::string(" none") : runs);
}
}  // namespace

bool KameoGraphicsSystem::UploadBinkPlanes(const BinkFrame& frame) {
  const uint32_t widths[4] = {frame.luma_width, frame.chroma_width, frame.chroma_width,
                              frame.luma_width};
  const uint32_t heights[4] = {frame.luma_height, frame.chroma_height, frame.chroma_height,
                               frame.luma_height};

  // Each plane needs its OWN region of the staging buffer. Reusing one region
  // and recording a copy per plane looks fine but is not: the copies do not run
  // until the command list is submitted, so every texture ends up with whichever
  // plane was written last. That produced green/magenta output with the image
  // repeated across the screen -- all three samplers reading the 640x360 chroma
  // plane as if it were 1280x720.
  uint32_t offsets[4] = {};
  uint32_t total = 0;
  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;
    offsets[i] = total;
    total += AlignRow(widths[i]) * heights[i];
  }
  if (total == 0) return false;

  if (!bink_upload_ || bink_upload_size_ < total) {
    bink_upload_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    bink_upload_size_ = total;
    if (!bink_upload_) return false;
  }

  auto* mapped = static_cast<uint8_t*>(bink_upload_->map());
  if (!mapped) return false;

  // One-shot dump of the Y plane exactly as it sits in guest memory. A clean
  // image here means the fault is in the copy footprint; a dirty one means the
  // guest pointer/pitch is wrong.
  // Re-dump periodically rather than once: the first Bink frame is the Rare
  // logo, which is near-greyscale, so flat chroma there proves nothing. Later
  // frames are the colourful title screen, which distinguishes genuinely
  // neutral chroma from a corrupt plane.
  static uint32_t dump_counter = 0;
  const bool diagnose = (dump_counter++ % 200) == 0 && frame.planes[0];
  if (diagnose) {
    static const char* kNames[4] = {"bink_y.pgm", "bink_cr.pgm", "bink_cb.pgm", "bink_a.pgm"};
    static const char* kGuest[4] = {"guest Y", "guest Cr", "guest Cb", "guest A"};
    for (int i = 0; i < 4; ++i) {
      if (!frame.planes[i] || !widths[i] || !heights[i]) continue;
      ScanZeroColumns(kGuest[i], frame.planes[i], widths[i], heights[i], frame.pitches[i]);
      if (FILE* f = fopen(kNames[i], "wb")) {
        fprintf(f, "P5\n%u %u\n255\n", widths[i], heights[i]);
        for (uint32_t y = 0; y < heights[i]; ++y) {
          fwrite(frame.planes[i] + size_t(y) * frame.pitches[i], 1, widths[i], f);
        }
        fclose(f);
        REXLOG_INFO("[kameo-gfx] wrote {} ({}x{} pitch {})", kNames[i], widths[i], heights[i],
                    frame.pitches[i]);
      }
    }
  }

  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;
    const uint32_t dst_pitch = AlignRow(widths[i]);
    uint8_t* dst = mapped + offsets[i];
    for (uint32_t y = 0; y < heights[i]; ++y) {
      std::memcpy(dst + y * dst_pitch, frame.planes[i] + size_t(y) * frame.pitches[i], widths[i]);
    }
  }

  // Same scan on what we actually hand the GPU. The copy reads this back with
  // Footprint.Width = widths[i] and RowPitch = AlignRow(widths[i]) -- verified
  // against plume's toD3D12() -- so these columns are exactly the texels the
  // shader samples. If the guest scan is clean and this one is clean too, the
  // zeros are entering somewhere after the copy is recorded.
  if (diagnose) {
    static const char* kStaging[4] = {"staging Y", "staging Cr", "staging Cb", "staging A"};
    for (int i = 0; i < 4; ++i) {
      if (!frame.planes[i] || !widths[i] || !heights[i]) continue;
      ScanZeroColumns(kStaging[i], mapped + offsets[i], widths[i], heights[i],
                      AlignRow(widths[i]));
    }
  }
  bink_upload_->unmap();

  for (int i = 0; i < 4; ++i) {
    if (!frame.planes[i] || widths[i] == 0 || heights[i] == 0) continue;

    if (!bink_textures_[i] || bink_texture_width_[i] != widths[i] ||
        bink_texture_height_[i] != heights[i]) {
      auto desc = plume::RenderTextureDesc::Texture2D(widths[i], heights[i], 1,
                                                      plume::RenderFormat::R8_UNORM);
      bink_textures_[i] = device_->createTexture(desc);
      if (!bink_textures_[i]) return false;
      bink_views_[i] = bink_textures_[i]->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM));
      bink_texture_width_[i] = widths[i];
      bink_texture_height_[i] = heights[i];
      bink_descriptors_->setTexture(uint32_t(i), bink_textures_[i].get(),
                                    plume::RenderTextureLayout::SHADER_READ, bink_views_[i].get());
    }

    command_list_->barriers(plume::RenderBarrierStage::COPY,
                            plume::RenderTextureBarrier(bink_textures_[i].get(),
                                                        plume::RenderTextureLayout::COPY_DEST));
    auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
        bink_upload_.get(), plume::RenderFormat::R8_UNORM, widths[i], heights[i], 1,
        AlignRow(widths[i]), offsets[i]);
    auto dst_loc = plume::RenderTextureCopyLocation::Subresource(bink_textures_[i].get(), 0, 0);
    command_list_->copyTextureRegion(dst_loc, src, 0, 0, 0, nullptr);
    command_list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                            plume::RenderTextureBarrier(bink_textures_[i].get(),
                                                        plume::RenderTextureLayout::SHADER_READ));
  }
  return true;
}

void KameoGraphicsSystem::DrawBink(plume::RenderCommandList* list) {
  // CONSUME the pending frame rather than copying it.
  //
  // Draw_Bink_textures submits one frame per present while a video is playing,
  // so holding onto the last one meant that as soon as playback stopped -- which
  // is exactly what happens on pressing Start -- we kept re-drawing that final
  // frame underneath everything, frozen. Consuming it means a frame is drawn
  // only when the guest actually produced one, and the background reverts to the
  // clear colour when the video ends.
  BinkFrame frame;
  {
    std::lock_guard<std::mutex> lock(bink_mutex_);
    frame = bink_pending_;
    bink_pending_ = BinkFrame{};
  }
  if (!frame.valid || !EnsureBinkResources()) {
    return;
  }

  // Plane 3 (alpha) may be absent; point its descriptor at the luma texture so
  // the slot is always bound, and let the shader ignore it.
  if (!frame.planes[3] && bink_textures_[0]) {
    bink_descriptors_->setTexture(3, bink_textures_[0].get(),
                                  plume::RenderTextureLayout::SHADER_READ, bink_views_[0].get());
  }

  if (!UploadBinkPlanes(frame)) {
    return;
  }

  struct Constants {
    float yuv_to_rgb[16];
    uint32_t has_alpha;
    uint32_t padding[3];
  } constants{};
  std::memcpy(constants.yuv_to_rgb, frame.yuv_to_rgb, sizeof(constants.yuv_to_rgb));
  constants.has_alpha = frame.has_alpha ? 1u : 0u;
  if (void* mapped = bink_constants_->map()) {
    std::memcpy(mapped, &constants, sizeof(constants));
    bink_constants_->unmap();
  }

  if (void* mapped = bink_vertices_->map()) {
    std::memcpy(mapped, frame.vertices, sizeof(frame.vertices));
    bink_vertices_->unmap();
  }

  const plume::RenderVertexBufferView view(bink_vertices_.get(), sizeof(frame.vertices));
  const plume::RenderInputSlot slot(0, sizeof(float) * 5);
  list->setPipeline(bink_pipeline_.get());
  list->setGraphicsPipelineLayout(bink_layout_.get());
  list->setGraphicsDescriptorSet(bink_descriptors_.get(), 0);
  list->setVertexBuffers(0, &view, 1, &slot);
  list->drawInstanced(4, 1, 0, 0);
}

// ============================================================================
// 2D overlay (title-screen text)
// ============================================================================

// ============================================================================
// Translated shader modules
// ============================================================================
//
// Proves the cache blobs actually load into the real device before any of the
// draw path is built on them -- a wrong entry point or a malformed blob would
// otherwise surface much later, buried in a pipeline creation failure.

// ============================================================================
// Bindless draw path foundation
// ============================================================================
//
// The translated shaders are bindless, and the layout below is dictated by them
// rather than chosen (see SHADERS.md):
//
//   Texture2D   g_Texture2DDescriptorHeap[]   : register(t0, space0)
//   Texture3D   g_Texture3DDescriptorHeap[]   : register(t0, space1)
//   TextureCube g_TextureCubeDescriptorHeap[] : register(t0, space2)
//   SamplerState g_SamplerDescriptorHeap[]    : register(s0, space3)
//   cbuffer VertexShaderConstants : register(b0, space4)
//   cbuffer PixelShaderConstants  : register(b1, space4)
//   cbuffer SharedConstants       : register(b2, space4)
//
// plume maps one descriptor set per space, and expresses unbounded arrays with
// RenderDescriptorSetDesc::lastRangeIsBoundless.

bool KameoGraphicsSystem::EnsureDrawPathResources() {
  if (draw_path_ready_) {
    return true;
  }
  if (draw_path_failed_ || !device_) {
    return false;
  }

  // Bindless needs descriptor indexing; fail loudly rather than producing a
  // layout that silently misbehaves.
  if (!device_->getCapabilities().descriptorIndexing) {
    REXLOG_ERROR("[kameo-gfx] device does not support descriptor indexing; bindless path unavailable");
    draw_path_failed_ = true;
    return false;
  }

  constexpr uint32_t kMaxTextures = 4096;
  constexpr uint32_t kMaxSamplers = 256;

  const plume::RenderDescriptorRange tex2d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange tex3d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange cube(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange samp(plume::RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);

  // Set index == HLSL space, so the order here is load-bearing.
  const plume::RenderDescriptorSetDesc set_descs[] = {
      plume::RenderDescriptorSetDesc(&tex2d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&tex3d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&cube, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&samp, 1, true, kMaxSamplers),
  };

  // The three space4 constant buffers are ROOT descriptors, not a descriptor
  // set. Every draw needs its own copy of the 256-register blocks, and a
  // descriptor set binds one whole buffer -- rebinding it per draw would mean a
  // descriptor set per draw. A root CBV takes a buffer address instead, so the
  // draw path suballocates each draw's constants out of one upload ring and
  // points the root descriptor at the offset.
  const plume::RenderRootDescriptorDesc root_descs[] = {
      plume::RenderRootDescriptorDesc(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
  };

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = set_descs;
  layout_desc.descriptorSetDescsCount = uint32_t(std::size(set_descs));
  layout_desc.rootDescriptorDescs = root_descs;
  layout_desc.rootDescriptorDescsCount = uint32_t(std::size(root_descs));
  layout_desc.allowInputLayout = true;
  draw_layout_ = device_->createPipelineLayout(layout_desc);
  if (!draw_layout_) {
    REXLOG_ERROR("[kameo-gfx] bindless pipeline layout creation failed");
    draw_path_failed_ = true;
    return false;
  }

  draw_texture_set_ = device_->createDescriptorSet(set_descs[0]);
  draw_texture3d_set_ = device_->createDescriptorSet(set_descs[1]);
  draw_cube_set_ = device_->createDescriptorSet(set_descs[2]);
  draw_sampler_set_ = device_->createDescriptorSet(set_descs[3]);

  if (!draw_texture_set_ || !draw_texture3d_set_ || !draw_cube_set_ || !draw_sampler_set_) {
    REXLOG_ERROR("[kameo-gfx] bindless descriptor sets failed");
    draw_path_failed_ = true;
    return false;
  }

  draw_path_ready_ = true;
  REXLOG_INFO("[kameo-gfx] bindless draw path ready ({} texture slots, {} samplers)", kMaxTextures,
              kMaxSamplers);
  return true;
}

bool KameoGraphicsSystem::GetDrawPathContext(DrawPathContext* out) {
  if (!out || !draw_path_ready_ || !device_) {
    return false;
  }
  out->device = device_.get();
  out->layout = draw_layout_.get();
  out->texture_set = draw_texture_set_.get();
  out->texture3d_set = draw_texture3d_set_.get();
  out->cube_set = draw_cube_set_.get();
  out->sampler_set = draw_sampler_set_.get();
  out->color_format = uint32_t(kColorFormat);
  out->depth_format = uint32_t(kDepthFormat);
  out->spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  return true;
}

plume::RenderShader* KameoGraphicsSystem::ShaderModule(const ShaderCacheEntry* entry) {
  if (!entry) {
    return nullptr;
  }
  auto found = shader_modules_.find(entry);
  return found != shader_modules_.end() ? found->second.get() : nullptr;
}

void KameoGraphicsSystem::QueueShaderModule(const ShaderCacheEntry* entry) {
  if (!entry) {
    return;
  }
  std::lock_guard<std::mutex> lock(shader_mutex_);
  shader_pending_.push_back(entry);
}

void KameoGraphicsSystem::EnsureShaderModules() {
  std::vector<const ShaderCacheEntry*> pending;
  {
    std::lock_guard<std::mutex> lock(shader_mutex_);
    pending.swap(shader_pending_);
  }
  if (pending.empty() || !device_) {
    return;
  }

  const bool spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  for (const ShaderCacheEntry* entry : pending) {
    if (shader_modules_.count(entry)) {
      continue;
    }
    // The caches are only actually compressed when the generator had zstandard
    // available; here the sizes are equal, so the blobs are used directly.
    const uint8_t* blob =
        spirv ? g_compressedSpirvCache + entry->spirvOffset : g_compressedDxilCache + entry->dxilOffset;
    const uint32_t size = spirv ? entry->spirvSize : entry->dxilSize;
    if (size == 0) {
      ++shader_module_failed_;
      continue;
    }
    // XenosRecomp emits "main" for both stages.
    auto module = device_->createShader(blob, size, "main",
                                        spirv ? plume::RenderShaderFormat::SPIRV
                                              : plume::RenderShaderFormat::DXIL);
    if (module) {
      ++shader_module_ok_;
      shader_modules_.emplace(entry, std::move(module));
    } else {
      ++shader_module_failed_;
    }
  }

  static uint32_t reported = 0;
  if (shader_module_ok_ + shader_module_failed_ >= reported + 200) {
    reported = shader_module_ok_ + shader_module_failed_;
    REXLOG_INFO("[kameo-gfx] shader modules: {} created, {} failed ({})", shader_module_ok_,
                shader_module_failed_, spirv ? "spirv" : "dxil");
  }
}

void KameoGraphicsSystem::SubmitOverlay(std::vector<OverlayBatch>&& batches) {
  std::lock_guard<std::mutex> lock(overlay_mutex_);
  overlay_pending_ = std::move(batches);
}

namespace {

// Xbox 360 2D texture tiling. This is the standard XGAddress2DTiledOffset
// swizzle: texels are stored in 32x32 macro tiles with a further micro-tile
// shuffle inside them, so a tiled surface cannot be read row-by-row at all.
//
// Returns a TEXEL index; multiply by the texel size for a byte offset. The
// trailing `>> log2_bpp` is what converts the byte-domain arithmetic back into
// texel units, so do not drop it.
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t log2_bpp) {
  const uint32_t aligned_width = (width + 31u) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpp + 7);
  const uint32_t micro = ((x & 7u) + ((y & 6u) << 2)) << log2_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                          ((y & 8u) << (3 + log2_bpp)) + ((y & 1u) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
           ((y & 16u) << 7) + (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >>
          log2_bpp);
}

}  // namespace

bool KameoGraphicsSystem::EnsureOverlayResources() {
  if (overlay_pipeline_) {
    return true;
  }
  if (overlay_failed_ || !device_) {
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  overlay_sampler_ = device_->createSampler(sampler_desc);

  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  overlay_layout_ = device_->createPipelineLayout(layout_desc);

  const plume::RenderShaderFormat format = interface_->getCapabilities().shaderFormat;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
  if (format == plume::RenderShaderFormat::SPIRV) {
    vs = device_->createShader(shaders::g_overlayVSSpirv, sizeof(shaders::g_overlayVSSpirv),
                               "VSMain", format);
    ps = device_->createShader(shaders::g_overlayPSSpirv, sizeof(shaders::g_overlayPSSpirv),
                               "PSMain", format);
  } else {
    vs = device_->createShader(shaders::g_overlayVSDxil, sizeof(shaders::g_overlayVSDxil), "VSMain",
                               format);
    ps = device_->createShader(shaders::g_overlayPSDxil, sizeof(shaders::g_overlayPSDxil), "PSMain",
                               format);
  }
  if (!vs || !ps) {
    REXLOG_ERROR("[kameo-gfx] overlay shader creation failed");
    overlay_failed_ = true;
    return false;
  }

  static const plume::RenderInputSlot slot(0, sizeof(OverlayVertex));
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, 0),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0,
                                sizeof(float) * 2),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R32G32B32A32_FLOAT, 0,
                                sizeof(float) * 4),
  };

  plume::RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.inputSlots = &slot;
  pipeline_desc.inputSlotsCount = 1;
  pipeline_desc.inputElements = elements;
  pipeline_desc.inputElementsCount = uint32_t(std::size(elements));
  pipeline_desc.pipelineLayout = overlay_layout_.get();
  pipeline_desc.vertexShader = vs.get();
  pipeline_desc.pixelShader = ps.get();
  pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  // Text is antialiased coverage, so it has to composite rather than overwrite.
  pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  overlay_pipeline_ = device_->createGraphicsPipeline(pipeline_desc);
  if (!overlay_pipeline_) {
    REXLOG_ERROR("[kameo-gfx] overlay pipeline creation failed");
    overlay_failed_ = true;
    return false;
  }

  REXLOG_INFO("[kameo-gfx] overlay pipeline ready ({})",
              format == plume::RenderShaderFormat::SPIRV ? "spirv" : "dxil");
  return true;
}

plume::RenderDescriptorSet* KameoGraphicsSystem::EnsureOverlayAtlas(const OverlayBatch& batch) {
  if (!batch.atlas || !batch.atlas_width || !batch.atlas_height) {
    return nullptr;
  }

  // Cached on the guest address: untiling a 2048-wide atlas costs megabytes of
  // shuffling for data that does not change, and the title screen alternates
  // between several atlases within a single frame.
  auto found = overlay_atlases_.find(batch.atlas_address);
  if (found != overlay_atlases_.end() && found->second.width == batch.atlas_width &&
      found->second.height == batch.atlas_height) {
    return found->second.descriptors.get();
  }

  const uint32_t width = batch.atlas_width;
  const uint32_t height = batch.atlas_height;
  overlay_untiled_.assign(size_t(width) * height, 0);

  if (batch.atlas_tiled) {
    // k_8: one byte per texel, so log2_bpp is 0.
    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* dst = overlay_untiled_.data() + size_t(y) * width;
      for (uint32_t x = 0; x < width; ++x) {
        dst[x] = batch.atlas[TiledOffset2D(x, y, batch.atlas_pitch, 0)];
      }
    }
  } else {
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(overlay_untiled_.data() + size_t(y) * width,
                  batch.atlas + size_t(y) * batch.atlas_pitch, width);
    }
  }

  OverlayAtlas entry;
  auto desc = plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8_UNORM);
  entry.texture = device_->createTexture(desc);
  if (!entry.texture) {
    return nullptr;
  }
  entry.view = entry.texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM));

  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
  entry.descriptors = device_->createDescriptorSet(set_desc);
  if (!entry.descriptors) {
    return nullptr;
  }
  entry.descriptors->setTexture(0, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                                entry.view.get());
  entry.descriptors->setSampler(1, overlay_sampler_.get());
  entry.width = width;
  entry.height = height;

  const uint32_t dst_pitch = AlignRow(width);
  const uint32_t total = dst_pitch * height;
  if (!overlay_upload_ || overlay_upload_size_ < total) {
    overlay_upload_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    overlay_upload_size_ = total;
    if (!overlay_upload_) return nullptr;
  }
  if (auto* mapped = static_cast<uint8_t*>(overlay_upload_->map())) {
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(mapped + size_t(y) * dst_pitch, overlay_untiled_.data() + size_t(y) * width,
                  width);
    }
    overlay_upload_->unmap();
  } else {
    return nullptr;
  }

  command_list_->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
      overlay_upload_.get(), plume::RenderFormat::R8_UNORM, width, height, 1, dst_pitch, 0);
  auto dst_loc = plume::RenderTextureCopyLocation::Subresource(entry.texture.get(), 0, 0);
  command_list_->copyTextureRegion(dst_loc, src, 0, 0, 0, nullptr);
  command_list_->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::SHADER_READ));

  REXLOG_INFO("[kameo-gfx] overlay atlas {:08X} {}x{} (pitch {}, tiled={}) untiled and uploaded",
              batch.atlas_address, width, height, batch.atlas_pitch, batch.atlas_tiled);

  // Dump the untiled result once. Whether the untiler is right is not something
  // to reason about from the rendered text -- a clean font sheet here settles
  // it, and a shredded one points straight at TiledOffset2D.
  if (FILE* f = fopen("overlay_atlas.pgm", "wb")) {
    fprintf(f, "P5\n%u %u\n255\n", width, height);
    fwrite(overlay_untiled_.data(), 1, overlay_untiled_.size(), f);
    fclose(f);
    REXLOG_INFO("[kameo-gfx] wrote overlay_atlas.pgm");
  }
  auto inserted = overlay_atlases_.insert_or_assign(batch.atlas_address, std::move(entry));
  return inserted.first->second.descriptors.get();
}

void KameoGraphicsSystem::DrawOverlay(plume::RenderCommandList* list) {
  std::vector<OverlayBatch> batches;
  {
    std::lock_guard<std::mutex> lock(overlay_mutex_);
    batches.swap(overlay_pending_);
  }
  if (batches.empty() || !EnsureOverlayResources()) {
    return;
  }

  // All runs share one vertex buffer; each draws its own slice of it. Runs are
  // kept in submission order rather than merged per texture, because the glyphs
  // alpha-blend and reordering them would change the result.
  uint32_t total = 0;
  for (const OverlayBatch& batch : batches) {
    total += uint32_t(batch.vertices.size());
  }
  if (total == 0) {
    return;
  }

  const uint32_t bytes = total * sizeof(OverlayVertex);
  if (!overlay_vertices_ || overlay_vertex_capacity_ < total) {
    overlay_vertices_ = device_->createBuffer(
        plume::RenderBufferDesc::VertexBuffer(bytes, plume::RenderHeapType::UPLOAD));
    overlay_vertex_capacity_ = total;
    if (!overlay_vertices_) return;
  }
  if (auto* mapped = static_cast<uint8_t*>(overlay_vertices_->map())) {
    uint32_t offset = 0;
    for (const OverlayBatch& batch : batches) {
      const uint32_t n = uint32_t(batch.vertices.size()) * sizeof(OverlayVertex);
      std::memcpy(mapped + offset, batch.vertices.data(), n);
      offset += n;
    }
    overlay_vertices_->unmap();
  } else {
    return;
  }

  const plume::RenderVertexBufferView view(overlay_vertices_.get(), bytes);
  const plume::RenderInputSlot slot(0, sizeof(OverlayVertex));
  list->setPipeline(overlay_pipeline_.get());
  list->setGraphicsPipelineLayout(overlay_layout_.get());
  list->setVertexBuffers(0, &view, 1, &slot);

  uint32_t first = 0;
  uint32_t drawn = 0;
  for (const OverlayBatch& batch : batches) {
    const uint32_t n = uint32_t(batch.vertices.size());
    if (plume::RenderDescriptorSet* set = EnsureOverlayAtlas(batch)) {
      list->setGraphicsDescriptorSet(set, 0);
      list->drawInstanced(n, 1, first, 0);
      ++drawn;
    }
    first += n;
  }

  if ((overlay_draw_count_++ % 300) == 0) {
    REXLOG_INFO("[kameo-gfx] overlay: {} glyphs across {} atlas runs ({} drawn)", total / 6,
                batches.size(), drawn);
  }
}

}  // namespace kameo::gfx
