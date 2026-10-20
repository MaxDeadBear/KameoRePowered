#pragma once

// plume hands back a live texture or buffer even when the device refused to create its resource (the error goes to stderr), and recording a copy into one is a corrupt-resource fault that kills the process. These return null instead, with the request and the device's memory state logged.

#include <memory>

#include <plume_render_interface.h>

namespace kameo::gpu::detail {

std::unique_ptr<plume::RenderTexture> CreateTextureChecked(plume::RenderDevice* device, const plume::RenderTextureDesc& desc, const char* what);
std::unique_ptr<plume::RenderBuffer> CreateBufferChecked(plume::RenderDevice* device, const plume::RenderBufferDesc& desc, const char* what);

}  // namespace kameo::gpu::detail
