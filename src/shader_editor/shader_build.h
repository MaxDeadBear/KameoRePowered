// Turning a shader mod into a .kshader: its Shade function compiled into each of the game's shaders it targets.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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
  const Target& all_characters() const { return all_characters_; }
  const Target& everything() const { return everything_; }
  const std::filesystem::path& folder() const { return folder_; }
  // The distinct shaders a list of target keys covers.
  std::vector<std::string> Shaders(const std::vector<std::string>& keys) const;
  // The meshes a mod is limited to: a target that stands for one model has them, and any other target beside it lifts them.
  std::vector<uint64_t> Meshes(const std::vector<std::string>& keys) const;

 private:
  std::filesystem::path folder_;
  std::vector<Target> characters_, levels_;
  std::map<std::string, std::vector<uint64_t>> meshes_;
  Target all_characters_, everything_;
};

struct BuildRequest {
  std::string shade;
  std::vector<std::string> shaders;
  std::vector<uint64_t> meshes;
  std::filesystem::path out;
  bool dxil = true;
  bool spirv = true;
};

// Compiles and writes the package, which a running game picks up by itself; false with the compiler's message in `log`.
bool BuildMod(const ShaderSource& source, const BuildRequest& request, std::string& log, std::atomic<size_t>* done = nullptr);

// Beside the editor: the shader compiler and the source pack, the mods folder, and which renderer the game is set to.
std::filesystem::path EditorFolder();
std::filesystem::path ModsFolder();
bool GameUsesVulkan();

}  // namespace kameo::shader_editor
