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
                                  "reroute", "time", "panner", "noise", "sine", "cosine", "frac", "floor", "ceil", "abs", "clamp", "dot", "cross", "normalize", "length", "distance", "remap", "if", "component_mask", "make_colour", "normal", "camera_vector", "up_vector", "key_vector", "sun_vector", "sun_colour", "outline", "game_texture"};
static_assert(std::size(kKindNames) == size_t(NodeKind::kCount));

const std::vector<NodeInfo> kInfos = {
    {TR_NOOP("Output"), TR_NOOP("Output"), {TR_NOOP("Colour"), TR_NOOP("Opacity")}, {{1, 0, 1}, kOne}, {}, {}, TR_NOOP("The colour drawn. Fog and fades still follow where the game has them. Opacity below 1 lets the scene show through in a fine dither (a hologram, a ghost).")},
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
    {TR_NOOP("Game texture"), TR_NOOP("Inputs"), {TR_NOOP("UV")}, {kZero}, {TR_NOOP("Read alpha")}, {0.0f}, TR_NOOP("One of the game's own textures, such as an enemy's or another character's, read at UV (the surface's while unwired) and repeating. Choose it in the Details panel. A mod names the texture rather than carrying it, so it holds none of the game's pictures; up to four in a mod."), 'G', true},
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
  // A mod also says how much of the surface shows (shader_mod.hlsli); one that doesn't shows all of it.
  constexpr const char* kSolid = "float ShadeOpacity(ShadeInputs s)\n{\n    return 1.0;\n}\n";
  if (graph.raw) {
    const std::string code = graph.raw_code.find("Shade(ShadeInputs") != std::string::npos ? graph.raw_code : std::format("float3 Shade(ShadeInputs s)\n{{\n{}\n}}\n", graph.raw_code);
    return code.find("ShadeOpacity(ShadeInputs") != std::string::npos ? code : code + kSolid;
  }
  std::string customs, body;
  std::vector<int> emitted;
  const std::vector<GameTextureRef> textures = GameTextures(graph);
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
        if (customs.find(std::format("ModCustom{}(", n.id)) == std::string::npos)
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
      case NodeKind::kGameTexture: {
        const std::optional<GameTextureRef> texture = ParseGameTexture(n.code);
        const auto slot = texture ? std::find(textures.begin(), textures.end(), *texture) : textures.end();
        // One not chosen yet, or past the fourth, reads nothing.
        if (slot == textures.end()) e = "(float3)0.0";
        else e = std::format("ModGameTexture({}u, ({}).xy).{}", slot - textures.begin(), in[0], !n.settings.empty() && n.settings[0] >= 0.5f ? "aaa" : "rgb");
        break;
      }
      case NodeKind::kCount: break;
    }
    body += std::format("    const float3 {} = {};\n", name, e);
    emitted.push_back(n.id);
    return name;
  };
  const auto output = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const Node& n) { return n.kind == NodeKind::kOutput; });
  const std::string result = output == graph.nodes.end() ? "s.colour" : emit(*output);
  const std::string shade = std::format("float3 Shade(ShadeInputs s)\n{{\n{}    return {};\n}}\n", body, result);
  // Opacity is worked out on its own: the nodes it shares with the colour are emitted again, and the compiler merges
  // them once both are inlined.
  const Link* opacity_link = output == graph.nodes.end() ? nullptr : graph.Into(output->id, 1);
  const Node* opacity_node = opacity_link ? graph.Find(opacity_link->from) : nullptr;
  if (!opacity_node) {
    const float constant = output == graph.nodes.end() || output->inputs.size() < 2 ? 1.0f : output->inputs[1][0];
    if (constant >= 1.0f) return customs + shade + kSolid;
    return std::format("{}{}float ShadeOpacity(ShadeInputs s)\n{{\n    return saturate({});\n}}\n", customs, shade, Hlsl(constant));
  }
  body.clear(), emitted.clear();
  const std::string opacity = emit(*opacity_node);
  return std::format("{}{}float ShadeOpacity(ShadeInputs s)\n{{\n{}    return saturate(({}).x);\n}}\n", customs, shade, body, opacity);
}

namespace {

// The game's holograms (the Chamber of Portraits' holo creatures): the surface's own light turned to one tint, a glow
// round the edge where it turns away from the camera, and see-through but for the edge. Scan lines rolling up the
// screen and a flicker make it a projection.
ShaderGraph MakeHologram() {
  ShaderGraph g;
  const auto named = [&](NodeKind kind, float x, float y, const std::string& name, std::vector<float> settings) {
    Node& n = g.Add(kind, x, y);
    n.name = name, n.settings = std::move(settings);
    return n.id;
  };
  const auto node = [&](NodeKind kind, float x, float y) { return g.Add(kind, x, y).id; };
  const auto input = [&](int id, size_t i, float v) { g.Find(id)->inputs[i] = {v, v, v}; };

  // The tinted body.
  const int game = node(NodeKind::kGameColour, 0, 0);
  const int luminance = node(NodeKind::kLuminance, 180, 0);
  g.Connect(game, luminance, 0);
  const int tint = named(NodeKind::kColor, 180, 100, Tr("Hologram colour"), {0.3f, 0.8f, 1.0f});
  const int brightness = named(NodeKind::kFloat, 180, 200, Tr("Brightness"), {2.2f});
  const int tinted = node(NodeKind::kMultiply, 380, 40);
  g.Connect(luminance, tinted, 0), g.Connect(tint, tinted, 1);
  const int bright = node(NodeKind::kMultiply, 540, 80);
  g.Connect(tinted, bright, 0), g.Connect(brightness, bright, 1);
  const int keep = named(NodeKind::kFloat, 380, 180, Tr("Own colours"), {0.15f});
  const int body = node(NodeKind::kLerp, 720, 60);
  g.Connect(bright, body, 0), g.Connect(game, body, 1), g.Connect(keep, body, 2);

  // The edge glow.
  const int rim = node(NodeKind::kFresnel, 0, 320);
  const int edge = node(NodeKind::kPower, 180, 320);
  g.Connect(rim, edge, 0);
  input(edge, 1, 2.0f);
  const int glow_colour = named(NodeKind::kColor, 180, 420, Tr("Edge glow"), {0.75f, 0.95f, 1.0f});
  const int glow = node(NodeKind::kMultiply, 380, 340);
  g.Connect(edge, glow, 0), g.Connect(glow_colour, glow, 1);
  const int glowing = node(NodeKind::kAdd, 900, 120);
  g.Connect(body, glowing, 0), g.Connect(glow, glowing, 1);

  // Scan lines: rows of the screen, rolling up.
  const int screen = node(NodeKind::kScreen, 0, 560);
  Node& row = g.Add(NodeKind::kComponentMask, 180, 560);
  row.settings = {1.0f};
  const int row_id = row.id;
  g.Connect(screen, row_id, 0);
  const int spacing = named(NodeKind::kFloat, 180, 660, Tr("Line spacing"), {6.0f});
  const int rows = node(NodeKind::kDivide, 380, 580);
  g.Connect(row_id, rows, 0), g.Connect(spacing, rows, 1);
  const int roll = named(NodeKind::kTime, 380, 680, Tr("Line speed"), {2.0f});
  const int rolled = node(NodeKind::kAdd, 540, 600);
  g.Connect(rows, rolled, 0), g.Connect(roll, rolled, 1);
  const int phase = node(NodeKind::kFrac, 700, 600);
  g.Connect(rolled, phase, 0);
  const int line = node(NodeKind::kStep, 860, 600);
  input(line, 0, 0.45f);
  g.Connect(phase, line, 1);
  const int dimmed = node(NodeKind::kMultiply, 1060, 220);
  g.Connect(glowing, dimmed, 0);
  input(dimmed, 1, 0.65f);
  const int lined = node(NodeKind::kLerp, 1240, 160);
  g.Connect(dimmed, lined, 0), g.Connect(glowing, lined, 1), g.Connect(line, lined, 2);

  // How much shows: the body faint, the edge solid, gaps between the lines fainter, all flickering.
  const int opacity = named(NodeKind::kFloat, 380, 440, Tr("Opacity"), {0.45f});
  const int shown = node(NodeKind::kLerp, 700, 400);
  g.Connect(opacity, shown, 0), g.Connect(edge, shown, 2);
  const int between = node(NodeKind::kLerp, 1060, 640);
  input(between, 0, 0.55f);
  g.Connect(line, between, 2);
  const int flicker_time = named(NodeKind::kTime, 700, 760, Tr("Flicker speed"), {12.0f});
  Node& noise = g.Add(NodeKind::kNoise, 860, 760);
  noise.inputs[1] = {1, 1, 1};
  const int noise_id = noise.id;
  g.Connect(flicker_time, noise_id, 0);
  const int flicker = node(NodeKind::kRemap, 1060, 760);
  input(flicker, 3, 0.75f);
  g.Connect(noise_id, flicker, 0);
  const int steady = node(NodeKind::kMultiply, 1240, 520);
  g.Connect(shown, steady, 0), g.Connect(between, steady, 1);
  const int shown_now = node(NodeKind::kMultiply, 1420, 560);
  g.Connect(steady, shown_now, 0), g.Connect(flicker, shown_now, 1);

  const int output = node(NodeKind::kOutput, 1600, 240);
  g.Connect(lined, output, 0), g.Connect(shown_now, output, 1);
  return g;
}

// The game's shadow trolls: a black body that shows only where a wall of screaming faces does. The faces scroll as
// theirs do (the material's texture transform: 0.25 across and 1 down a second), pushed about by a streaked map fixed
// to the surface (texbem, by 0.1 of its value). Both textures are the troll's own, from QuadapusCave (024CE245.lvl),
// which the mod names and the game reads from the player's own files.
ShaderGraph MakeShadowTroll() {
  ShaderGraph g;
  const auto named = [&](NodeKind kind, float x, float y, const std::string& name, std::vector<float> settings) {
    Node& n = g.Add(kind, x, y);
    n.name = name, n.settings = std::move(settings);
    return n.id;
  };
  const auto node = [&](NodeKind kind, float x, float y) { return g.Add(kind, x, y).id; };
  const auto input = [&](int id, size_t i, Float3 v) { g.Find(id)->inputs[i] = v; };
  const auto texture = [&](float x, float y, uint32_t entry, uint32_t size, bool alpha) {
    Node& n = g.Add(NodeKind::kGameTexture, x, y);
    n.code = FormatGameTexture({"024CE245.lvl", entry, size, size, "ShadowTroll"});
    n.settings = {alpha ? 1.0f : 0.0f};
    return n.id;
  };

  // The push: the streaked map where it lies on the surface.
  const int streaks = texture(0, 0, 74, 256, false);
  const int swirl = named(NodeKind::kFloat, 180, 100, Tr("Swirl strength"), {0.1f});
  const int push = node(NodeKind::kMultiply, 380, 20);
  g.Connect(streaks, push, 0), g.Connect(swirl, push, 1);

  // The faces, scrolling.
  const int uv = node(NodeKind::kUv, 0, 220);
  const int face_size = named(NodeKind::kFloat, 0, 320, Tr("Face tiling"), {2.0f});
  const int tiled = node(NodeKind::kMultiply, 180, 240);
  g.Connect(uv, tiled, 0), g.Connect(face_size, tiled, 1);
  const int time = node(NodeKind::kTime, 0, 440);
  const int speed = named(NodeKind::kFloat, 0, 540, Tr("Scroll speed"), {1.0f});
  const int seconds = node(NodeKind::kMultiply, 180, 460);
  g.Connect(time, seconds, 0), g.Connect(speed, seconds, 1);
  const int scroll = node(NodeKind::kMultiply, 380, 460);
  input(scroll, 1, {0.25f, -1.0f, 0.0f});
  g.Connect(seconds, scroll, 0);
  const int scrolled = node(NodeKind::kAdd, 560, 300);
  g.Connect(tiled, scrolled, 0), g.Connect(scroll, scrolled, 1);
  const int face_uv = node(NodeKind::kAdd, 720, 160);
  g.Connect(scrolled, face_uv, 0), g.Connect(push, face_uv, 1);
  const int faces = texture(880, 160, 75, 128, true);
  g.Connect(face_uv, faces, 0);

  // The body: black, as their face texture is, with the game's light on it barely showing its shape.
  const int game = node(NodeKind::kGameColour, 0, 680);
  const int luminance = node(NodeKind::kLuminance, 180, 680);
  g.Connect(game, luminance, 0);
  const int body_colour = named(NodeKind::kColor, 180, 780, Tr("Shadow colour"), {0.06f, 0.06f, 0.07f});
  const int body = node(NodeKind::kMultiply, 380, 700);
  g.Connect(luminance, body, 0), g.Connect(body_colour, body, 1);

  // Shown only where the faces are.
  const int opacity = named(NodeKind::kFloat, 880, 320, Tr("Opacity"), {1.0f});
  const int shown = node(NodeKind::kMultiply, 1080, 220);
  g.Connect(faces, shown, 0), g.Connect(opacity, shown, 1);

  const int output = node(NodeKind::kOutput, 1280, 420);
  g.Connect(body, output, 0), g.Connect(shown, output, 1);
  return g;
}

// Grass for the levels (the "Grass and plants" target): only its green pixels change, found from the surface's own
// colour, since the same shaders draw the rock and earth beside it. On them: broad patches of sunnier and lusher green
// to break up the tiling, richer colour, gusts of wind rolling over as a sheen, a speckle of blades, and the glow of
// sunlight through it when the sun is ahead. All of it rides on the game's own light, so shadows stay where they are.
ShaderGraph MakeGrass() {
  ShaderGraph g;
  const auto named = [&](NodeKind kind, float x, float y, const std::string& name, std::vector<float> settings) {
    Node& n = g.Add(kind, x, y);
    n.name = name, n.settings = std::move(settings);
    return n.id;
  };
  const auto node = [&](NodeKind kind, float x, float y) { return g.Add(kind, x, y).id; };
  const auto input = [&](int id, size_t i, Float3 v) { g.Find(id)->inputs[i] = v; };
  const auto custom = [&](float x, float y, const char* code) {
    Node& n = g.Add(NodeKind::kCustom, x, y);
    n.code = code;
    return n.id;
  };

  const int game = node(NodeKind::kGameColour, 0, 0);
  const int albedo = node(NodeKind::kAlbedo, 0, 120);

  // Which pixels are grass: how much greener than red and blue the surface is, against its green.
  const int detection = named(NodeKind::kFloat, 0, 220, Tr("Grass detection"), {6.0f});
  const int mask = custom(220, 160, "return (float3)saturate((a.g - max(a.r, a.b)) / max(a.g, 1e-3) * b.x);");
  g.Connect(albedo, mask, 0), g.Connect(detection, mask, 1);

  // Patches: a large, soft noise between a sunny and a lush tint.
  const int patches = node(NodeKind::kNoise, 0, 340);
  input(patches, 1, {0.35f, 0.35f, 0.35f});
  const int sunny = named(NodeKind::kColor, 180, 300, Tr("Sunny grass"), {1.12f, 1.06f, 0.74f});
  const int lush = named(NodeKind::kColor, 180, 400, Tr("Lush grass"), {0.8f, 1.08f, 0.86f});
  const int tint = node(NodeKind::kLerp, 380, 320);
  g.Connect(sunny, tint, 0), g.Connect(lush, tint, 1), g.Connect(patches, tint, 2);
  const int tinted = node(NodeKind::kMultiply, 560, 60);
  g.Connect(game, tinted, 0), g.Connect(tint, tinted, 1);

  // Richer colour: away from its own grey.
  const int grey = node(NodeKind::kLuminance, 720, 160);
  g.Connect(tinted, grey, 0);
  const int richness = named(NodeKind::kFloat, 720, 260, Tr("Richness"), {1.3f});
  const int rich = node(NodeKind::kLerp, 900, 80);
  g.Connect(grey, rich, 0), g.Connect(tinted, rich, 1), g.Connect(richness, rich, 2);

  // Wind: slow gusts drifting over the field, brightening the blades they bend.
  const int drift = node(NodeKind::kPanner, 0, 520);
  input(drift, 1, {0.06f, 0.025f, 0.0f});
  const int gusts = node(NodeKind::kNoise, 180, 520);
  input(gusts, 1, {0.6f, 0.6f, 0.6f});
  g.Connect(drift, gusts, 0);
  const int centred = node(NodeKind::kSubtract, 380, 520);
  input(centred, 1, {0.5f, 0.5f, 0.5f});
  g.Connect(gusts, centred, 0);
  const int sheen = named(NodeKind::kFloat, 380, 620, Tr("Wind sheen"), {0.4f});
  const int gust = node(NodeKind::kMultiply, 560, 540);
  g.Connect(centred, gust, 0), g.Connect(sheen, gust, 1);
  const int brighten = node(NodeKind::kAdd, 720, 540);
  input(brighten, 1, {1.0f, 1.0f, 1.0f});
  g.Connect(gust, brighten, 0);
  const int windy = node(NodeKind::kMultiply, 1080, 120);
  g.Connect(rich, windy, 0), g.Connect(brighten, windy, 1);

  // Blades: a fine speckle.
  const int blades = node(NodeKind::kNoise, 720, 680);
  input(blades, 1, {45.0f, 45.0f, 45.0f});
  const int speckle = node(NodeKind::kRemap, 900, 680);
  input(speckle, 3, {0.82f, 0.82f, 0.82f});
  input(speckle, 4, {1.06f, 1.06f, 1.06f});
  g.Connect(blades, speckle, 0);
  const int detailed = node(NodeKind::kMultiply, 1240, 160);
  g.Connect(windy, detailed, 0), g.Connect(speckle, detailed, 1);

  // Sunlight through it, looking towards the sun, the more so on blades seen edge-on.
  const int backlight_strength = named(NodeKind::kFloat, 900, 820, Tr("Backlight"), {0.6f});
  const int backlight = custom(1080, 760,
                               "const float ahead = pow(saturate(dot(-s.view, s.light_dir)), 3.0);\n"
                               "const float edge = 1.0 - saturate(dot(s.normal, s.view));\n"
                               "return s.light_colour * a * ahead * (0.35 + edge) * b.x;");
  g.Connect(albedo, backlight, 0), g.Connect(backlight_strength, backlight, 1);
  // Depth, for flat grass that should look deep. Grass cards (the tall grass, drawn without a surface direction) darken
  // towards their roots, which are at the bottom of their texture, and brighten at the tips. Everywhere, the
  // texture's own light and dark is taken for height: its bumps are lit again from the sun (from over the camera's
  // shoulder where the shader doesn't know the sun), and the gaps between blades go darker.
  const int blade_depth = named(NodeKind::kFloat, 1240, 860, Tr("Blade depth"), {0.55f});
  const int relief_strength = named(NodeKind::kFloat, 1240, 940, Tr("Relief"), {0.6f});
  const int cavities = named(NodeKind::kFloat, 1240, 1020, Tr("Cavities"), {0.35f});
  const int depth = custom(1420, 900,
                           "const float height = dot(a, float3(0.3, 0.59, 0.11));\n"
                           "float shade = 1.0;\n"
                           "if (!s.has_normal) {\n"
                           "    const float root = pow(saturate(s.uv.y), 1.6);\n"
                           "    shade *= lerp(1.0 + b.x * 0.25, 1.0 - b.x, root);\n"
                           "}\n"
                           "const float2 slope = clamp(float2(ddx(height), ddy(height)), -0.08, 0.08);\n"
                           "const float3 n = s.has_normal ? s.normal : float3(0.0, 0.0, 1.0);\n"
                           "const float3 l = s.has_normal ? s.light_dir : ModKeyDirection(s, 0.5, 0.7, 0.5);\n"
                           "const float3 bent = normalize(n + float3(-slope.x, slope.y, 0.0) * c.x * 30.0);\n"
                           "shade *= (saturate(dot(bent, l)) * 0.7 + 0.3) / (saturate(dot(n, l)) * 0.7 + 0.3);\n"
                           "shade *= lerp(1.0 - d.x, 1.0, saturate(height * 2.5));\n"
                           "return (float3)shade;");
  g.Connect(albedo, depth, 0), g.Connect(blade_depth, depth, 1), g.Connect(relief_strength, depth, 2), g.Connect(cavities, depth, 3);
  const int deep = node(NodeKind::kMultiply, 1400, 200);
  g.Connect(detailed, deep, 0), g.Connect(depth, deep, 1);
  const int lit = node(NodeKind::kAdd, 1560, 260);
  g.Connect(deep, lit, 0), g.Connect(backlight, lit, 1);

  // Only on the grass, by Strength.
  const int strength = named(NodeKind::kFloat, 1400, 0, Tr("Strength"), {1.0f});
  const int amount = node(NodeKind::kMultiply, 1560, 60);
  g.Connect(mask, amount, 0), g.Connect(strength, amount, 1);
  const int result = node(NodeKind::kLerp, 1740, 120);
  g.Connect(game, result, 0), g.Connect(lit, result, 1), g.Connect(amount, result, 2);
  // Grass cards fade out over the bottom of their height, so the blades melt into the ground rather than stop at the
  // card's edge.
  const int root_fade = named(NodeKind::kFloat, 1560, 380, Tr("Root fade"), {0.25f});
  const int fade = custom(1740, 380,
                          "if (s.has_normal) return (float3)1.0;\n"
                          "return (float3)(1.0 - smoothstep(1.0 - max(b.x, 1e-3), 1.0, saturate(s.uv.y)));");
  g.Connect(root_fade, fade, 1);
  const int output = node(NodeKind::kOutput, 1920, 160);
  g.Connect(result, output, 0), g.Connect(fade, output, 1);
  return g;
}

}  // namespace

std::optional<GameTextureRef> ParseGameTexture(const std::string& code) {
  std::vector<std::string> parts;
  for (size_t start = 0;;) {
    const size_t bar = code.find('|', start);
    parts.push_back(code.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  if (parts.size() < 4 || parts[0].empty()) return std::nullopt;
  GameTextureRef texture;
  texture.archive = parts[0];
  texture.entry = uint32_t(std::strtoul(parts[1].c_str(), nullptr, 10));
  texture.width = uint32_t(std::strtoul(parts[2].c_str(), nullptr, 10));
  texture.height = uint32_t(std::strtoul(parts[3].c_str(), nullptr, 10));
  if (parts.size() > 4) texture.label = parts[4];
  if (!texture.entry || !texture.width || !texture.height) return std::nullopt;
  return texture;
}

std::string FormatGameTexture(const GameTextureRef& texture) {
  return std::format("{}|{}|{}|{}|{}", texture.archive, texture.entry, texture.width, texture.height, texture.label);
}

bool SeeThrough(const ShaderGraph& graph) {
  if (graph.raw) return graph.raw_code.find("ShadeOpacity(ShadeInputs") != std::string::npos;
  const auto output = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const Node& n) { return n.kind == NodeKind::kOutput; });
  if (output == graph.nodes.end()) return false;
  return graph.Into(output->id, 1) || (output->inputs.size() > 1 && output->inputs[1][0] < 1.0f);
}

std::vector<GameTextureRef> GameTextures(const ShaderGraph& graph) {
  std::vector<const Node*> nodes;
  for (const Node& n : graph.nodes)
    if (n.kind == NodeKind::kGameTexture) nodes.push_back(&n);
  std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
  std::vector<GameTextureRef> textures;
  for (const Node* n : nodes) {
    const std::optional<GameTextureRef> texture = ParseGameTexture(n->code);
    if (texture && textures.size() < kGameTextureSlots && std::find(textures.begin(), textures.end(), *texture) == textures.end()) textures.push_back(*texture);
  }
  return textures;
}

const char* PresetName(Preset preset) {
  constexpr const char* kNames[] = {TR_NOOP("The game's own look"), TR_NOOP("Cel shade"), TR_NOOP("Toon bands"), TR_NOOP("Flat colour"), TR_NOOP("Rim lit"), TR_NOOP("Comic halftone"), TR_NOOP("Hologram"), TR_NOOP("Shadow troll"), TR_NOOP("Lush grass")};
  return kNames[size_t(preset)];
}

ShaderGraph MakePreset(Preset preset) {
  ShaderGraph g;
  if (preset == Preset::kGameLook) {
    const int colour = g.Add(NodeKind::kGameColour, 0, 0).id;
    g.Connect(colour, g.Add(NodeKind::kOutput, 300, 0).id, 0);
    return g;
  }
  if (preset == Preset::kHologram) return MakeHologram();
  if (preset == Preset::kShadowTroll) return MakeShadowTroll();
  if (preset == Preset::kGrass) return MakeGrass();
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
