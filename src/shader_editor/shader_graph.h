// A shader mod as a node graph (docs/modding.md): every value is a float3, a single number being the same in all three,
// and the graph ends in one Output node, the colour Shade returns.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kameo::shader_editor {

enum class NodeKind {
  kOutput,
  kGameColour,
  kAlbedo,
  kLight,
  kUv,
  kScreen,
  kKeyLight,
  kSunLight,
  kFresnel,
  kUpFacing,
  kFloat,
  kColor,
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kLerp,
  kOneMinus,
  kSaturate,
  kPower,
  kMin,
  kMax,
  kStep,
  kSmoothstep,
  kBands,
  kDesaturate,
  kLuminance,
  kHalftone,
  kCustom,
  kReroute,
  kTime,
  kPanner,
  kNoise,
  kSine,
  kCosine,
  kFrac,
  kFloor,
  kCeil,
  kAbs,
  kClamp,
  kDot,
  kCross,
  kNormalize,
  kLength,
  kDistance,
  kRemap,
  kIf,
  kComponentMask,
  kMakeColour,
  kNormal,
  kCameraVector,
  kUpVector,
  kKeyVector,
  kSunVector,
  kSunColour,
  kOutline,
  kCount,
};

using Float3 = std::array<float, 3>;

struct NodeInfo {
  const char* name;
  const char* category;
  std::vector<const char*> inputs;
  std::vector<Float3> defaults;
  // Settings edited on the node itself, not wired: a key light's direction, a constant's value.
  std::vector<const char*> settings;
  std::vector<float> setting_defaults;
  const char* help;
  // Held while clicking the canvas: '1' a number, 'M' a multiply.
  char hotkey = 0;
  // Its first input takes the surface's UV while nothing is wired to it.
  bool uv_input = false;
};

const NodeInfo& Info(NodeKind kind);
const std::vector<const char*>& Categories();
// The title bar colour of a category's nodes, as packed RGBA.
uint32_t CategoryColour(const char* category);

struct Node {
  int id = 0;
  NodeKind kind = NodeKind::kFloat;
  // The value an input takes while nothing is wired to it.
  std::vector<Float3> inputs;
  std::vector<float> settings;
  // A Custom node's HLSL: a function body over ShadeInputs s and float3 a, b, c, d that returns a float3.
  std::string code;
  // A constant's parameter name, listed on the Parameters panel; empty for none.
  std::string name;
  float x = 0.0f;
  float y = 0.0f;
};

struct Link {
  int id = 0;
  int from = 0;
  int to = 0;
  int input = 0;
};

// A comment box behind nodes; moving it moves the nodes inside.
struct Comment {
  int id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float w = 300.0f;
  float h = 200.0f;
  std::string text = "Comment";
  Float3 colour = {0.85f, 0.35f, 0.55f};
};

struct ShaderGraph {
  std::vector<Node> nodes;
  std::vector<Link> links;
  std::vector<Comment> comments;
  int next_id = 1;
  // Raw HLSL instead of the graph: the body of float3 Shade(ShadeInputs s).
  bool raw = false;
  std::string raw_code;

  Node& Add(NodeKind kind, float x, float y);
  void Remove(int node);
  const Node* Find(int id) const;
  Node* Find(int id);
  // The link into a node's input, if any.
  const Link* Into(int node, int input) const;
  // Wires `from`'s output to `to`'s input, replacing what was there; refused when it would make a loop.
  bool Connect(int from, int to, int input);
};

// The source of float3 Shade(ShadeInputs s) for shader_mod.hlsli, with any Custom nodes' functions before it.
std::string GenerateHlsl(const ShaderGraph& graph);

enum class Preset { kGameLook, kCelShade, kToonBands, kFlat, kRimLit, kComicHalftone, kCount };
const char* PresetName(Preset preset);
ShaderGraph MakePreset(Preset preset);

// What the editor saves beside a mod's .kshader, to open it again.
struct ShaderMod {
  ShaderGraph graph;
  std::vector<std::string> targets;
};
bool SaveMod(const ShaderMod& mod, const std::filesystem::path& path, std::string& error);
std::optional<ShaderMod> LoadMod(const std::filesystem::path& path, std::string& error);
// A graph, or some of its nodes, as text for the clipboard, and back.
std::string GraphToText(const ShaderGraph& graph);
std::optional<ShaderGraph> GraphFromText(const std::string& text);

}  // namespace kameo::shader_editor
