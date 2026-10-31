#include "gpu/pipeline/pso_precache.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache_data.h"
#include "gpu/shaders/shader_cache.h"

namespace kameo::gpu::detail {
namespace {

// The fallback when no worker can start: the render thread takes this long a frame, so frames keep going out.
constexpr std::chrono::milliseconds kPrecacheSlice{32};

struct PrecacheJob {
  DrawCall call;
  plume::RenderShader* vs = nullptr;
  plume::RenderShader* ps = nullptr;
  uint32_t samples = 1;
};

bool g_pipelinePrecacheQueued = false;
bool g_pipelinePrecacheComplete = false;
bool g_precacheStarted = false;
std::atomic<bool> g_precacheRunning{false};
std::atomic<bool> g_precacheStop{false};
std::atomic<uint32_t> g_jobsTotal{0};
std::atomic<uint32_t> g_jobsDone{0};
std::atomic<size_t> g_nextJob{0};
std::atomic<bool> g_gamePictureShown{false};
std::vector<PrecacheJob> g_precacheJobs;
GraphicsSystem::DrawPathContext g_precacheContext;
std::chrono::steady_clock::time_point g_precacheStart;
uint32_t g_precacheMissing = 0;
uint32_t g_failuresBefore = 0;
size_t g_pipelinesBefore = 0;
// Never destroyed: a joinable std::thread in a static vector would terminate a process that exits without the renderer shutting down first.
std::vector<std::thread>& PrecacheWorkers() {
  static auto* workers = new std::vector<std::thread>();
  return *workers;
}

// One compile a call, so the two ways of working through the list stay the same.
bool RunNextJob() {
  const size_t index = g_nextJob.fetch_add(1, std::memory_order_relaxed);
  if (index >= g_precacheJobs.size()) return false;
  const PrecacheJob& job = g_precacheJobs[index];
  GraphicsSystem::DrawPathContext ctx = g_precacheContext;
  EnsurePipeline(ctx, job.call, job.vs, job.ps, job.samples, plume::RenderFormat(ctx.color_format));
  g_jobsDone.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void PrecacheWorker() {
#ifdef _WIN32
  // Every worker compiling at normal priority would preempt the render thread and stretch its frames.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  while (!g_precacheStop.load(std::memory_order_relaxed) && RunNextJob()) {
  }
}

uint32_t PrecacheWorkerCount() {
  const uint32_t hardware = std::thread::hardware_concurrency();
  uint32_t count = hardware > 3 ? hardware * 2 / 3 : 2;
  // Three cores stay free for the game's own threads.
  if (hardware > 5) count = std::min(count, hardware - 3);
  return std::max(count, 2u);
}

}  // namespace

void StopPipelinePrecache() {
  g_precacheStop.store(true);
  for (std::thread& worker : PrecacheWorkers()) worker.join();
  PrecacheWorkers().clear();
  g_precacheRunning.store(false);
}

void ResetPipelinePrecache() {
  StopPipelinePrecache();
  g_pipelinePrecacheQueued = false;
  g_pipelinePrecacheComplete = false;
  g_precacheStarted = false;
  g_precacheStop.store(false);
  g_precacheJobs.clear();
  g_nextJob.store(0);
  g_jobsDone.store(0);
  g_jobsTotal.store(0);
}

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void QueuePipelinePrecacheShaders(GraphicsSystem& gfx) {
  if (g_pipelinePrecacheQueued || !REXCVAR_GET(kameo_gfx_pipeline_precache)) {
    return;
  }
  g_pipelinePrecacheQueued = true;
  g_precacheRunning.store(true);
  std::set<uint64_t> hashes;
  for (const PipelineRecipe& recipe : kPipelineRecipes) {
    hashes.insert(recipe.vs_hash);
    if (recipe.ps_hash) hashes.insert(recipe.ps_hash);
  }
  for (const ShaderCacheEntry& entry : std::span(g_shaderCacheEntries, g_shaderCacheEntryCount)) {
    if (hashes.contains(entry.hash)) gfx.QueueShaderModule(&entry);
  }
}

// Shader modules and the context are only safe to touch on the render thread, so each recipe is resolved here and the workers are handed finished jobs.
static void BuildPrecacheJobs(GraphicsSystem& gfx) {
  std::unordered_map<uint64_t, const ShaderCacheEntry*> entries;
  for (const ShaderCacheEntry& entry : std::span(g_shaderCacheEntries, g_shaderCacheEntryCount)) {
    entries.emplace(entry.hash, &entry);
  }
  const uint32_t scene_samples = gfx.SceneSamples();
  g_precacheJobs.reserve(std::size(kPipelineRecipes) * 2);
  for (const PipelineRecipe& recipe : kPipelineRecipes) {
    PrecacheJob job;
    DrawCall& call = job.call;
    const auto vs_entry = entries.find(recipe.vs_hash);
    const auto ps_entry = entries.find(recipe.ps_hash);
    call.vs = vs_entry == entries.end() ? nullptr : vs_entry->second;
    call.ps = ps_entry == entries.end() ? nullptr : ps_entry->second;
    job.vs = gfx.ShaderModule(call.vs);
    job.ps = recipe.depth_only ? nullptr : gfx.ShaderModule(call.ps);
    if (!job.vs || (!recipe.depth_only && !job.ps)) {
      ++g_precacheMissing;
      continue;
    }
    call.layout = &kPrecacheLayouts[recipe.layout_index];
    call.primitive = recipe.primitive;
    call.point_sprite = recipe.primitive == kPrimPointList;
    call.depth_control = recipe.depth_control;
    call.blend_control = recipe.blend_control;
    call.color_control = recipe.color_control;
    call.mode_control = recipe.mode_control;
    call.color_mask = recipe.color_mask;
    call.alpha_to_mask = recipe.alpha_to_mask;
    call.depth_only = recipe.depth_only;
    call.poly_offset = recipe.poly_offset;
    call.poly_offset_scale = recipe.poly_offset_scale;
    // The same recipe can land in the multisampled scene or in a single-sample offscreen pass, so both are prepared.
    if (scene_samples > 1 && !recipe.depth_only) {
      PrecacheJob multisampled = job;
      multisampled.samples = scene_samples;
      g_precacheJobs.push_back(std::move(multisampled));
    }
    g_precacheJobs.push_back(std::move(job));
  }
}

void PrecacheDrawPipelines(GraphicsSystem& gfx) {
  if (g_pipelinePrecacheComplete || !g_pipelinePrecacheQueued) {
    return;
  }
  if (!g_precacheStarted) {
    if (!gfx.GetDrawPathContext(&g_precacheContext)) {
      return;
    }
    g_precacheStarted = true;
    g_precacheStart = std::chrono::steady_clock::now();
    g_precacheMissing = 0;
    g_failuresBefore = g_pipelineFailures.load();
    g_pipelinesBefore = PipelineCount();
    BuildPrecacheJobs(gfx);
    g_jobsTotal.store(uint32_t(g_precacheJobs.size()));
    const uint32_t wanted = PrecacheWorkerCount();
    try {
      for (uint32_t i = 0; i < wanted; ++i) PrecacheWorkers().emplace_back(PrecacheWorker);
    } catch (const std::system_error& error) {
      KGFX_WARN("pipeline precache could only start {} of {} threads: {}", PrecacheWorkers().size(), wanted, error.what());
    }
    KGFX_INFO("preparing {} pipelines from {} embedded recipes on {} background threads", g_precacheJobs.size(), std::size(kPipelineRecipes), PrecacheWorkers().size());
  }
  // With no worker running the render thread does the work itself, a slice a frame.
  if (PrecacheWorkers().empty()) {
    const auto slice_end = std::chrono::steady_clock::now() + kPrecacheSlice;
    while (std::chrono::steady_clock::now() < slice_end && RunNextJob()) {
    }
  }
  if (g_jobsDone.load(std::memory_order_relaxed) < g_precacheJobs.size()) {
    return;
  }
  for (std::thread& worker : PrecacheWorkers()) worker.join();
  PrecacheWorkers().clear();
  g_pipelinePrecacheComplete = true;
  g_precacheRunning.store(false);
  g_precacheJobs.clear();
  g_precacheJobs.shrink_to_fit();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_precacheStart).count();
  KGFX_INFO("pipeline precache finished: {} cached, {} missing shaders, {} failures in {:.1f} ms", PipelineCount() - g_pipelinesBefore, g_precacheMissing, g_pipelineFailures.load() - g_failuresBefore, ms);
}

bool PipelinePrecacheProgress(uint32_t* done, uint32_t* total) {
  *done = g_jobsDone.load(std::memory_order_relaxed);
  *total = g_jobsTotal.load(std::memory_order_relaxed);
  return g_precacheRunning.load(std::memory_order_relaxed);
}

void NoteGamePicture() { g_gamePictureShown.store(true, std::memory_order_relaxed); }

bool GamePictureShown() { return g_gamePictureShown.load(std::memory_order_relaxed); }

}  // namespace kameo::gpu
