#pragma once

// Native renderer skeleton: a custom rex::system::IGraphicsSystem backed by
// plume, standing in for the xenos GPU-emulation plugin.
//
// The runtime supports this directly -- RuntimeConfig::graphics takes an
// injected IGraphicsSystem and the `gpu_plugin` string is only consulted when
// that field is empty -- so no plugin DLL and no SDK modification are involved.
//
// This deliberately does NOT emulate the guest GPU. The ring-buffer entry
// points (InitializeRingBuffer / EnableReadPointerWriteBack /
// SetInterruptCallback) are optional on the interface and stay unimplemented;
// draws will arrive instead through hooks on the game's own D3D entry points
// (see kameo_guest_device.h), which is the UnleashedRecomp approach.
//
// Status: brings up a plume device, command queue and swap chain and reports
// what it selected. It does not render or present yet.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/system/interfaces/graphics.h>

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
}  // namespace plume

// Declared in the generated shader_cache.h, at global scope.
struct ShaderCacheEntry;

namespace kameo::gfx {

enum class Backend {
  Auto,    // D3D12 on Windows, Vulkan elsewhere
  D3D12,
  Vulkan,
};

// Reads the kameo_native_renderer cvar (defined in kameo_gfx_hooks.cpp).
bool NativeRendererEnabled();

// Records a checkpoint for the stall watchdog (defined in kameo_gfx_hooks.cpp).
// Lets the watchdog see WHERE inside a present we are, not just that a present
// was entered.
void GfxTrace(const char* what);

// One decoded Bink frame, captured in the Draw_Bink_textures hook and drawn
// during the next present. The planes point straight into guest memory: Bink
// decodes on the CPU into D3DFMT_LIN_L8 surfaces, so there is no tiling or
// format conversion to undo.
struct BinkFrame {
  bool valid = false;
  bool has_alpha = false;
  uint32_t luma_width = 0;
  uint32_t luma_height = 0;
  uint32_t chroma_width = 0;
  uint32_t chroma_height = 0;
  const uint8_t* planes[4] = {};  // Y, Cr, Cb, A
  uint32_t pitches[4] = {};
  float vertices[4][5] = {};      // clip-space x,y,z + u,v, triangle strip
  float yuv_to_rgb[16] = {};      // the guest's own matrix
};

// One frame of 2D overlay geometry (the title-screen text), captured in the
// D3DDevice_DrawVerticesUP hook. The guest emits one quad per glyph with
// PRE-TRANSFORMED positions, so the hook converts to clip space and expands the
// strip into a triangle list, letting every glyph in the frame go out as a
// single batched draw.
struct OverlayVertex {
  float x, y;        // clip space
  float u, v;
  float r, g, b, a;  // expanded from the guest's D3DCOLOR
};

struct OverlayBatch {
  bool valid = false;
  std::vector<OverlayVertex> vertices;
  const uint8_t* atlas = nullptr;  // guest texture memory
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  uint32_t atlas_pitch = 0;   // texels
  uint32_t atlas_address = 0; // identity for the untiled cache
  bool atlas_tiled = false;
};

class KameoGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  // `get_native_window` is called lazily during SetupPresentation rather than
  // captured up front: RuntimeConfig::graphics is populated in OnPreSetup,
  // which runs before the window exists.
  explicit KameoGraphicsSystem(std::function<void*()> get_native_window,
                               Backend backend = Backend::Auto);
  ~KameoGraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                              rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return device_ != nullptr; }
  void Shutdown() override;

  // Left null on purpose: these hand the runtime the SDK's own presenter and
  // overlay objects, which a plume-backed system does not own. ReXApp tolerates
  // null here; the ImGui overlay simply will not be wired up.
  rex::ui::GraphicsProvider* provider() const override { return nullptr; }
  rex::ui::Presenter* presenter() const override { return nullptr; }

  plume::RenderDevice* device() const { return device_.get(); }
  plume::RenderCommandQueue* queue() const { return queue_.get(); }
  plume::RenderSwapChain* swap_chain() const { return swap_chain_.get(); }

  // Acquire, clear, present. Currently the whole renderer: it proves the
  // device, swap chain and window are wired up correctly and gives us a frame
  // to hang real work off. Safe to call from the thread that hooks the game's
  // D3DDevice_Swap; not thread-safe against itself.
  void PresentClear(float r, float g, float b);

  // Hand over a decoded Bink frame; drawn by the next PresentClear.
  void SubmitBinkFrame(const BinkFrame& frame);

  // Draws the pending Bink frame, if any, into an already-recording command
  // list. Called from the draw path at the point the guest blitted it; a frame
  // is consumed by whichever call gets to it first, so the fallback in
  // PresentClear does nothing when the ordered path already drew it.
  void DrawPendingBink(plume::RenderCommandList* list);

  // Hand over this frame's 2D overlay geometry; drawn by the next PresentClear,
  // after the video so it composites on top. One entry per run of glyphs
  // sharing an atlas -- the title screen uses several, and batching them all
  // against one texture samples most of the glyphs from the wrong atlas.
  void SubmitOverlay(std::vector<OverlayBatch>&& batches);

  // Queue a resolved shader for module creation. Called from the guest-thread
  // creation hooks; the plume objects are built later on the thread that
  // presents, matching how the Bink and overlay resources are handled.
  void QueueShaderModule(const ShaderCacheEntry* entry);

  // -- General draw path -----------------------------------------------------
  //
  // kameo_draw_path.cpp owns the per-frame capture and the texture / sampler /
  // pipeline caches; it reaches the device and the bindless descriptor sets
  // through this context rather than by befriending the class, so what the draw
  // path is allowed to touch stays explicit.
  struct DrawPathContext {
    plume::RenderDevice* device = nullptr;
    plume::RenderPipelineLayout* layout = nullptr;
    plume::RenderDescriptorSet* texture_set = nullptr;
    plume::RenderDescriptorSet* texture3d_set = nullptr;
    plume::RenderDescriptorSet* cube_set = nullptr;
    plume::RenderDescriptorSet* sampler_set = nullptr;
    // plume::RenderFormat is a scoped enum with no fixed underlying type, so it
    // cannot be forward declared; carrying the two render target formats as
    // plain values keeps this header free of plume's types.
    uint32_t color_format = 0;
    uint32_t depth_format = 0;
    bool spirv = false;
  };
  // False until the bindless layout exists; the caller draws nothing then.
  bool GetDrawPathContext(DrawPathContext* out);
  // The translated module for a cache entry, or null if it never got built.
  plume::RenderShader* ShaderModule(const ShaderCacheEntry* entry);

  // Process-wide accessor so the guest-function hooks can reach the active
  // system. Null when the native renderer is not in use.
  static KameoGraphicsSystem* instance();


 private:
  bool CreateInterface();
  bool CreateFrameResources();
  // Waits only when a submission is actually outstanding. plume's
  // waitForCommandFence is a bare WaitForSingleObjectEx(INFINITE) on an
  // auto-reset event, so waiting twice for one submission blocks forever.
  void WaitForGpu();
  bool EnsureBinkResources();
  bool UploadBinkPlanes(const BinkFrame& frame);
  void DrawBink(plume::RenderCommandList* list);
  bool EnsureOverlayResources();
  // Untiles and uploads on first sight of an atlas, then reuses it. Returns the
  // descriptor set to bind, or null if the atlas could not be prepared.
  plume::RenderDescriptorSet* EnsureOverlayAtlas(const OverlayBatch& batch);
  void DrawOverlay(plume::RenderCommandList* list);
  // Built lazily: ReXApp creates the window AFTER calling SetupPresentation.
  bool EnsureSwapChain();

  std::function<void*()> get_native_window_;
  Backend backend_ = Backend::Auto;
  bool using_vulkan_ = false;
  bool swap_chain_failed_ = false;
  bool fence_pending_ = false;
  uint64_t frame_index_ = 0;

  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::unique_ptr<plume::RenderCommandList> command_list_;
  std::unique_ptr<plume::RenderCommandFence> fence_;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphore_;
  std::unique_ptr<plume::RenderCommandSemaphore> present_semaphore_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  // Shared by every framebuffer: the scene draws are depth tested against it
  // and it is cleared once per frame, which is all the guest's own EDRAM depth
  // buffer amounts to for a single full-resolution pass.
  std::unique_ptr<plume::RenderTexture> depth_texture_;
  std::unique_ptr<plume::RenderTextureView> depth_view_;

  // Bink blit resources, created on first use.
  std::unique_ptr<plume::RenderPipelineLayout> bink_layout_;
  std::unique_ptr<plume::RenderPipeline> bink_pipeline_;
  std::unique_ptr<plume::RenderDescriptorSet> bink_descriptors_;
  std::unique_ptr<plume::RenderSampler> bink_sampler_;
  std::unique_ptr<plume::RenderBuffer> bink_vertices_;
  std::unique_ptr<plume::RenderBuffer> bink_constants_;
  std::unique_ptr<plume::RenderBuffer> bink_upload_;
  std::unique_ptr<plume::RenderTexture> bink_textures_[4];
  std::unique_ptr<plume::RenderTextureView> bink_views_[4];
  uint32_t bink_texture_width_[4] = {};
  uint32_t bink_texture_height_[4] = {};
  uint32_t bink_upload_size_ = 0;
  bool bink_failed_ = false;

  // 2D overlay resources, created on first use.
  struct OverlayAtlas {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderDescriptorSet> descriptors;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  std::unique_ptr<plume::RenderPipelineLayout> overlay_layout_;
  std::unique_ptr<plume::RenderPipeline> overlay_pipeline_;
  std::unique_ptr<plume::RenderSampler> overlay_sampler_;
  std::unique_ptr<plume::RenderBuffer> overlay_vertices_;
  std::unique_ptr<plume::RenderBuffer> overlay_upload_;
  std::vector<uint8_t> overlay_untiled_;
  // Keyed on the guest texture address; untiling a 2048-wide atlas is far too
  // expensive to redo per frame for data that does not change.
  std::map<uint32_t, OverlayAtlas> overlay_atlases_;
  uint32_t overlay_vertex_capacity_ = 0;
  uint32_t overlay_upload_size_ = 0;
  uint32_t overlay_draw_count_ = 0;
  bool overlay_failed_ = false;

  // Bindless draw path. The translated shaders declare unbounded descriptor
  // arrays in spaces 0-2 (textures) and 3 (samplers), plus three constant
  // buffers in space 4, so the pipeline layout has to match that exactly before
  // any real geometry can be drawn.
  bool EnsureDrawPathResources();
  std::unique_ptr<plume::RenderPipelineLayout> draw_layout_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_texture_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_texture3d_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_cube_set_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_sampler_set_;
  // The three space4 constant buffers are root descriptors rather than a
  // descriptor set, so the per-draw blocks live in the draw path's own upload
  // ring; nothing for them is owned here.
  bool draw_path_ready_ = false;
  bool draw_path_failed_ = false;

  // Translated shader modules, created lazily from the cache blobs.
  void EnsureShaderModules();
  std::mutex shader_mutex_;
  std::vector<const ShaderCacheEntry*> shader_pending_;
  std::map<const ShaderCacheEntry*, std::unique_ptr<plume::RenderShader>> shader_modules_;
  uint32_t shader_module_ok_ = 0;
  uint32_t shader_module_failed_ = 0;

  std::mutex bink_mutex_;
  BinkFrame bink_pending_;
  std::mutex overlay_mutex_;
  std::vector<OverlayBatch> overlay_pending_;

  static KameoGraphicsSystem* s_instance;
};

// -- General draw path (kameo_draw_path.cpp) ---------------------------------
//
// Capture runs on the guest thread inside the draw hooks; submission runs on
// the presenting thread. Everything the guest owns -- vertices, indices,
// constants, fetch constants -- is copied at capture time, because the guest is
// free to overwrite any of it later in the same frame.

// Called from the D3DDevice_DrawIndexedVertices / DrawVertices hooks.
void CaptureIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex,
                        uint32_t start_index, uint32_t index_count);
void CaptureDraw(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t start_vertex,
                 uint32_t vertex_count);

// Called from the D3DDevice_DrawVerticesUP hook. Same capture as any other
// draw; the only difference is that the vertex data arrives as a pointer rather
// than through a bound stream.
// Captures a standalone D3DDevice_BeginVertices draw: state now, geometry at
// frame end (the guest has not written the vertices yet, and EndVertices is
// inlined so there is nothing to hook on the far side).
void CaptureDrawBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive,
                              uint32_t vertex_count, uint32_t ring, uint32_t stride);

void CaptureDrawUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count,
                   uint32_t data, uint32_t stride);

// Called from the Draw_Bink_textures hook, so the video composites in guest
// order rather than always underneath the scene.
void CaptureBinkMarker();

// Called from the D3DDevice_Resolve hook. Recorded in frame order alongside the
// draws, because a resolve copies whatever has been rendered up to that point.
void CaptureResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t source_rect,
                    uint32_t dest_texture);

// Called from the Swap hook: hands the frame just captured to the presenter and
// starts a new one.
void SubmitCapturedFrame();

// Called from PresentClear, on the presenting thread.
void DrawCapturedScene(KameoGraphicsSystem& gfx, plume::RenderCommandList* list,
                       plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer,
                       uint32_t width, uint32_t height);

// Turns on the D3D12 debug layer. Must run BEFORE the device is created, and
// only does anything when the optional Graphics Tools feature is installed.
// plume enables its own layer only in debug builds, and it ignores the HRESULT
// from CreateGraphicsPipelineState, so without this a rejected pipeline is
// completely silent.
void EnableD3D12DebugLayer();

// Drops the texture / sampler / pipeline caches. Must run before the device
// goes away.
void ReleaseDrawPathCaches();

// Resolves a guest shader object to its translated cache entry. Implemented in
// kameo_gfx_hooks.cpp, which owns the creation-time bindings.
const ShaderCacheEntry* LookupShaderBinding(uint32_t guest_object);

}  // namespace kameo::gfx
