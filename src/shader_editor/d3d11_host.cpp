#include "shader_editor/d3d11_host.h"

namespace kameo::shader_editor {

void D3D11EditorHost::Attach(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device_) return;
  device_ = device;
  context_ = context;
  preview_.Attach(device, context);
}

ImTextureID D3D11EditorHost::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba) {
  if (!device_ || !context_) return 0;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 0;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ID3D11Texture2D* handle = nullptr;
  device_->CreateTexture2D(&desc, nullptr, &handle);
  if (!handle) return 0;
  ID3D11ShaderResourceView* view = nullptr;
  context_->UpdateSubresource(handle, 0, nullptr, rgba, width * 4, 0);
  device_->CreateShaderResourceView(handle, nullptr, &view);
  if (view) context_->GenerateMips(view);
  handle->Release();
  return ImTextureID(view);
}

void D3D11EditorHost::FreeTexture(ImTextureID texture) {
  if (texture) reinterpret_cast<ID3D11ShaderResourceView*>(texture)->Release();
}

void D3D11EditorHost::DrawPreview(const std::string& shade, const std::array<ImTextureID, kGameTextureSlots>& textures) {
  preview_.SetShade(shade);
  std::array<ID3D11ShaderResourceView*, kGameTextureSlots> views{};
  for (size_t i = 0; i < views.size(); ++i) views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(textures[i]);
  preview_.SetGameTextures(views);
  preview_.Draw();
}

}  // namespace kameo::shader_editor
