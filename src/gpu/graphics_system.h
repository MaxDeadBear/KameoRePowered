#pragma once

// The native renderer: a plume-backed rex::system::IGraphicsSystem injected through RuntimeConfig::graphics in place of the xenos plugin.
// It does not emulate the guest GPU; draws arrive through hooks on the game's own D3D entry points (gpu/hooks/), as in UnleashedRecomp.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/system/interfaces/graphics.h>

#include "gpu/config.h"
#include "gpu/movie/bink_frame.h"
#include "gpu/shaders/shader_registry.h"

namespace plume {
struct RenderInterface;
struct RenderDevice;
struct RenderCommandQueue;
struct RenderSwapChain;
struct RenderCommandList;
struct RenderCommandFence;
struct RenderCommandSemaphore;
struct RenderFramebuffer;
struct RenderPipeline;
struct RenderPipelineLayout;
struct RenderDescriptorSet;
struct RenderSampler;
struct RenderBuffer;
struct RenderTexture;
struct RenderTextureView;
struct RenderShader;
struct RenderQueryPool;
}  // namespace plume

// Declared in the generated shader_cache.h, at global scope.
struct ShaderCacheEntry;

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace kameo::gpu {

void* NativeWindowHandle(rex::ui::Window* window);

// How many frames the CPU may run ahead of the GPU; the upload rings are sliced by it, so raising it needs the ring sizes raised with it.
inline constexpr uint32_t kFramesInFlight = 2;
inline constexpr uint32_t kSwapChainTextureCount = 3;
// The render thread's name, which the sample profiler finds it by.
inline constexpr wchar_t kRenderThreadName[] = L"Kameo render";

// One address per physical byte: the A/C/E views alias the same physical memory, and the E view sits 4 KB further in, so reads recorded through one view match a free through another.
inline uint32_t CanonicalGuestAddress(uint32_t address) {
  if (address < 0xA0000000u) return address;
  return 0xA0000000u + (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// One vertex of the ImGui UI, already in clip space.
struct UiVertex {
  float x, y;
  float u, v;
  float r, g, b, a;
};

class GraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  // `get_native_window` is called lazily because RuntimeConfig::graphics is populated in OnPreSetup, before the window exists.
  explicit GraphicsSystem(std::function<void*()> get_native_window, Backend backend = Backend::Auto);
  ~GraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher, rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return device_ != nullptr; }
  void Shutdown() override;

  // Null on purpose: these hand out the SDK's own presenter and overlay objects, which a plume-backed system does not own, and ReXApp tolerates null.
  rex::ui::GraphicsProvider* provider() const override { return nullptr; }
  rex::ui::Presenter* presenter() const override { return nullptr; }

  plume::RenderDevice* device() const { return device_.get(); }
  // The draw path's device-removal and info-queue diagnostics are D3D12-only and cast RenderDevice* to plume::D3D12Device*.
  bool using_vulkan() const { return using_vulkan_; }
  plume::RenderCommandQueue* queue() const { return queue_.get(); }
  plume::RenderSwapChain* swap_chain() const { return swap_chain_.get(); }
  // Exclusive fullscreen on D3D12: the render thread enters it once the swap chain matches the window, and leaves it before any swap chain rebuild.
  void SetExclusiveFullscreen(bool wanted) { exclusive_wanted_.store(wanted); }
  bool ExclusiveFullscreenActive() const { return exclusive_active_.load(); }

  // Acquire, record the captured frame, submit and present; runs on the render thread only, so the guest captures frame N+1 while frame N is recorded.
  void PresentClear();

  // Blocks until the render thread has started the previously handed-over frame, so at most one frame waits behind the one being recorded.
  void WaitForRenderSlot();
  // Hands the frame just published by SubmitCapturedFrame to the render thread; `guest_fence` is the guest's CPU fence at that moment, `readback_word` the guest's fence read-back word advanced once the frame is recorded, and `guest_reads` the canonical guest addresses recording it may read.
  void KickRenderThread(uint32_t guest_fence, uint32_t* readback_word, std::vector<uint32_t> guest_reads);
  // Blocks until every handed-over frame has been recorded, which is when the guest memory it names has been read.
  void WaitForRenderIdle(const char* reason);
  // Blocks only until no handed-over frame still to be recorded reads a canonical guest address in [begin, end).
  void WaitForRenderReads(uint32_t begin, uint32_t end, const char* reason);
  // True while a fence from an already handed-over frame is still being recorded; fences of the frame still being captured are never pending.
  bool IsGuestFencePending(uint32_t fence) const;
  // Runs the UI callback on the calling (guest) thread at present time, so dialogs touch guest state from the guest.
  void RunUiCallback();
  // Called by the draw path once it has taken a captured frame, releasing a guest waiting in WaitForRenderSlot.
  void NotifyCapturedFrameTaken();

  struct FrameStats {
    double fps = 0.0;
    double average_ms = 0.0;
    double worst_ms = 0.0;
  };
  // Frames actually presented to the swap chain over the last second, measured on the render thread at each present.
  FrameStats GetFrameStats();
  // The size of the swap chain as of the last present, or 0 by 0 before the first.
  void WindowSize(uint32_t* width, uint32_t* height) const { *width = ui_width_.load(std::memory_order_relaxed); *height = ui_height_.load(std::memory_order_relaxed); }
  // Where the scene lands in a window of this size and the size it renders at: scaled evenly and centred, so a render size of another shape gets black bars rather than a stretch.
  void SceneRectInWindow(uint32_t window_width, uint32_t window_height, float* x, float* y, float* width, float* height, uint32_t* scene_width, uint32_t* scene_height) const;

  // Width over height of what is actually presented, for the widescreen projection hook. 16:9 until the swap chain exists.
  double DisplayAspect() const;

  // Hands over a decoded Bink frame; drawn by the next PresentClear.
  void SubmitBinkFrame(const BinkFrame& frame);
  // Draws the pending Bink frame into an already-recording list at the point the guest blitted it; the present-time fallback then does nothing.
  void DrawPendingBink(plume::RenderCommandList* list, uint32_t samples);
  uint32_t SceneSamples() const { return SceneSampleCount(); }

  // The app drives the ImGui drawer itself (a plume-backed system exposes no presenter) and hands its geometry here, already in clip space.
  struct UiDraw {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t texture_id = 0;  // from CreateUiTexture; 0 draws nothing
    bool scissor = false;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    int32_t scissor_w = 0;
    int32_t scissor_h = 0;
  };
  struct UiFrame {
    std::vector<UiVertex> vertices;
    std::vector<UiDraw> draws;
  };
  // Takes RGBA8 and keeps all four channels, since the achievement icons carry their own colour; callable from any thread, uploaded at present.
  uint32_t CreateUiTexture(uint32_t width, uint32_t height, const uint8_t* rgba);
  void SubmitUi(UiFrame&& frame);
  // Called once per presented frame with the render target size, just before the UI geometry is consumed; the app runs the ImGui drawer here.
  void SetUiFrameCallback(std::function<void(uint32_t, uint32_t)> callback);

  // Queues a resolved shader for module creation; called from the guest-thread creation hooks, built later on the render thread.
  void QueueShaderModule(const ShaderCacheEntry* entry);

  // What the draw path (gpu/draw, gpu/replay) may touch: the device, the bindless layout and the scene targets of the frame being recorded.
  struct DrawPathContext {
    plume::RenderDevice* device = nullptr;
    plume::RenderPipelineLayout* layout = nullptr;
    plume::RenderDescriptorSet* texture_set = nullptr;
    plume::RenderDescriptorSet* texture3d_set = nullptr;
    plume::RenderDescriptorSet* cube_set = nullptr;
    plume::RenderDescriptorSet* sampler_set = nullptr;
    // The scene's own depth attachment, so a depth resolve taken while the colour target is bound copies the depth the scene was drawn with.
    plume::RenderTexture* scene_depth = nullptr;
    // With MSAA the scene target is multisampled: draws into it need that sample count, and anything reading it goes through the resolved copy.
    uint32_t scene_samples = 1;
    plume::RenderTexture* scene_resolved = nullptr;
    // Bumped whenever the scene targets are recreated, since a new texture can land at the address of the one it replaced.
    uint64_t scene_target_generation = 0;
    // plume::RenderFormat values, carried as integers because the scoped enum cannot be forward declared.
    uint32_t color_format = 0;
    uint32_t depth_format = 0;
    // Which set of per-frame CPU-written resources this frame owns; the upload rings are sliced by it and the texture graveyard retires against it.
    uint32_t frame_slot = 0;
    uint64_t frame_index = 0;
    bool spirv = false;
  };
  // False until the bindless layout exists; the caller draws nothing then.
  bool GetDrawPathContext(DrawPathContext* out);
  // The translated module for a cache entry, or null if it never got built.
  plume::RenderShader* ShaderModule(const ShaderCacheEntry* entry);

  // The active system, for the guest-function hooks; null when the native renderer is not in use.
  static GraphicsSystem* instance();

 private:
  struct FrameSlot {
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderCommandSemaphore> acquire;
    bool pending = false;  // a submission is outstanding on this slot's fence
    // Two GPU timestamps around the slot's frame; one pool per slot, since plume's Vulkan read-back wants every query in a pool complete.
    std::unique_ptr<plume::RenderQueryPool> timestamps;
    bool timestamps_written = false;
  };
  struct PendingGuestReads {
    uint64_t kick = 0;
    std::vector<uint32_t> addresses;
  };
  struct UiTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGBA, non-empty until uploaded
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderDescriptorSet> descriptors;
  };

  // graphics_system.cpp
  bool CreateInterface();

  // swap_chain.cpp; the swap chain is built lazily because ReXApp creates the window after SetupPresentation.
  bool EnsureSwapChain();
  bool CreateFrameResources();
  // Waits until every slot's submission has retired; plume's waitForCommandFence blocks forever on a second wait for one submission, hence the per-slot `pending` flag.
  void WaitForAllFrames();
  // The list the frame being recorded belongs to, which the Bink upload records into from outside PresentClear.
  plume::RenderCommandList* CurrentCommandList();
  void UpdateExclusiveFullscreen();
  void LeaveExclusiveFullscreen();
  bool ResizeSwapChainInPlace();

  // present.cpp
  bool DropStaleSwapChain();

  // frame_pacing.cpp
  void RecordPresentTime();
  void LimitFrameRate();

  // render_thread.cpp
  void RenderThreadMain();
  void PublishFinishedOcclusion();
  void StopRenderThread();

  // scene_target.cpp; the scene renders at its own resolution and is scaled onto the swap chain, so the video and UI still land at native size.
  bool EnsureSceneTarget(uint32_t width, uint32_t height, uint32_t samples);
  uint32_t SceneSampleCount() const;
  bool EnsureUpscaleResources();
  void SceneRenderSize(uint32_t window_width, uint32_t window_height, uint32_t* out_width, uint32_t* out_height) const;
  void UpscaleScene(plume::RenderCommandList* list, plume::RenderTexture* scene_texture, plume::RenderTexture* texture, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);

  // passes/gamma_pass.cpp; the 360 applies the guest's gamma ramp at scanout, so it is redrawn by hand as the last thing in the frame.
  bool EnsureGammaResources(uint32_t width, uint32_t height);
  void ApplyGamma(plume::RenderCommandList* list, plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);

  // movie/bink_renderer.cpp
  bool EnsureBinkResources();
  bool UploadBinkPlanes(const BinkFrame& frame);
  // hold_last redraws the previous video frame on a present the guest produced no new one for, and belongs only to the ordered marker path: the present-time fallback runs after the UI, where a redraw would cover it.
  void DrawBink(plume::RenderCommandList* list, bool hold_last, uint32_t samples = 1);

  // ui/ui_renderer.cpp
  bool EnsureUiPipeline();
  void DrawUi(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);

  // pipeline/bindless_layout.cpp
  bool EnsureDrawPathResources();

  // shaders/shader_modules.cpp
  void EnsureShaderModules();

  std::function<void*()> get_native_window_;
  Backend backend_ = Backend::Auto;
  bool using_vulkan_ = false;
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  uint64_t frame_index_ = 0;

  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  bool swap_chain_failed_ = false;
  // The window the live swap chain was built against; a fullscreen toggle can hand back a different handle and destroy the old one.
  void* swap_chain_window_ = nullptr;
  std::atomic<bool> exclusive_wanted_{false};
  std::atomic<bool> exclusive_active_{false};
  uint32_t exclusive_retry_frames_ = 0;
  uint32_t exclusive_entries_ = 0;
  uint32_t exclusive_held_frames_ = 0;
  bool exclusive_last_state_ = false;
  // One set of CPU-written objects per frame in flight; the wait is for the slot about to be reused, so recording frame N+1 overlaps the GPU executing frame N.
  FrameSlot frames_[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandSemaphore> present_semaphores_[kSwapChainTextureCount];
  uint32_t frame_slot_ = 0;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  // Shared by every swap chain framebuffer, standing in for the guest's EDRAM depth for a single full-resolution pass.
  std::unique_ptr<plume::RenderTexture> depth_texture_;
  std::unique_ptr<plume::RenderTextureView> depth_view_;

  std::chrono::steady_clock::time_point fps_cap_deadline_{};
  void* fps_cap_timer_ = nullptr;
  std::deque<std::chrono::steady_clock::time_point> present_times_;
  std::mutex stats_mutex_;
  FrameStats frame_stats_;

  std::thread render_thread_;
  std::mutex render_mutex_;
  std::condition_variable render_cv_;
  uint64_t render_kicks_ = 0;    // frames handed to the render thread
  uint64_t render_started_ = 0;  // frames it has begun
  uint64_t render_done_ = 0;     // frames it has finished recording and presenting
  bool render_stop_ = false;
  uint32_t render_fence_ = 0;
  uint32_t* render_readback_ = nullptr;
  std::deque<PendingGuestReads> render_reads_;
  std::atomic<uint32_t> published_fence_{0};
  std::atomic<uint32_t> completed_fence_{0};


  std::unique_ptr<plume::RenderPipelineLayout> bink_layout_;
  std::unique_ptr<plume::RenderPipeline> bink_pipeline_;
  // The same blit for a video drawn inside the multisampled scene, one per sample count and kept for the session so a frame in flight never loses its pipeline.
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> bink_pipelines_msaa_;
  std::unique_ptr<plume::RenderShader> bink_vs_;
  std::unique_ptr<plume::RenderShader> bink_ps_;
  std::unique_ptr<plume::RenderDescriptorSet> bink_descriptors_;
  const plume::RenderTexture* bink_slot3_ = nullptr;  // what the alpha binding holds
  std::unique_ptr<plume::RenderSampler> bink_sampler_;
  std::unique_ptr<plume::RenderBuffer> bink_vertices_[kFramesInFlight];
  std::unique_ptr<plume::RenderBuffer> bink_constants_;
  // Written only when a video's colour space changes, behind a full GPU wait, because its descriptor is bound once and cannot rotate.
  std::vector<uint8_t> bink_constants_shadow_;
  // One staging buffer per frame in flight, since the previous frame's copy out of it may not have run yet.
  std::unique_ptr<plume::RenderBuffer> bink_upload_[kFramesInFlight];
  std::unique_ptr<plume::RenderTexture> bink_textures_[4];
  std::unique_ptr<plume::RenderTextureView> bink_views_[4];
  uint32_t bink_texture_width_[4] = {};
  uint32_t bink_texture_height_[4] = {};
  uint32_t bink_upload_size_[kFramesInFlight] = {};
  bool bink_failed_ = false;
  std::mutex bink_mutex_;
  BinkFrame bink_pending_;
  // The last frame the guest produced, redrawn on presents it had nothing new for.
  BinkFrame bink_last_;
  uint32_t bink_held_ = 0;
  bool bink_in_scene_ = false;
  static constexpr uint32_t kBinkHoldPresents = 10;

  std::unique_ptr<plume::RenderPipelineLayout> gamma_layout_;
  std::unique_ptr<plume::RenderPipeline> gamma_pipeline_;
  std::unique_ptr<plume::RenderDescriptorSet> gamma_descriptors_;
  std::unique_ptr<plume::RenderSampler> gamma_sampler_;
  std::unique_ptr<plume::RenderTexture> gamma_source_;
  std::unique_ptr<plume::RenderTextureView> gamma_source_view_;
  std::unique_ptr<plume::RenderBuffer> gamma_constants_;
  // Written only when the gamma settings change, behind a full GPU wait, because the descriptor set binding it cannot rotate.
  float gamma_constants_shadow_ = 0.0f;
  bool gamma_constants_written_ = false;
  uint32_t gamma_width_ = 0;
  uint32_t gamma_height_ = 0;
  bool gamma_failed_ = false;

  std::unique_ptr<plume::RenderTexture> scene_color_;
  std::unique_ptr<plume::RenderTextureView> scene_color_view_;
  std::unique_ptr<plume::RenderTexture> scene_depth_;
  std::unique_ptr<plume::RenderTextureView> scene_depth_view_;
  std::unique_ptr<plume::RenderFramebuffer> scene_framebuffer_;
  std::unique_ptr<plume::RenderTexture> scene_resolved_;
  std::unique_ptr<plume::RenderTextureView> scene_resolved_view_;
  uint32_t scene_width_ = 0;
  uint32_t scene_height_ = 0;
  uint32_t scene_samples_ = 1;
  uint64_t scene_target_generation_ = 0;
  uint32_t active_scene_samples_ = 1;
  bool scene_target_failed_ = false;
  // What the draw path must use as the scene's depth this frame: the swap chain's when rendering at window size, the scaled one otherwise.
  plume::RenderTexture* active_scene_depth_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> upscale_layout_;
  std::unique_ptr<plume::RenderPipeline> upscale_pipeline_;
  std::unique_ptr<plume::RenderDescriptorSet> upscale_descriptors_[kFramesInFlight];
  std::unique_ptr<plume::RenderSampler> upscale_sampler_;
  std::unique_ptr<plume::RenderBuffer> upscale_constants_;
  bool upscale_failed_ = false;

  // The bindless layout the translated shaders dictate; the space4 constant buffers are root descriptors fed from the draw path's own upload ring.
  std::unique_ptr<plume::RenderPipelineLayout> draw_layout_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_texture_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_texture3d_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_cube_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_sampler_set_;
  bool draw_path_ready_ = false;
  bool draw_path_failed_ = false;

  std::mutex shader_mutex_;
  std::vector<const ShaderCacheEntry*> shader_pending_;
  std::map<const ShaderCacheEntry*, std::unique_ptr<plume::RenderShader>> shader_modules_;
  // The same modules by cache-entry index, for the two lookups every draw makes; the map walk was 408 samples of a 15 s profile.
  std::vector<plume::RenderShader*> shader_module_by_index_;
  uint32_t shader_module_ok_ = 0;
  uint32_t shader_module_failed_ = 0;
  uint32_t shader_module_empty_ = 0;
  uint32_t shader_module_undecodable_ = 0;
  uint32_t shader_module_rejected_ = 0;

  std::unique_ptr<plume::RenderPipelineLayout> ui_layout_;
  std::unique_ptr<plume::RenderPipeline> ui_pipeline_;
  std::unique_ptr<plume::RenderSampler> ui_sampler_;
  bool ui_pipeline_failed_ = false;
  // The ImGui drawer runs on whichever thread ReXApp pumps the UI on, so the frame geometry and the texture registry cross a thread boundary.
  std::mutex ui_mutex_;
  UiFrame ui_pending_;
  bool ui_pending_fresh_ = false;
  // The frame the render thread keeps drawing until the guest submits a newer one; presents outnumber UI frames during loads and videos.
  UiFrame ui_current_;
  std::map<uint32_t, UiTexture> ui_textures_;
  uint32_t ui_next_texture_ = 1;
  std::unique_ptr<plume::RenderBuffer> ui_vertices_[kFramesInFlight];
  std::unique_ptr<plume::RenderBuffer> ui_upload_[kFramesInFlight];
  uint32_t ui_vertex_capacity_[kFramesInFlight] = {};
  uint32_t ui_upload_size_[kFramesInFlight] = {};
  std::function<void(uint32_t, uint32_t)> ui_frame_callback_;
  std::atomic<uint32_t> ui_width_{0};
  std::atomic<uint32_t> ui_height_{0};

  static GraphicsSystem* s_instance;
};

}  // namespace kameo::gpu
