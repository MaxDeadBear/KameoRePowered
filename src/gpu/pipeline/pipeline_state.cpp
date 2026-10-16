#include "gpu/pipeline/pipeline_state.h"

#include "gpu/guest_state.h"

namespace kameo::gpu::detail {

plume::RenderPrimitiveTopology HostTopology(uint32_t primitive) {
  switch (primitive) {
    case kPrimPointList: return plume::RenderPrimitiveTopology::POINT_LIST;
    case kPrimLineList: return plume::RenderPrimitiveTopology::LINE_LIST;
    // Both strip types arrive expanded into lists by ExpandTopology.
    case kPrimLineStrip: return plume::RenderPrimitiveTopology::LINE_LIST;
    default: return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

plume::RenderComparisonFunction HostCompare(uint32_t func) {
  switch (func) {
    case kCompareNever: return plume::RenderComparisonFunction::NEVER;
    case kCompareLess: return plume::RenderComparisonFunction::LESS;
    case kCompareEqual: return plume::RenderComparisonFunction::EQUAL;
    case kCompareLessEqual: return plume::RenderComparisonFunction::LESS_EQUAL;
    case kCompareGreater: return plume::RenderComparisonFunction::GREATER;
    case kCompareNotEqual: return plume::RenderComparisonFunction::NOT_EQUAL;
    case kCompareGreaterEqual: return plume::RenderComparisonFunction::GREATER_EQUAL;
    default: return plume::RenderComparisonFunction::ALWAYS;
  }
}

plume::RenderStencilFaceDesc HostStencilFace(uint32_t depth_control, uint32_t shift) {
  plume::RenderStencilFaceDesc face;
  face.compareFunction = HostCompare((depth_control >> shift) & state::kStencilOpMask);
  face.failOp = plume::RenderStencilOp(((depth_control >> (shift + 3)) & state::kStencilOpMask) + 1);
  face.passOp = plume::RenderStencilOp(((depth_control >> (shift + 6)) & state::kStencilOpMask) + 1);
  face.depthFailOp = plume::RenderStencilOp(((depth_control >> (shift + 9)) & state::kStencilOpMask) + 1);
  return face;
}

plume::RenderBlend HostBlend(uint32_t factor) {
  switch (factor) {
    case kBlendZero: return plume::RenderBlend::ZERO;
    case kBlendOne: return plume::RenderBlend::ONE;
    case kBlendSrcColor: return plume::RenderBlend::SRC_COLOR;
    case kBlendInvSrcColor: return plume::RenderBlend::INV_SRC_COLOR;
    case kBlendSrcAlpha: return plume::RenderBlend::SRC_ALPHA;
    case kBlendInvSrcAlpha: return plume::RenderBlend::INV_SRC_ALPHA;
    case kBlendDestColor: return plume::RenderBlend::DEST_COLOR;
    case kBlendInvDestColor: return plume::RenderBlend::INV_DEST_COLOR;
    case kBlendDestAlpha: return plume::RenderBlend::DEST_ALPHA;
    case kBlendInvDestAlpha: return plume::RenderBlend::INV_DEST_ALPHA;
    case kBlendConstColor: return plume::RenderBlend::BLEND_FACTOR;
    case kBlendInvConstColor: return plume::RenderBlend::INV_BLEND_FACTOR;
    case kBlendConstAlpha: return plume::RenderBlend::BLEND_FACTOR;
    case kBlendInvConstAlpha: return plume::RenderBlend::INV_BLEND_FACTOR;
    case kBlendSrcAlphaSat: return plume::RenderBlend::SRC_ALPHA_SAT;
    default: return plume::RenderBlend::ONE;
  }
}

// The alpha blend slots take a narrower set of factors: Xenos uses the alpha of a colour factor there, which D3D12 rejects outright (silently, without the debug layer), so each folds onto its alpha counterpart.
plume::RenderBlend HostBlendAlpha(uint32_t factor) {
  switch (HostBlend(factor)) {
    case plume::RenderBlend::SRC_COLOR: return plume::RenderBlend::SRC_ALPHA;
    case plume::RenderBlend::INV_SRC_COLOR: return plume::RenderBlend::INV_SRC_ALPHA;
    case plume::RenderBlend::DEST_COLOR: return plume::RenderBlend::DEST_ALPHA;
    case plume::RenderBlend::INV_DEST_COLOR: return plume::RenderBlend::INV_DEST_ALPHA;
    default: return HostBlend(factor);
  }
}

plume::RenderBlendOperation HostBlendOp(uint32_t op) {
  switch (op) {
    case kBlendOpAdd: return plume::RenderBlendOperation::ADD;
    case kBlendOpSrcMinusDest: return plume::RenderBlendOperation::SUBTRACT;
    case kBlendOpMin: return plume::RenderBlendOperation::MIN;
    case kBlendOpMax: return plume::RenderBlendOperation::MAX;
    case kBlendOpDestMinusSrc: return plume::RenderBlendOperation::REV_SUBTRACT;
    default: return plume::RenderBlendOperation::ADD;
  }
}

}  // namespace kameo::gpu::detail
