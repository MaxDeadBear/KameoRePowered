#pragma once

// Ability energy at any frame rate; see ability_energy.cpp.

namespace kameo::engine {

// While one is alive, energy spent through ObjPlaySetValue is treated as a per-tick drain and scaled by the frame time; held abilities whose tick is hooked elsewhere (WaterCannonTick, in input/mouse_aim.cpp) open one around the original.
class PerTickEnergyDrainScope {
 public:
  PerTickEnergyDrainScope();
  ~PerTickEnergyDrainScope();
  PerTickEnergyDrainScope(const PerTickEnergyDrainScope&) = delete;
  PerTickEnergyDrainScope& operator=(const PerTickEnergyDrainScope&) = delete;

 private:
  bool previous_;
};

}  // namespace kameo::engine
