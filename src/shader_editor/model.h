// A character for the preview, read from the player's own game files: geometry in its bind pose and its colour textures.
// The formats are described in tools/modkit/kameo_modkit, which this reads the same way.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kameo::shader_editor {

struct PreviewVertex {
  float position[3];
  float normal[3];
  float uv[2];
};

// One draw: a run of indices with its material.
struct PreviewPart {
  uint32_t first = 0;
  uint32_t count = 0;
  int texture = -1;  // into PreviewModel::textures; -1 draws `colour`
  float colour[3] = {1, 1, 1};
  float alpha_reference = 0;
};

struct PreviewTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

struct PreviewModel {
  std::vector<PreviewVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<PreviewPart> parts;
  std::vector<PreviewTexture> textures;
  float min[3] = {0, 0, 0};
  float max[3] = {0, 0, 0};
};

// `graph` is which of the file's rendergraphs to read, by position; a negative one takes the largest, which is the
// character in a model file that also holds what it throws or lays.
bool LoadPreviewModel(const std::filesystem::path& file, int graph, PreviewModel& model, std::string& error);

}  // namespace kameo::shader_editor
