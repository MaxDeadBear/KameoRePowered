#include "shader_editor/preview.h"

#include <d3dcompiler.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include <imgui.h>

#include "shader_editor/shader_build.h"

namespace fs = std::filesystem;

namespace kameo::shader_editor {

namespace {

constexpr float kPi = 3.14159265f;
constexpr UINT kSamples = 4;
constexpr float kNear = 0.05f, kFar = 500.0f, kFieldOfView = 0.6f;
constexpr float kBackground[4] = {0.07f, 0.06f, 0.13f, 1.0f};

// Where everyone's standard look is: a rendergraph of this level archive, the one with the same skeleton as their skins.
constexpr char kSharedArchive[] = "011C2CC3.lvl";

// Kameo's looks, in the order of the title update's skin table (src/engine/dlc/dlc_skin_names.cpp), after the one she starts in.
struct Skin {
  const char* label;
  const char* suffix;
};
constexpr Skin kKameoSkins[] = {
    {"Classic", "00"},          {"Gothic", "01"},          {"Coyote", "02"},          {"Armored", "35"},         {"Fright Pack", "46"},
    {"Masquerade #1", "57"},    {"Masquerade #2", "58"},   {"Masquerade #3", "59"},   {"Masquerade #4", "60"},   {"Masquerade #5", "61"},
    {"Masquerade #6", "62"},    {"Masquerade #7", "63"},   {"Masquerade #8", "64"},   {"Masquerade #9", "65"},   {"Masquerade #10", "66"},
    {"Winter Warrior", "xmas1"}, {"Design-A-Skin", "67"},
};
constexpr int kKameoStandardGraph = 1;

// The warriors in the game's roster order; a warrior's DLC skins come in the same packs as Kameo's, and are named after them.
struct Warrior {
  const char* label;
  const char* model;
  int standard_graph;
  const char* alternate;
  const char* armored;
  const char* fright;
  const char* design;
};
constexpr Warrior kWarriors[] = {
    {"Pummel Weed", "0CE65812", 8, "03", "36", "47", "73"}, {"Snare", "0A70BAB2", 36, "10", "44", "55", "74"},     {"Rubble", "079B8925", 27, "07", "37", "48", "69"},
    {"Major Ruin", "037AABBF", 10, "09", "38", "49", "68"}, {"Deep Blue", "005C444C", 34, "04", "45", "56", "76"}, {"Flex", "08EBDF9E", 43, "08", "39", "50", "75"},
    {"40 Below", "056D64DE", 48, "12", "41", "52", "77"},   {"Chilla", "05800519", 12, "11", "43", "54", "72"},    {"Thermite", "008B5284", 39, "06", "42", "53", "70"},
    {"Ash", "0BBEAEDE", 29, "05", "40", "51", "71"},
};
constexpr char kKameoModel[] = "007183BF";
constexpr char kDlcFolder[] = "kameorepowered/0000000000000000/4D5307D2/00000002";

template <typename T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

struct alignas(16) Scene {
  float right[4];
  float up[4];
  float forward[4];
  float eye[4];
  float projection[4];  // x scale, y scale, then depth's scale and offset
  float sun[4];         // towards the light, camera space
  float sky[4];         // towards the sky, camera space
  float colour[4];      // the part's own colour, a its alpha test
  float frame[4];       // the time, then whether the part has a texture
};

const char kVertexShader[] = R"(
cbuffer Scene : register(b0) {
    float4 right_axis, up_axis, forward_axis, eye, projection, sun, sky, part_colour, frame;
};
struct In { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct Out { float4 clip : SV_Position; float3 view_position : TEXCOORD1; float3 normal : TEXCOORD2; float2 uv : TEXCOORD0; };
Out main(In v) {
    Out o;
    const float3 d = v.position - eye.xyz;
    const float depth = dot(d, forward_axis.xyz);
    o.view_position = float3(dot(d, right_axis.xyz), dot(d, up_axis.xyz), -depth);
    o.normal = float3(dot(v.normal, right_axis.xyz), dot(v.normal, up_axis.xyz), -dot(v.normal, forward_axis.xyz));
    o.uv = v.uv;
    o.clip = float4(o.view_position.x * projection.x, o.view_position.y * projection.y, depth * projection.z + projection.w, depth);
    return o;
}
)";

// DXC's HLSL 2021 the game's shaders are built with has these, and the compiler Windows ships doesn't.
const char kPrelude[] = R"(
#define select(c, a, b) ((c) ? (a) : (b))
#define rsqrt(x) (1.0 / sqrt(x))
)";

const char kPixelEntry[] = R"(
cbuffer Scene : register(b0) {
    float4 right_axis, up_axis, forward_axis, eye, projection, sun, sky, part_colour, frame;
};
Texture2D base_texture : register(t0);
SamplerState base_sampler : register(s0);
float4 main(float4 position : SV_Position, float3 view_position : TEXCOORD1, float3 normal : TEXCOORD2, float2 uv : TEXCOORD0) : SV_Target {
    const float4 texel = frame.y > 0.5 ? base_texture.Sample(base_sampler, uv) : float4(part_colour.rgb, 1.0);
    clip(texel.a - max(part_colour.a, 0.02));
    ModState m = ModStart(position.xy, frame.x);
    m.lit = true;
    m.s.view = normalize(-view_position);
    m.s.normal = ModNormalize(normal, m.s.view);
    if (dot(m.s.normal, m.s.view) < 0.0) m.s.normal = -m.s.normal;
    m.s.up = sky.xyz;
    m.s.light_dir = sun.xyz;
    m.s.uv = uv;
    m.s.albedo = texel.rgb;
    m.s.has_albedo = true;
    m.s.has_normal = true;
    const float sky_facing = dot(m.s.normal, m.s.up) * 0.5 + 0.5;
    m.s.light = lerp(float3(0.30, 0.27, 0.33), float3(0.55, 0.58, 0.62), sky_facing) + m.s.light_colour * 0.75 * saturate(dot(m.s.normal, m.s.light_dir));
    return float4(ModFinish(m, texel.rgb * m.s.light, 1.0), 1.0);
}
)";

ID3DBlob* Compile(const std::string& source, const char* profile, std::string& error) {
  ID3DBlob* code = nullptr;
  ID3DBlob* messages = nullptr;
  const HRESULT result = D3DCompile(source.data(), source.size(), "shader", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &messages);
  if (FAILED(result)) error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "The preview's shader compiler failed.";
  if (messages) messages->Release();
  return code;
}

void Normalize(float v[3]) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int c = 0; c < 3; ++c) v[c] /= length > 1e-6f ? length : 1.0f;
}

void Cross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

Preview::~Preview() {
  if (loading_.valid()) loading_.wait();
  if (compiling_.valid()) compiling_.wait();
  ReleaseModel();
  ReleaseTargets();
  Release(vertex_shader_);
  Release(pixel_shader_);
  Release(layout_);
  Release(constants_);
  Release(sampler_);
  Release(rasterizer_);
  Release(depth_state_);
  Release(white_);
}

void Preview::Attach(ID3D11Device* device, ID3D11DeviceContext* context) {
  device_ = device;
  context_ = context;
  CreateResources();
  FindCostumes();
  StartModelLoad();
}

// Everything installed: the game's own files, and DLC in the folder the game reads it from.
void Preview::FindCostumes() {
  const fs::path assets = EditorFolder() / "assets";
  std::map<std::string, fs::path> files;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(assets, error))
    if (entry.path().extension() == ".mdl") files[entry.path().filename().string()] = entry.path();
  wchar_t* documents = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(documents) / kDlcFolder, fs::directory_options::skip_permission_denied, error))
      if (entry.path().extension() == ".mdl") files.emplace(entry.path().filename().string(), entry.path());
  }
  CoTaskMemFree(documents);

  const auto add = [&](Character& character, const char* label, const char* model, const char* suffix) {
    const auto found = files.find(std::format("{}_{}.mdl", model, suffix));
    if (found != files.end()) character.costumes.push_back({label, found->second, -1});
  };
  const bool shared = fs::exists(assets / kSharedArchive, error);
  Character kameo{"Kameo", {}};
  if (shared) kameo.costumes.push_back({"Everyday", assets / kSharedArchive, kKameoStandardGraph});
  for (const Skin& skin : kKameoSkins) add(kameo, skin.label, kKameoModel, skin.suffix);
  characters_.push_back(std::move(kameo));
  for (const Warrior& warrior : kWarriors) {
    Character character{warrior.label, {}};
    if (shared) character.costumes.push_back({"Standard", assets / kSharedArchive, warrior.standard_graph});
    add(character, "Alternate", warrior.model, warrior.alternate);
    add(character, "Armored", warrior.model, warrior.armored);
    add(character, "Fright Pack", warrior.model, warrior.fright);
    add(character, "Winter Warrior", warrior.model, "xmas1");
    add(character, "Design-A-Skin", warrior.model, warrior.design);
    characters_.push_back(std::move(character));
  }
}

void Preview::CreateResources() {
  std::string error;
  ID3DBlob* code = Compile(kVertexShader, "vs_5_0", error);
  if (!code) {
    compile_error_ = error;
    return;
  }
  device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex_shader_);
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PreviewVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PreviewVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(PreviewVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  device_->CreateInputLayout(elements, UINT(std::size(elements)), code->GetBufferPointer(), code->GetBufferSize(), &layout_);
  code->Release();

  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = sizeof(Scene);
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device_->CreateBuffer(&constants, nullptr, &constants_);

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_ANISOTROPIC;
  sampler.MaxAnisotropy = 8;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&sampler, &sampler_);

  D3D11_RASTERIZER_DESC rasterizer{};
  rasterizer.FillMode = D3D11_FILL_SOLID;
  rasterizer.CullMode = D3D11_CULL_NONE;
  rasterizer.DepthClipEnable = TRUE;
  rasterizer.MultisampleEnable = TRUE;
  device_->CreateRasterizerState(&rasterizer, &rasterizer_);

  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = TRUE;
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth.DepthFunc = D3D11_COMPARISON_LESS;
  device_->CreateDepthStencilState(&depth, &depth_state_);

  const uint32_t white = 0xFFFFFFFF;
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = texture.Height = texture.MipLevels = texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA data{&white, 4, 0};
  ID3D11Texture2D* one = nullptr;
  device_->CreateTexture2D(&texture, &data, &one);
  if (one) {
    device_->CreateShaderResourceView(one, nullptr, &white_);
    one->Release();
  }
}

void Preview::ReleaseTargets() {
  Release(shown_);
  Release(target_);
  Release(depth_view_);
  Release(depth_);
  Release(resolved_);
  Release(colour_);
  width_ = height_ = 0;
}

void Preview::ResizeTargets(uint32_t width, uint32_t height) {
  if (width == width_ && height == height_) return;
  ReleaseTargets();
  width_ = width, height_ = height;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = kSamples;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  device_->CreateTexture2D(&desc, nullptr, &colour_);
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  device_->CreateTexture2D(&desc, nullptr, &resolved_);
  desc.SampleDesc.Count = kSamples;
  desc.Format = DXGI_FORMAT_D32_FLOAT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  device_->CreateTexture2D(&desc, nullptr, &depth_);
  if (colour_) device_->CreateRenderTargetView(colour_, nullptr, &target_);
  if (depth_) device_->CreateDepthStencilView(depth_, nullptr, &depth_view_);
  if (resolved_) device_->CreateShaderResourceView(resolved_, nullptr, &shown_);
}

void Preview::ReleaseModel() {
  Release(vertices_);
  Release(indices_);
  for (ID3D11ShaderResourceView*& texture : textures_) Release(texture);
  textures_.clear();
  parts_.clear();
}

void Preview::StartModelLoad() {
  if (loading_.valid()) return;
  if (characters_.empty() || characters_[character_].costumes.empty()) {
    model_error_ = "Install the game to preview its characters.";
    return;
  }
  loading_ = std::async(std::launch::async, [costume = characters_[character_].costumes[costume_]] {
    PreviewModel model;
    std::string error;
    LoadPreviewModel(costume.file, costume.graph, model, error);
    return std::pair(std::move(model), std::move(error));
  });
}

void Preview::TakeModel() {
  if (!loading_.valid() || loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  auto [model, error] = loading_.get();
  model_error_ = std::move(error);
  ReleaseModel();
  if (!model_error_.empty()) return;

  D3D11_BUFFER_DESC desc{};
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  desc.ByteWidth = UINT(model.vertices.size() * sizeof(PreviewVertex));
  D3D11_SUBRESOURCE_DATA data{model.vertices.data(), 0, 0};
  device_->CreateBuffer(&desc, &data, &vertices_);
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  desc.ByteWidth = UINT(model.indices.size() * sizeof(uint32_t));
  data.pSysMem = model.indices.data();
  device_->CreateBuffer(&desc, &data, &indices_);

  for (const PreviewTexture& texture : model.textures) {
    D3D11_TEXTURE2D_DESC t{};
    t.Width = texture.width;
    t.Height = texture.height;
    t.MipLevels = 0;
    t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* handle = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    device_->CreateTexture2D(&t, nullptr, &handle);
    if (handle) {
      context_->UpdateSubresource(handle, 0, nullptr, texture.rgba.data(), texture.width * 4, 0);
      device_->CreateShaderResourceView(handle, nullptr, &view);
      if (view) context_->GenerateMips(view);
      handle->Release();
    }
    textures_.push_back(view);
  }
  parts_ = std::move(model.parts);
  float extent = 0;
  for (int c = 0; c < 3; ++c) {
    centre_[c] = (model.min[c] + model.max[c]) * 0.5f;
    extent += (model.max[c] - model.min[c]) * (model.max[c] - model.min[c]);
  }
  // Half the diagonal, so a character in its T-pose fits from any angle.
  radius_ = std::max(std::sqrt(extent) * 0.5f, 0.01f);
  Frame();
}

void Preview::Frame() {
  yaw_ = 0.0f;
  pitch_ = 0.12f;
  pan_[0] = pan_[1] = 0;
  distance_ = radius_ / std::tan(kFieldOfView * 0.5f) * 1.1f;
}

void Preview::SetTemplate(std::string text) { template_ = std::move(text); }

void Preview::SetShade(const std::string& shade) { shade_ = shade; }

void Preview::StartCompile() {
  if (compiling_.valid() || template_.empty() || shade_ == compiled_shade_) return;
  compiled_shade_ = shade_;
  compiling_ = std::async(std::launch::async, [source = std::string(kPrelude) + template_ + "\n" + shade_ + "\n" + kPixelEntry] {
    CompiledShader out;
    if (ID3DBlob* code = Compile(source, "ps_5_0", out.error)) {
      const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
      out.bytes.assign(bytes, bytes + code->GetBufferSize());
      code->Release();
    }
    return out;
  });
}

void Preview::TakeCompile() {
  if (!compiling_.valid() || compiling_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  CompiledShader result = compiling_.get();
  compile_error_ = std::move(result.error);
  if (result.bytes.empty()) return;
  ID3D11PixelShader* shader = nullptr;
  if (SUCCEEDED(device_->CreatePixelShader(result.bytes.data(), result.bytes.size(), nullptr, &shader))) {
    Release(pixel_shader_);
    pixel_shader_ = shader;
  }
}

void Preview::Render(uint32_t width, uint32_t height) {
  ResizeTargets(width, height);
  if (!target_ || !depth_view_) return;
  context_->ClearRenderTargetView(target_, kBackground);
  context_->ClearDepthStencilView(depth_view_, D3D11_CLEAR_DEPTH, 1.0f, 0);
  if (pixel_shader_ && vertices_ && indices_ && vertex_shader_ && constants_) {
    const float cos_pitch = std::cos(pitch_);
    float forward[3] = {-std::sin(yaw_) * cos_pitch, -std::sin(pitch_), -std::cos(yaw_) * cos_pitch};
    const float world_up[3] = {0, 1, 0};
    float right[3], up[3];
    Cross(world_up, forward, right);
    Normalize(right);
    Cross(forward, right, up);
    float eye[3];
    for (int c = 0; c < 3; ++c) eye[c] = centre_[c] - forward[c] * distance_ + right[c] * pan_[0] + up[c] * pan_[1];

    Scene scene{};
    std::memcpy(scene.right, right, sizeof(right));
    std::memcpy(scene.up, up, sizeof(up));
    std::memcpy(scene.forward, forward, sizeof(forward));
    std::memcpy(scene.eye, eye, sizeof(eye));
    const float scale = 1.0f / std::tan(kFieldOfView * 0.5f);
    scene.projection[0] = scale * float(height) / float(width);
    scene.projection[1] = scale;
    scene.projection[2] = kFar / (kFar - kNear);
    scene.projection[3] = -kNear * kFar / (kFar - kNear);
    // The sun stays over the model's right shoulder as the view turns about it, as a viewer expects of a turntable.
    float sun_world[3] = {0.45f, 0.75f, 0.5f};
    Normalize(sun_world);
    scene.sun[0] = Dot(sun_world, right), scene.sun[1] = Dot(sun_world, up), scene.sun[2] = -Dot(sun_world, forward);
    scene.sky[0] = Dot(world_up, right), scene.sky[1] = Dot(world_up, up), scene.sky[2] = -Dot(world_up, forward);
    scene.frame[0] = std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count(), 3600.0f);

    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_);
    context_->OMSetRenderTargets(1, &target_, depth_view_);
    context_->OMSetDepthStencilState(depth_state_, 0);
    const float blend[4] = {};
    context_->OMSetBlendState(nullptr, blend, 0xFFFFFFFF);
    context_->IASetInputLayout(layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(PreviewVertex), offset = 0;
    context_->IASetVertexBuffers(0, 1, &vertices_, &stride, &offset);
    context_->IASetIndexBuffer(indices_, DXGI_FORMAT_R32_UINT, 0);
    context_->VSSetShader(vertex_shader_, nullptr, 0);
    context_->PSSetShader(pixel_shader_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetSamplers(0, 1, &sampler_);
    for (const PreviewPart& part : parts_) {
      ID3D11ShaderResourceView* texture = part.texture >= 0 && size_t(part.texture) < textures_.size() ? textures_[part.texture] : nullptr;
      std::memcpy(scene.colour, part.colour, sizeof(part.colour));
      scene.colour[3] = part.alpha_reference;
      scene.frame[1] = texture ? 1.0f : 0.0f;
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
      std::memcpy(mapped.pData, &scene, sizeof(scene));
      context_->Unmap(constants_, 0);
      ID3D11ShaderResourceView* bound = texture ? texture : white_;
      context_->PSSetShaderResources(0, 1, &bound);
      context_->DrawIndexed(part.count, part.first, 0);
    }
    ID3D11RenderTargetView* none = nullptr;
    context_->OMSetRenderTargets(1, &none, nullptr);
  }
  context_->ResolveSubresource(resolved_, 0, colour_, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void Preview::Draw() {
  if (!device_) return;
  TakeModel();
  TakeCompile();
  StartCompile();

  if (!characters_.empty()) {
    const float combo = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2 - 76.0f) * 0.5f;
    ImGui::SetNextItemWidth(combo);
    if (ImGui::BeginCombo("##character", characters_[character_].label.c_str())) {
      for (size_t i = 0; i < characters_.size(); ++i) {
        if (ImGui::Selectable(characters_[i].label.c_str(), i == character_) && i != character_ && !loading_.valid()) {
          character_ = i;
          costume_ = 0;
          StartModelLoad();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo);
    const std::vector<Costume>& costumes = characters_[character_].costumes;
    if (ImGui::BeginCombo("##costume", costumes.empty() ? "" : costumes[costume_].label.c_str())) {
      for (size_t i = 0; i < costumes.size(); ++i) {
        if (ImGui::Selectable(costumes[i].label.c_str(), i == costume_) && i != costume_ && !loading_.valid()) {
          costume_ = i;
          StartModelLoad();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }
  ImGui::Checkbox("Spin", &spin_);

  const ImVec2 size = ImGui::GetContentRegionAvail();
  const uint32_t width = uint32_t(std::max(size.x, 16.0f)), height = uint32_t(std::max(size.y, 16.0f));
  if (spin_) yaw_ += ImGui::GetIO().DeltaTime * 0.6f;
  Render(width, height);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##view", ImVec2(float(width), float(height)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      yaw_ -= io.MouseDelta.x * 0.01f;
      pitch_ = std::clamp(pitch_ + io.MouseDelta.y * 0.01f, -kPi * 0.48f, kPi * 0.48f);
    } else {
      const float unit = distance_ * std::tan(kFieldOfView * 0.5f) * 2.0f / float(height);
      pan_[0] -= io.MouseDelta.x * unit;
      pan_[1] += io.MouseDelta.y * unit;
    }
  }
  if (hovered && io.MouseWheel != 0.0f) distance_ = std::clamp(distance_ * std::pow(0.9f, io.MouseWheel), radius_ * 0.3f, radius_ * 12.0f);
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Frame();
  ImDrawList* list = ImGui::GetWindowDrawList();
  if (shown_) list->AddImage(ImTextureRef(ImTextureID(shown_)), at, ImVec2(at.x + float(width), at.y + float(height)));
  const ImU32 note = ImGui::GetColorU32(ImGuiCol_Text);
  if (loading_.valid()) list->AddText(ImVec2(at.x + 8, at.y + 6), note, "Loading...");
  else if (!model_error_.empty()) list->AddText(nullptr, 0.0f, ImVec2(at.x + 8, at.y + 6), note, model_error_.c_str(), nullptr, float(width) - 16.0f);
  if (!compile_error_.empty()) list->AddText(nullptr, 0.0f, ImVec2(at.x + 8, at.y + 26), IM_COL32(255, 120, 110, 255), compile_error_.c_str(), nullptr, float(width) - 16.0f);
  if (hovered) ImGui::SetItemTooltip("Drag to turn, right drag to move, wheel to zoom, double-click to frame.\nThe lighting stands in for a level's.");
}

}  // namespace kameo::shader_editor
