#include "shader_editor/shader_graph.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/localization.h"

namespace kameo::shader_editor {

using l10n::Tr;
using l10n::TrFormat;

namespace {

const Float3 kZero{0, 0, 0}, kOne{1, 1, 1};

// Stable names for the saved files, by NodeKind.
const char* const kKindNames[] = {"output", "game_colour", "albedo", "light", "uv", "screen", "key_light", "sun_light", "fresnel", "up_facing", "float", "color", "add", "subtract", "multiply", "divide", "lerp", "one_minus", "saturate", "power", "min", "max", "step", "smoothstep", "bands", "desaturate", "luminance", "halftone", "custom",
                                  "reroute", "time", "panner", "noise", "sine", "cosine", "frac", "floor", "ceil", "abs", "clamp", "dot", "cross", "normalize", "length", "distance", "remap", "if", "component_mask", "make_colour", "normal", "camera_vector", "up_vector", "key_vector", "sun_vector", "sun_colour", "outline"};
static_assert(std::size(kKindNames) == size_t(NodeKind::kCount));

const std::vector<NodeInfo> kInfos = {
    {TR_NOOP("Output"), TR_NOOP("Output"), {TR_NOOP("Colour")}, {{1, 0, 1}}, {}, {}, TR_NOOP("The colour drawn. Fog and fades still follow where the game has them.")},
    {TR_NOOP("Game colour"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The pixel as the game lit it: its textures, lights and shadows. Wire this to Output to change nothing.")},
    {TR_NOOP("Base texture"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The surface's colour before lighting, with all its layers. Often paler than it looks in game: multiply in Light's colour to keep a level's mood. Using it makes the surface twice the work to draw.")},
    {TR_NOOP("Light"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The light on this pixel, with its shadows and shine: Game colour over Base texture. Using it makes the surface twice the work to draw.")},
    {TR_NOOP("UV"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The surface's first texture coordinate, (u, v, 0)."), 'U'},
    {TR_NOOP("Screen position"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The pixel's position on screen, (x, y, 0) in pixels.")},
    {TR_NOOP("Key light"), TR_NOOP("Lighting"), {}, {}, {TR_NOOP("Towards camera"), TR_NOOP("Up"), TR_NOOP("Side")}, {0.8f, 0.55f, 0.45f}, TR_NOOP("N.L for one light placed from the camera: 1 facing it, -1 facing away. It looks the same in every scene."), 'K'},
    {TR_NOOP("Sun light"), TR_NOOP("Lighting"), {}, {}, {}, {}, TR_NOOP("N.L for the level's own sun or main light: 1 facing it, -1 facing away.")},
    {TR_NOOP("Rim"), TR_NOOP("Lighting"), {}, {}, {}, {}, TR_NOOP("1 - N.V: 0 facing the camera, 1 at the silhouette (fresnel)."), 'R'},
    {TR_NOOP("Facing up"), TR_NOOP("Lighting"), {}, {}, {}, {}, TR_NOOP("N.up: 1 facing the sky, -1 the ground.")},
    {TR_NOOP("Number"), TR_NOOP("Constants"), {}, {}, {TR_NOOP("Value")}, {1.0f}, TR_NOOP("A constant. Name it to list it on the Parameters panel."), '1'},
    {TR_NOOP("Colour"), TR_NOOP("Constants"), {}, {}, {"R", "G", "B"}, {1.0f, 1.0f, 1.0f}, TR_NOOP("A constant colour. Name it to list it on the Parameters panel."), '3'},
    {TR_NOOP("Add"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, "A + B", 'A'},
    {TR_NOOP("Subtract"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, "A - B"},
    {TR_NOOP("Multiply"), TR_NOOP("Math"), {"A", "B"}, {kOne, kOne}, {}, {}, "A x B", 'M'},
    {TR_NOOP("Divide"), TR_NOOP("Math"), {"A", "B"}, {kOne, kOne}, {}, {}, "A / B", 'D'},
    {TR_NOOP("Mix"), TR_NOOP("Math"), {"A", "B", TR_NOOP("Amount")}, {kZero, kOne, {0.5f, 0.5f, 0.5f}}, {}, {}, TR_NOOP("A at amount 0, B at 1 (lerp)."), 'L'},
    {TR_NOOP("One minus"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, "1 - X", 'O'},
    {TR_NOOP("Clamp 0..1"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("X kept between 0 and 1 (saturate).")},
    {TR_NOOP("Power"), TR_NOOP("Math"), {"X", TR_NOOP("Exponent")}, {kOne, {2, 2, 2}}, {}, {}, TR_NOOP("X to the power of Exponent."), 'E'},
    {TR_NOOP("Min"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, TR_NOOP("The smaller of A and B.")},
    {TR_NOOP("Max"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, TR_NOOP("The larger of A and B.")},
    {TR_NOOP("Step"), TR_NOOP("Cel"), {TR_NOOP("Edge"), "X"}, {{0.2f, 0.2f, 0.2f}, kZero}, {}, {}, TR_NOOP("0 below Edge and 1 above, with a hard, antialiased edge."), 'S'},
    {TR_NOOP("Smooth step"), TR_NOOP("Cel"), {TR_NOOP("Edge 0"), TR_NOOP("Edge 1"), "X"}, {kZero, kOne, kZero}, {}, {}, TR_NOOP("0 below Edge 0, 1 above Edge 1, smooth between.")},
    {TR_NOOP("Bands"), TR_NOOP("Cel"), {"X", TR_NOOP("Count")}, {kZero, {3, 3, 3}}, {}, {}, TR_NOOP("X (0..1) cut into Count flat steps (posterize)."), 'B'},
    {TR_NOOP("Desaturate"), TR_NOOP("Colour"), {TR_NOOP("Colour"), TR_NOOP("Amount")}, {kOne, kOne}, {}, {}, TR_NOOP("Colour turned towards grey by Amount.")},
    {TR_NOOP("Luminance"), TR_NOOP("Colour"), {TR_NOOP("Colour")}, {kOne}, {}, {}, TR_NOOP("The colour's brightness.")},
    {TR_NOOP("Halftone"), TR_NOOP("Cel"), {TR_NOOP("Amount"), TR_NOOP("Cell size")}, {{0.5f, 0.5f, 0.5f}, {6, 6, 6}}, {}, {}, TR_NOOP("1 inside comic dots that grow with Amount (0..1), Cell size pixels apart."), 'H'},
    {TR_NOOP("Custom HLSL"), TR_NOOP("Advanced"), {"a", "b", "c", "d"}, {kZero, kZero, kZero, kZero}, {}, {}, TR_NOOP("Your own HLSL: a function body over ShadeInputs s and float3 a, b, c, d that returns a float3.")},
    {TR_NOOP("Reroute"), TR_NOOP("Utility"), {""}, {kZero}, {}, {}, TR_NOOP("Passes its input on, to tidy wires. Double-click a wire to add one.")},
    {TR_NOOP("Time"), TR_NOOP("Inputs"), {}, {}, {TR_NOOP("Speed")}, {1.0f}, TR_NOOP("Seconds since the game started, times Speed; wraps every hour.")},
    {TR_NOOP("Panner"), TR_NOOP("Utility"), {TR_NOOP("UV"), TR_NOOP("Speed")}, {kZero, {0.1f, 0.0f, 0.0f}}, {}, {}, TR_NOOP("UV moving at Speed (u, v per second). Unwired, UV is the surface's."), 'P', true},
    {TR_NOOP("Noise"), TR_NOOP("Cel"), {TR_NOOP("UV"), TR_NOOP("Scale")}, {kZero, {8, 8, 8}}, {}, {}, TR_NOOP("Smooth value noise, 0..1, Scale cells across UV. Unwired, UV is the surface's."), 'N', true},
    {TR_NOOP("Sine"), TR_NOOP("Math"), {"X"}, {kZero}, {TR_NOOP("Period")}, {1.0f}, TR_NOOP("sin(X), one wave every Period.")},
    {TR_NOOP("Cosine"), TR_NOOP("Math"), {"X"}, {kZero}, {TR_NOOP("Period")}, {1.0f}, TR_NOOP("cos(X), one wave every Period.")},
    {TR_NOOP("Frac"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("The part of X after the decimal point.")},
    {TR_NOOP("Floor"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("X rounded down.")},
    {TR_NOOP("Ceil"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("X rounded up.")},
    {TR_NOOP("Abs"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("X without its sign.")},
    {TR_NOOP("Clamp"), TR_NOOP("Math"), {"X", TR_NOOP("Min"), TR_NOOP("Max")}, {kZero, kZero, kOne}, {}, {}, TR_NOOP("X kept between Min and Max.")},
    {TR_NOOP("Dot"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, TR_NOOP("The dot product of A and B.")},
    {TR_NOOP("Cross"), TR_NOOP("Math"), {"A", "B"}, {{1, 0, 0}, {0, 1, 0}}, {}, {}, TR_NOOP("The cross product of A and B.")},
    {TR_NOOP("Normalize"), TR_NOOP("Math"), {"X"}, {{0, 0, 1}}, {}, {}, TR_NOOP("X scaled to length 1.")},
    {TR_NOOP("Length"), TR_NOOP("Math"), {"X"}, {kZero}, {}, {}, TR_NOOP("How long X is.")},
    {TR_NOOP("Distance"), TR_NOOP("Math"), {"A", "B"}, {kZero, kZero}, {}, {}, TR_NOOP("How far A is from B.")},
    {TR_NOOP("Remap"), TR_NOOP("Math"), {"X", TR_NOOP("In min"), TR_NOOP("In max"), TR_NOOP("Out min"), TR_NOOP("Out max")}, {kZero, kZero, kOne, kZero, kOne}, {}, {}, TR_NOOP("X from In min..In max to Out min..Out max.")},
    {TR_NOOP("If"), TR_NOOP("Math"), {"A", "B", "A > B", "A = B", "A < B"}, {kZero, kZero, kOne, {0.5f, 0.5f, 0.5f}, kZero}, {}, {}, TR_NOOP("Picks by comparing A and B (their first channels).")},
    {TR_NOOP("Component mask"), TR_NOOP("Colour"), {"X"}, {kZero}, {TR_NOOP("Channel (0 R, 1 G, 2 B)")}, {0.0f}, TR_NOOP("One channel of X, in all three.")},
    {TR_NOOP("Make colour"), TR_NOOP("Constants"), {"R", "G", "B"}, {kZero, kZero, kZero}, {}, {}, TR_NOOP("A colour from three numbers (append).")},
    {TR_NOOP("Surface normal"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The direction the surface faces, in camera space, with normal maps where the game's shader uses them.")},
    {TR_NOOP("Camera vector"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The direction to the camera, camera space.")},
    {TR_NOOP("Up vector"), TR_NOOP("Inputs"), {}, {}, {}, {}, TR_NOOP("The direction to the sky, camera space.")},
    {TR_NOOP("Key vector"), TR_NOOP("Lighting"), {}, {}, {TR_NOOP("Towards camera"), TR_NOOP("Up"), TR_NOOP("Side")}, {0.8f, 0.55f, 0.45f}, TR_NOOP("The direction to the light Key light uses, camera space.")},
    {TR_NOOP("Sun vector"), TR_NOOP("Lighting"), {}, {}, {}, {}, TR_NOOP("The direction to the level's sun or main light, camera space.")},
    {TR_NOOP("Sun colour"), TR_NOOP("Lighting"), {}, {}, {}, {}, TR_NOOP("The colour of the level's sun or main light.")},
    {TR_NOOP("Outline"), TR_NOOP("Cel"), {TR_NOOP("Width")}, {{2.5f, 2.5f, 2.5f}}, {}, {}, TR_NOOP("1 on an inked line Width pixels wide just inside the silhouette, and round any part seen in front of another; 0 elsewhere. It keeps its width at any distance."), 'I'},
};

std::string Hlsl(float v) {
  if (!std::isfinite(v)) v = 0;
  std::string s = std::format("{:.7g}", v);
  if (s.find_first_of(".e") == std::string::npos) s += ".0";
  return s;
}

std::string Hlsl(const Float3& v) { return std::format("float3({}, {}, {})", Hlsl(v[0]), Hlsl(v[1]), Hlsl(v[2])); }

}  // namespace

const NodeInfo& Info(NodeKind kind) { return kInfos[size_t(kind)]; }

const std::vector<const char*>& Categories() {
  static const std::vector<const char*> kCategories = {"Inputs", "Lighting", "Constants", "Math", "Cel", "Colour", "Utility", "Advanced"};
  return kCategories;
}

uint32_t CategoryColour(const char* category) {
  struct Entry {
    const char* name;
    uint8_t r, g, b;
  };
  static const Entry kColours[] = {{"Inputs", 52, 104, 160}, {"Lighting", 196, 116, 36}, {"Constants", 62, 132, 66}, {"Math", 82, 84, 96}, {"Cel", 182, 56, 116}, {"Colour", 160, 140, 36}, {"Utility", 100, 100, 104}, {"Advanced", 160, 46, 46}, {"Output", 112, 58, 152}};
  for (const Entry& e : kColours)
    if (std::string_view(e.name) == category) return 0xFF000000u | uint32_t(e.b) << 16 | uint32_t(e.g) << 8 | e.r;
  return 0xFF606060u;
}

Node& ShaderGraph::Add(NodeKind kind, float x, float y) {
  Node n;
  n.id = next_id++;
  n.kind = kind;
  n.inputs = Info(kind).defaults;
  n.settings = Info(kind).setting_defaults;
  if (kind == NodeKind::kCustom) n.code = "return a;";
  n.x = x;
  n.y = y;
  nodes.push_back(std::move(n));
  return nodes.back();
}

void ShaderGraph::Remove(int node) {
  std::erase_if(nodes, [&](const Node& n) { return n.id == node; });
  std::erase_if(links, [&](const Link& l) { return l.from == node || l.to == node; });
}

const Node* ShaderGraph::Find(int id) const {
  const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == id; });
  return it == nodes.end() ? nullptr : &*it;
}

Node* ShaderGraph::Find(int id) { return const_cast<Node*>(std::as_const(*this).Find(id)); }

const Link* ShaderGraph::Into(int node, int input) const {
  const auto it = std::find_if(links.begin(), links.end(), [&](const Link& l) { return l.to == node && l.input == input; });
  return it == links.end() ? nullptr : &*it;
}

bool ShaderGraph::Connect(int from, int to, int input) {
  if (from == to || !Find(from) || !Find(to)) return false;
  // `from` must not already depend on `to`.
  std::vector<int> stack = {from};
  std::vector<int> seen;
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    if (n == to) return false;
    if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
    seen.push_back(n);
    for (const Link& l : links)
      if (l.to == n) stack.push_back(l.from);
  }
  std::erase_if(links, [&](const Link& l) { return l.to == to && l.input == input; });
  links.push_back({next_id++, from, to, input});
  return true;
}

std::string GenerateHlsl(const ShaderGraph& graph) {
  // Raw HLSL is the body of Shade, or a whole file when it defines Shade itself, for code that needs more around it,
  // such as its own MOD_FETCH2D.
  if (graph.raw && graph.raw_code.find("Shade(ShadeInputs") != std::string::npos) return graph.raw_code;
  if (graph.raw) return std::format("float3 Shade(ShadeInputs s)\n{{\n{}\n}}\n", graph.raw_code);
  std::string customs, body;
  std::vector<int> emitted;
  std::function<std::string(const Node&)> emit = [&](const Node& n) -> std::string {
    const std::string name = std::format("n{}", n.id);
    if (std::find(emitted.begin(), emitted.end(), n.id) != emitted.end()) return name;
    std::vector<std::string> in(n.inputs.size());
    for (size_t i = 0; i < in.size(); ++i) {
      const Link* l = graph.Into(n.id, int(i));
      const Node* from = l ? graph.Find(l->from) : nullptr;
      in[i] = from ? emit(*from) : i == 0 && Info(n.kind).uv_input ? std::string("float3(s.uv, 0.0)") : Hlsl(n.inputs[i]);
    }
    const auto set = [&](size_t i) { return Hlsl(i < n.settings.size() ? n.settings[i] : 0.0f); };
    std::string e;
    switch (n.kind) {
      case NodeKind::kOutput: e = in[0]; break;
      case NodeKind::kGameColour: e = "s.colour"; break;
      case NodeKind::kAlbedo: e = "s.albedo"; break;
      case NodeKind::kLight: e = "s.light"; break;
      case NodeKind::kUv: e = "float3(s.uv, 0.0)"; break;
      case NodeKind::kScreen: e = "float3(s.screen, 0.0)"; break;
      case NodeKind::kKeyLight: e = std::format("(float3)ModKeyLight(s, {}, {}, {})", set(0), set(1), set(2)); break;
      case NodeKind::kSunLight: e = "(float3)dot(s.normal, s.light_dir)"; break;
      case NodeKind::kFresnel: e = "(float3)ModFresnel(s)"; break;
      case NodeKind::kUpFacing: e = "(float3)dot(s.normal, s.up)"; break;
      case NodeKind::kFloat: e = std::format("(float3){}", set(0)); break;
      case NodeKind::kColor: e = std::format("float3({}, {}, {})", set(0), set(1), set(2)); break;
      case NodeKind::kAdd: e = std::format("({} + {})", in[0], in[1]); break;
      case NodeKind::kSubtract: e = std::format("({} - {})", in[0], in[1]); break;
      case NodeKind::kMultiply: e = std::format("({} * {})", in[0], in[1]); break;
      case NodeKind::kDivide: e = std::format("ModDivide3({}, {})", in[0], in[1]); break;
      case NodeKind::kLerp: e = std::format("lerp({}, {}, {})", in[0], in[1], in[2]); break;
      case NodeKind::kOneMinus: e = std::format("(1.0 - {})", in[0]); break;
      case NodeKind::kSaturate: e = std::format("saturate({})", in[0]); break;
      case NodeKind::kPower: e = std::format("pow(max({}, 0.0), {})", in[0], in[1]); break;
      case NodeKind::kMin: e = std::format("min({}, {})", in[0], in[1]); break;
      case NodeKind::kMax: e = std::format("max({}, {})", in[0], in[1]); break;
      case NodeKind::kStep: e = std::format("ModStep3({}, {})", in[0], in[1]); break;
      case NodeKind::kSmoothstep: e = std::format("smoothstep({}, {}, {})", in[0], in[1], in[2]); break;
      case NodeKind::kBands: e = std::format("ModBands3({}, {})", in[0], in[1]); break;
      case NodeKind::kDesaturate: e = std::format("lerp({0}, (float3)ModLuminance({0}), saturate(({1}).x))", in[0], in[1]); break;
      case NodeKind::kLuminance: e = std::format("(float3)ModLuminance({})", in[0]); break;
      case NodeKind::kHalftone: e = std::format("(float3)ModHalftone(s.screen, ({}).x, ({}).x)", in[0], in[1]); break;
      case NodeKind::kCustom:
        customs += std::format("float3 ModCustom{}(ShadeInputs s, float3 a, float3 b, float3 c, float3 d)\n{{\n{}\n}}\n\n", n.id, n.code);
        e = std::format("ModCustom{}(s, {}, {}, {}, {})", n.id, in[0], in[1], in[2], in[3]);
        break;
      case NodeKind::kReroute: e = in[0]; break;
      case NodeKind::kTime: e = std::format("(float3)(s.time * {})", set(0)); break;
      case NodeKind::kPanner: e = std::format("float3(({}).xy + ({}).xy * s.time, 0.0)", in[0], in[1]); break;
      case NodeKind::kNoise: e = std::format("(float3)ModNoise(({}).xy * ({}).x)", in[0], in[1]); break;
      case NodeKind::kSine: e = std::format("sin({} * 6.2831853 / max(abs({}), 1e-5))", in[0], set(0)); break;
      case NodeKind::kCosine: e = std::format("cos({} * 6.2831853 / max(abs({}), 1e-5))", in[0], set(0)); break;
      case NodeKind::kFrac: e = std::format("frac({})", in[0]); break;
      case NodeKind::kFloor: e = std::format("floor({})", in[0]); break;
      case NodeKind::kCeil: e = std::format("ceil({})", in[0]); break;
      case NodeKind::kAbs: e = std::format("abs({})", in[0]); break;
      case NodeKind::kClamp: e = std::format("clamp({0}, {1}, max({1}, {2}))", in[0], in[1], in[2]); break;
      case NodeKind::kDot: e = std::format("(float3)dot({}, {})", in[0], in[1]); break;
      case NodeKind::kCross: e = std::format("cross({}, {})", in[0], in[1]); break;
      case NodeKind::kNormalize: e = std::format("({0} * rsqrt(max(dot({0}, {0}), 1e-8)))", in[0]); break;
      case NodeKind::kLength: e = std::format("(float3)length({})", in[0]); break;
      case NodeKind::kDistance: e = std::format("(float3)distance({}, {})", in[0], in[1]); break;
      case NodeKind::kRemap: e = std::format("ModRemap({}, {}, {}, {}, {})", in[0], in[1], in[2], in[3], in[4]); break;
      case NodeKind::kIf: e = std::format("(({0}).x > ({1}).x ? {2} : (({0}).x == ({1}).x ? {3} : {4}))", in[0], in[1], in[2], in[3], in[4]); break;
      case NodeKind::kComponentMask: e = std::format("(float3)({}).{}", in[0], "rgb"[std::clamp(n.settings.empty() ? 0 : int(n.settings[0]), 0, 2)]); break;
      case NodeKind::kMakeColour: e = std::format("float3(({}).x, ({}).x, ({}).x)", in[0], in[1], in[2]); break;
      case NodeKind::kNormal: e = "s.normal"; break;
      case NodeKind::kCameraVector: e = "s.view"; break;
      case NodeKind::kUpVector: e = "s.up"; break;
      case NodeKind::kKeyVector: e = std::format("ModKeyDirection(s, {}, {}, {})", set(0), set(1), set(2)); break;
      case NodeKind::kSunVector: e = "s.light_dir"; break;
      case NodeKind::kSunColour: e = "s.light_colour"; break;
      case NodeKind::kOutline: e = std::format("(float3)ModOutline(s, ({}).x)", in[0]); break;
      case NodeKind::kCount: break;
    }
    body += std::format("    const float3 {} = {};\n", name, e);
    emitted.push_back(n.id);
    return name;
  };
  const auto output = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const Node& n) { return n.kind == NodeKind::kOutput; });
  const std::string result = output == graph.nodes.end() ? "s.colour" : emit(*output);
  return std::format("{}float3 Shade(ShadeInputs s)\n{{\n{}    return {};\n}}\n", customs, body, result);
}

const char* PresetName(Preset preset) {
  constexpr const char* kNames[] = {TR_NOOP("The game's own look"), TR_NOOP("Cel shade"), TR_NOOP("Toon bands"), TR_NOOP("Flat colour"), TR_NOOP("Rim lit"), TR_NOOP("Comic halftone")};
  return kNames[size_t(preset)];
}

ShaderGraph MakePreset(Preset preset) {
  ShaderGraph g;
  if (preset == Preset::kGameLook) {
    const int colour = g.Add(NodeKind::kGameColour, 0, 0).id;
    g.Connect(colour, g.Add(NodeKind::kOutput, 300, 0).id, 0);
    return g;
  }
  const int albedo = g.Add(NodeKind::kAlbedo, 0, 0).id;
  const int key = g.Add(NodeKind::kKeyLight, 0, 140).id;
  // From the side, so the shadow makes a shape across a character, not just a rim.
  g.Find(key)->settings = {0.45f, 0.55f, 0.7f};
  const int output = g.Add(NodeKind::kOutput, 900, 60).id;
  if (preset == Preset::kFlat) {
    g.Remove(key);
    g.Connect(albedo, output, 0);
    return g;
  }
  // The shared start: a shadow colour where the key light does not reach, the lit colour where it does.
  Node& tint = g.Add(NodeKind::kColor, 250, 260);
  // Dark, as the game lights in linear light and brightens the picture at the end.
  tint.settings = {0.075f, 0.055f, 0.14f};
  tint.name = Tr("Shadow tint");
  const int tint_id = tint.id;
  int lit = 0;
  if (preset == Preset::kToonBands) {
    const int clamp = g.Add(NodeKind::kSaturate, 250, 140).id;
    g.Connect(key, clamp, 0);
    Node& bands = g.Add(NodeKind::kBands, 420, 140);
    bands.inputs[1] = {3, 3, 3};
    lit = bands.id;
    g.Connect(clamp, lit, 0);
    g.Find(tint_id)->settings = {0.45f, 0.40f, 0.58f};
  } else {
    lit = g.Add(NodeKind::kStep, 250, 140).id;
    g.Connect(key, lit, 1);
  }
  const int mix = g.Add(NodeKind::kLerp, 560, 180).id;
  g.Connect(tint_id, mix, 0);
  g.Connect(lit, mix, 2);
  const int shaded = g.Add(NodeKind::kMultiply, 720, 60).id;
  g.Connect(albedo, shaded, 0);
  g.Connect(mix, shaded, 1);
  int result = shaded;
  if (preset == Preset::kRimLit) {
    const int rim = g.Add(NodeKind::kFresnel, 250, 380).id;
    Node& edge = g.Add(NodeKind::kStep, 420, 380);
    edge.inputs[0] = {0.65f, 0.65f, 0.65f};
    const int edge_id = edge.id;
    g.Connect(rim, edge_id, 1);
    Node& colour = g.Add(NodeKind::kColor, 420, 500);
    colour.settings = {0.6f, 0.45f, 0.6f};
    colour.name = Tr("Rim colour");
    const int colour_id = colour.id;
    const int glow = g.Add(NodeKind::kMultiply, 600, 420).id;
    g.Connect(edge_id, glow, 0);
    g.Connect(colour_id, glow, 1);
    result = g.Add(NodeKind::kAdd, 780, 200).id;
    g.Connect(shaded, result, 0);
    g.Connect(glow, result, 1);
  } else if (preset == Preset::kComicHalftone) {
    // Dots in the shadow side, as a comic prints them.
    const int dark = g.Add(NodeKind::kOneMinus, 420, 380).id;
    g.Connect(lit, dark, 0);
    Node& half = g.Add(NodeKind::kMultiply, 560, 380);
    half.inputs[1] = {0.5f, 0.5f, 0.5f};
    const int half_id = half.id;
    g.Connect(dark, half_id, 0);
    Node& dots = g.Add(NodeKind::kHalftone, 700, 380);
    dots.inputs[1] = {5, 5, 5};
    const int dots_id = dots.id;
    g.Connect(half_id, dots_id, 0);
    Node& darker = g.Add(NodeKind::kMultiply, 720, 240);
    darker.inputs[1] = {0.55f, 0.5f, 0.6f};
    const int darker_id = darker.id;
    g.Connect(shaded, darker_id, 0);
    result = g.Add(NodeKind::kLerp, 860, 240).id;
    g.Connect(shaded, result, 0);
    g.Connect(darker_id, result, 1);
    g.Connect(dots_id, result, 2);
  }
  // The comic looks are inked round the edge.
  if (preset == Preset::kCelShade || preset == Preset::kComicHalftone) {
    Node& width = g.Add(NodeKind::kFloat, 700, 560);
    width.settings = {2.5f};
    width.name = Tr("Ink width");
    const int width_id = width.id;
    const int outline = g.Add(NodeKind::kOutline, 860, 520).id;
    g.Connect(width_id, outline, 0);
    Node& ink = g.Add(NodeKind::kColor, 860, 620);
    ink.settings = {0.012f, 0.01f, 0.02f};
    ink.name = Tr("Ink colour");
    const int ink_id = ink.id;
    const int inked = g.Add(NodeKind::kLerp, 1050, 300).id;
    g.Connect(result, inked, 0);
    g.Connect(ink_id, inked, 1);
    g.Connect(outline, inked, 2);
    result = inked;
  }
  g.Find(output)->x = 1250;
  g.Connect(result, output, 0);
  return g;
}

namespace {

nlohmann::json ToJson(const ShaderGraph& g) {
  nlohmann::json nodes = nlohmann::json::array(), links = nlohmann::json::array(), comments = nlohmann::json::array();
  for (const Node& n : g.nodes) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const Float3& v : n.inputs) inputs.push_back({v[0], v[1], v[2]});
    nodes.push_back({{"id", n.id}, {"kind", kKindNames[size_t(n.kind)]}, {"inputs", inputs}, {"settings", n.settings}, {"code", n.code}, {"name", n.name}, {"x", n.x}, {"y", n.y}});
  }
  for (const Link& l : g.links) links.push_back({{"id", l.id}, {"from", l.from}, {"to", l.to}, {"input", l.input}});
  for (const Comment& c : g.comments) comments.push_back({{"id", c.id}, {"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}, {"text", c.text}, {"colour", {c.colour[0], c.colour[1], c.colour[2]}}});
  return {{"nodes", nodes}, {"links", links}, {"comments", comments}, {"next_id", g.next_id}, {"raw", g.raw}, {"raw_code", g.raw_code}};
}

ShaderGraph FromJson(const nlohmann::json& j) {
  ShaderGraph g;
  g.next_id = j.value("next_id", 1);
  g.raw = j.value("raw", false);
  g.raw_code = j.value("raw_code", std::string());
  for (const auto& jn : j.at("nodes")) {
    const std::string kind = jn.at("kind");
    const auto k = std::find(std::begin(kKindNames), std::end(kKindNames), kind);
    if (k == std::end(kKindNames)) continue;
    Node n;
    n.id = jn.at("id");
    n.kind = NodeKind(k - std::begin(kKindNames));
    n.inputs = Info(n.kind).defaults;
    const auto& inputs = jn.value("inputs", nlohmann::json::array());
    for (size_t i = 0; i < n.inputs.size() && i < inputs.size(); ++i) n.inputs[i] = {inputs[i][0], inputs[i][1], inputs[i][2]};
    n.settings = Info(n.kind).setting_defaults;
    const auto& settings = jn.value("settings", nlohmann::json::array());
    for (size_t i = 0; i < n.settings.size() && i < settings.size(); ++i) n.settings[i] = settings[i];
    n.code = jn.value("code", std::string());
    n.name = jn.value("name", std::string());
    n.x = jn.value("x", 0.0f);
    n.y = jn.value("y", 0.0f);
    g.next_id = std::max(g.next_id, n.id + 1);
    g.nodes.push_back(std::move(n));
  }
  for (const auto& jl : j.at("links")) {
    Link l{jl.at("id"), jl.at("from"), jl.at("to"), jl.at("input")};
    if (!g.Find(l.from) || !g.Find(l.to)) continue;
    g.next_id = std::max(g.next_id, l.id + 1);
    g.links.push_back(l);
  }
  for (const auto& jc : j.value("comments", nlohmann::json::array())) {
    Comment c;
    c.id = jc.value("id", 0);
    c.x = jc.value("x", 0.0f), c.y = jc.value("y", 0.0f), c.w = jc.value("w", 300.0f), c.h = jc.value("h", 200.0f);
    c.text = jc.value("text", std::string("Comment"));
    if (const auto& col = jc.value("colour", nlohmann::json::array()); col.size() == 3) c.colour = {col[0], col[1], col[2]};
    g.next_id = std::max(g.next_id, c.id + 1);
    g.comments.push_back(std::move(c));
  }
  return g;
}

}  // namespace

bool SaveMod(const ShaderMod& mod, const std::filesystem::path& path, std::string& error) {
  std::ofstream out(path, std::ios::trunc);
  out << nlohmann::json({{"version", 1}, {"targets", mod.targets}, {"unlit", mod.unlit}, {"graph", ToJson(mod.graph)}}).dump(2);
  if (out) return true;
  error = TrFormat("could not write {0}", {path.string()});
  return false;
}

std::optional<ShaderMod> LoadMod(const std::filesystem::path& path, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = TrFormat("could not read {0}", {path.string()});
    return std::nullopt;
  }
  const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
  if (j.is_discarded()) {
    error = TrFormat("{0} is not valid JSON", {path.filename().string()});
    return std::nullopt;
  }
  try {
    ShaderMod mod;
    mod.graph = FromJson(j.at("graph"));
    mod.targets = j.value("targets", std::vector<std::string>{});
    mod.unlit = j.value("unlit", false);
    return mod;
  } catch (const std::exception& e) {
    error = std::format("{}: {}", path.filename().string(), e.what());
    return std::nullopt;
  }
}

std::string GraphToText(const ShaderGraph& graph) { return nlohmann::json({{"kameo_shader_nodes", 1}, {"graph", ToJson(graph)}}).dump(); }

std::optional<ShaderGraph> GraphFromText(const std::string& text) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.contains("kameo_shader_nodes")) return std::nullopt;
  try {
    return FromJson(j.at("graph"));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace kameo::shader_editor
