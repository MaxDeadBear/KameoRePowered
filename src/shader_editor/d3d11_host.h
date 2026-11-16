// The standalone editor's host: thumbnails as Direct3D 11 views, and the model preview.
#pragma once

#include <d3d11.h>

#include "shader_editor/editor.h"
#include "shader_editor/preview.h"

namespace kameo::shader_editor {

class D3D11EditorHost final : public EditorHost {
 public:
  // Called every frame with the window's device; the preview is attached to the first.
  void Attach(ID3D11Device* device, ID3D11DeviceContext* context);

  ImTextureID CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba) override;
  void FreeTexture(ImTextureID texture) override;
  bool HasPreview() const override { return true; }
  void SetPreviewTemplate(std::string text) override { preview_.SetTemplate(std::move(text)); }
  void DrawPreview(const std::string& shade, const std::array<ImTextureID, kGameTextureSlots>& textures) override;

 private:
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  Preview preview_;
};

}  // namespace kameo::shader_editor
