// Turning a shader mod into a .kshader: its Shade function compiled into each of the game's shaders it targets.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "gpu/shaders/shader_mods.h"
#include "shader_editor/shader_graph.h"

namespace kameo::shader_editor {

// Something a mod can apply to. It stands for shaders, not objects: whatever else draws with them changes too.
struct Target {
  std::string key;
  std::string label;
  std::vector<std::string> shaders;
};

// The game's translated shaders, from editor_shaders/source.zip beside the editor, unpacked once.
class ShaderSource {
 public:
  bool Prepare(std::string& error);
  const std::vector<Target>& characters() const { return characters_; }
  const std::vector<Target>& levels() const { return levels_; }
  const std::vector<Target>& surfaces() const { return surfaces_; }
  const Target& all_characters() const { return all_characters_; }
  const Target& everything() const { return everything_; }
  const std::filesystem::path& folder() const { return folder_; }
  // The distinct shaders a list of target keys covers.
  std::vector<std::string> Shaders(const std::vector<std::string>& keys) const;
  // The meshes a mod is limited to: a target that stands for one model has them, and any other target beside it lifts them.
  std::vector<uint64_t> Meshes(const std::vector<std::string>& keys) const;
  // The character a mod is a costume for, by model type, when its one target is a character's standard look; else 0.
  uint32_t CostumeModel(const std::vector<std::string>& keys) const;
  // Whether a target is a surface the game draws unlit, which a mod only reaches when built to include those.
  bool NeedsUnlit(const std::vector<std::string>& keys) const;

 private:
  std::filesystem::path folder_;
  std::vector<Target> characters_, levels_, surfaces_;
  std::map<std::string, std::vector<uint64_t>> meshes_;
  std::map<std::string, uint32_t> costume_models_;
  Target all_characters_, everything_;
};

struct BuildRequest {
  std::string shade;
  std::vector<std::string> shaders;
  std::vector<uint64_t> meshes;
  // With meshes, the character the costume is for.
  uint32_t model_type = 0;
  // Reach surfaces the game doesn't light too (MOD_INCLUDE_UNLIT in shader_mod.hlsli).
  bool unlit = false;
  // The game's textures it draws with, by slot (GameTextures in shader_graph.h); named, never copied in.
  std::vector<gpu::ShaderModTexture> textures;
  // Its Opacity can be below 1 (SeeThrough in shader_graph.h), so the game shows it by coverage where it can.
  bool see_through = false;
  std::filesystem::path out;
  bool dxil = true;
  bool spirv = true;
};

// A graph's game textures as the package names them.
std::vector<gpu::ShaderModTexture> PackageTextures(const std::vector<GameTextureRef>& textures);

// Compiles and writes the package, which a running game picks up by itself; false with the compiler's message in `log`.
bool BuildMod(const ShaderSource& source, const BuildRequest& request, std::string& log, std::atomic<size_t>* done = nullptr);

// Beside the editor: the shader compiler and the source pack, the mods folder, and which renderer the game is set to.
std::filesystem::path EditorFolder();
std::filesystem::path ModsFolder();
bool GameUsesVulkan();

}  // namespace kameo::shader_editor
