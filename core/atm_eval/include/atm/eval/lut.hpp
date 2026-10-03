#pragma once
// Colour lookup tables in the Adobe/Resolve ".cube" text format, the one every grading tool exports. A 3D table
// (LUT_3D_SIZE n) has n*n*n RGB entries with red changing fastest; a 1D table (LUT_1D_SIZE n) has n entries that map
// each channel on its own. DOMAIN_MIN / DOMAIN_MAX rescale the input; TITLE and comments are ignored. Values are
// display-referred RGB, 0..1 (the picture is converted from its video-range BT.709 YUV before the lookup).

#include <string>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::eval {

struct Lut {
  int size = 0;
  bool three_d = true;
  float domain_min[3] = {0.0f, 0.0f, 0.0f};
  float domain_max[3] = {1.0f, 1.0f, 1.0f};
  std::vector<float> rgb; // three_d: size^3 triples (red fastest); else size triples (one per step of every channel)

  // The table's output for one colour (0..1 each; values outside the domain are clamped to it): trilinear for a 3D
  // table, linear per channel for a 1D one.
  void sample(float r, float g, float b, float out[3]) const;
};

// Errors carry the rule LUT_FORMAT with the line that failed in the message.
Result<Lut> parse_cube(std::string_view text);
Result<Lut> load_cube(const std::string &path); // LUT_FILE when the file cannot be read

} // namespace atm::eval
