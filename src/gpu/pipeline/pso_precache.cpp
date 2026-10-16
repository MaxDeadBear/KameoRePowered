#include "gpu/pipeline/pso_precache.h"

#include <chrono>
#include <iterator>
#include <set>
#include <span>
#include <unordered_map>

#include <plume_render_interface.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache_data.h"
#include "gpu/shaders/shader_cache.h"

namespace kameo::gpu::detail {
namespace {

bool g_pipelinePrecacheQueued = false;
bool g_pipelinePrecacheComplete = false;

}  // namespace

void ResetPipelinePrecache() {
  g_pipelinePrecacheQueued = false;
  g_pipelinePrecacheComplete = false;
}

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void QueuePipelinePrecacheShaders(GraphicsSystem& gfx) {
  if (g_pipelinePrecacheQueued || !REXCVAR_GET(kameo_gfx_pipeline_precache)) {
    return;
  }
  g_pipelinePrecacheQueued = true;
  std::set<uint64_t> hashes;
  for (const PipelineRecipe& recipe : kPipelineRecipes) {
    hashes.insert(recipe.vs_hash);
    if (recipe.ps_hash) hashes.insert(recipe.ps_hash);
  }
  for (const ShaderCacheEntry& entry : std::span(g_shaderCacheEntries, g_shaderCacheEntryCount)) {
    if (hashes.contains(entry.hash)) gfx.QueueShaderModule(&entry);
  }
}

void PrecacheDrawPipelines(GraphicsSystem& gfx) {
  GraphicsSystem::DrawPathContext ctx;
  if (g_pipelinePrecacheComplete || !g_pipelinePrecacheQueued || !gfx.GetDrawPathContext(&ctx)) {
    return;
  }
  g_pipelinePrecacheComplete = true;
  std::unordered_map<uint64_t, const ShaderCacheEntry*> entries;
  for (const ShaderCacheEntry& entry : std::span(g_shaderCacheEntries, g_shaderCacheEntryCount)) {
    entries.emplace(entry.hash, &entry);
  }
  const auto start = std::chrono::steady_clock::now();
  const uint32_t scene_samples = gfx.SceneSamples();
  uint32_t missing = 0;
  const uint32_t failures_before = g_pipelineFailures;
  const size_t pipelines_before = PipelineCount();
  KGFX_INFO("preparing {} embedded pipeline recipes before gameplay", std::size(kPipelineRecipes));
  for (const PipelineRecipe& recipe : kPipelineRecipes) {
    DrawCall call;
    const auto vs_entry = entries.find(recipe.vs_hash);
    const auto ps_entry = entries.find(recipe.ps_hash);
    call.vs = vs_entry == entries.end() ? nullptr : vs_entry->second;
    call.ps = ps_entry == entries.end() ? nullptr : ps_entry->second;
    auto* vs = gfx.ShaderModule(call.vs);
    auto* ps = recipe.depth_only ? nullptr : gfx.ShaderModule(call.ps);
    if (!vs || (!recipe.depth_only && !ps)) {
      ++missing;
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
    EnsurePipeline(ctx, call, vs, ps, 1, plume::RenderFormat(ctx.color_format));
    // The same recipe can land in the multisampled scene or in a single-sample offscreen pass, so both are prepared.
    if (scene_samples > 1 && !recipe.depth_only) EnsurePipeline(ctx, call, vs, ps, scene_samples, plume::RenderFormat(ctx.color_format));
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  KGFX_INFO("pipeline precache finished: {} cached, {} missing shaders, {} failures in {:.1f} ms", PipelineCount() - pipelines_before, missing, g_pipelineFailures - failures_before, ms);
}

}  // namespace kameo::gpu
