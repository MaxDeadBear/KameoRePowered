// The editor's preview: a character from the player's game, drawn with Direct3D 11 and the mod's own generated HLSL, so
// Custom nodes and raw HLSL show as they will in game. Its lighting is a stand-in for a level's.
#pragma once

#include <d3d11.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "shader_editor/model.h"

namespace kameo::shader_editor {

class Preview {
 public:
  ~Preview();
  void Attach(ID3D11Device* device, ID3D11DeviceContext* context);
  // The template the mod's code goes with, shader_mod.hlsli, once the shader source is ready.
  void SetTemplate(std::string text);
  // The mod's Shade function; rebuilt when it changes.
  void SetShade(const std::string& shade);
  void Draw();

 private:
  // One way a character can look, and where the game keeps it.
  struct Costume {
    std::string label;
    std::filesystem::path file;
    int graph = -1;
  };
  struct Character {
    std::string label;
    std::vector<Costume> costumes;
  };
  struct CompiledShader {
    std::vector<uint8_t> bytes;
    std::string error;
    std::string source;
  };

  void CreateResources();
  void FindCostumes();
  void ResizeTargets(uint32_t width, uint32_t height);
  void ReleaseTargets();
  void ReleaseModel();
  void StartModelLoad();
  void TakeModel();
  void StartCompile();
  void TakeCompile();
  void Render(uint32_t width, uint32_t height);
  void Frame();

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  ID3D11VertexShader* vertex_shader_ = nullptr;
  ID3D11PixelShader* pixel_shader_ = nullptr;
  ID3D11InputLayout* layout_ = nullptr;
  ID3D11Buffer* constants_ = nullptr;
  ID3D11SamplerState* sampler_ = nullptr;
  ID3D11RasterizerState* rasterizer_ = nullptr;
  ID3D11DepthStencilState* depth_state_ = nullptr;
  ID3D11ShaderResourceView* white_ = nullptr;

  ID3D11Texture2D* colour_ = nullptr;
  ID3D11Texture2D* resolved_ = nullptr;
  ID3D11Texture2D* depth_ = nullptr;
  ID3D11RenderTargetView* target_ = nullptr;
  ID3D11DepthStencilView* depth_view_ = nullptr;
  ID3D11ShaderResourceView* shown_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  ID3D11Buffer* vertices_ = nullptr;
  ID3D11Buffer* indices_ = nullptr;
  std::vector<ID3D11ShaderResourceView*> textures_;
  std::vector<PreviewPart> parts_;
  float centre_[3] = {0, 0, 0};
  float radius_ = 1;

  std::vector<Character> characters_;
  size_t character_ = 0;
  size_t costume_ = 0;
  std::future<std::pair<PreviewModel, std::string>> loading_;
  std::string model_error_;

  std::string template_;
  std::string shade_;
  std::string compiled_shade_;
  std::string compile_error_;
  std::future<CompiledShader> compiling_;

  float yaw_ = 0.0f;
  float pitch_ = 0.12f;
  float distance_ = 3.0f;
  float pan_[2] = {0, 0};
  bool spin_ = false;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace kameo::shader_editor
