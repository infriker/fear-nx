// Standalone CPU reference tests for guest_output_bilinear_gamma.frag.glsl.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kGammaEntries = 256;
constexpr uint32_t kUnorm10Max = 1023;

struct Rgb {
  float r;
  float g;
  float b;
};

struct Image {
  uint32_t width;
  uint32_t height;
  std::vector<Rgb> texels;

  Image(uint32_t w, uint32_t h) : width(w), height(h), texels(std::size_t(w) * h) {}

  Rgb& At(uint32_t x, uint32_t y) { return texels[std::size_t(y) * width + x]; }
  const Rgb& At(uint32_t x, uint32_t y) const {
    return texels[std::size_t(y) * width + x];
  }
};

using GammaRamp = std::array<uint32_t, kGammaEntries>;

uint32_t PackGamma(uint32_t red, uint32_t green, uint32_t blue) {
  assert(red <= kUnorm10Max && green <= kUnorm10Max && blue <= kUnorm10Max);
  return (red << 20) | (green << 10) | blue;
}

uint32_t GammaIndex(float normalized_source) {
  // SPIR-V OpConvertFToU truncates after the shader adds 0.5.
  return static_cast<uint32_t>(normalized_source * 255.0f + 0.5f);
}

uint32_t GammaCode(const GammaRamp& ramp, float normalized_source, uint32_t shift) {
  return (ramp[GammaIndex(normalized_source)] >> shift) & kUnorm10Max;
}

float Unorm10(uint32_t code) {
  assert(code <= kUnorm10Max);
  return float(code) / float(kUnorm10Max);
}

float RoundTripUnorm10(float value) {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return float(std::lround(clamped * float(kUnorm10Max))) / float(kUnorm10Max);
}

Rgb GammaAtPixel(const Image& source, uint32_t x, uint32_t y, const GammaRamp& ramp) {
  const Rgb input = source.At(x, y);
  // The packed table's channel fields are the Xenos B/G/R bit fields:
  // the red lookup is bits 20-29, green is 10-19, and blue is 0-9.
  const uint32_t red_code = GammaCode(ramp, input.r, 20);
  const uint32_t green_code = GammaCode(ramp, input.g, 10);
  const uint32_t blue_code = GammaCode(ramp, input.b, 0);
  // The old gamma pass writes A2B10G10R10_UNORM. Each LUT value is already an
  // exact 10-bit code, so this round-trip is idempotent per source tap.
  return {RoundTripUnorm10(Unorm10(red_code)), RoundTripUnorm10(Unorm10(green_code)),
          RoundTripUnorm10(Unorm10(blue_code))};
}

Rgb Mix(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb BilinearRgb(Rgb top_left, Rgb top_right, Rgb bottom_left, Rgb bottom_right,
                float weight_x, float weight_y) {
  return Mix(Mix(top_left, top_right, weight_x),
             Mix(bottom_left, bottom_right, weight_x), weight_y);
}

Rgb SampleGammaBilinear(const Image& source, float u, float v, const GammaRamp& ramp) {
  const float source_x = u * float(source.width) - 0.5f;
  const float source_y = v * float(source.height) - 0.5f;
  const int32_t base_x = int32_t(std::floor(source_x));
  const int32_t base_y = int32_t(std::floor(source_y));
  const float weight_x = source_x - std::floor(source_x);
  const float weight_y = source_y - std::floor(source_y);
  auto tap = [&](int32_t x, int32_t y) {
    const uint32_t clamped_x =
        uint32_t(std::clamp(x, 0, int32_t(source.width) - 1));
    const uint32_t clamped_y =
        uint32_t(std::clamp(y, 0, int32_t(source.height) - 1));
    return GammaAtPixel(source, clamped_x, clamped_y, ramp);
  };
  return BilinearRgb(tap(base_x, base_y), tap(base_x + 1, base_y),
                     tap(base_x, base_y + 1), tap(base_x + 1, base_y + 1),
                     weight_x, weight_y);
}

Rgb SampleRawBilinear(const Image& source, float u, float v) {
  const float source_x = u * float(source.width) - 0.5f;
  const float source_y = v * float(source.height) - 0.5f;
  const int32_t base_x = int32_t(std::floor(source_x));
  const int32_t base_y = int32_t(std::floor(source_y));
  const float weight_x = source_x - std::floor(source_x);
  const float weight_y = source_y - std::floor(source_y);
  auto tap = [&](int32_t x, int32_t y) {
    const uint32_t clamped_x =
        uint32_t(std::clamp(x, 0, int32_t(source.width) - 1));
    const uint32_t clamped_y =
        uint32_t(std::clamp(y, 0, int32_t(source.height) - 1));
    return source.At(clamped_x, clamped_y);
  };
  return BilinearRgb(tap(base_x, base_y), tap(base_x + 1, base_y),
                     tap(base_x, base_y + 1), tap(base_x + 1, base_y + 1),
                     weight_x, weight_y);
}

Rgb GammaAfterBilinear(const Image& source, float u, float v, const GammaRamp& ramp) {
  const Rgb filtered = SampleRawBilinear(source, u, v);
  Image single_pixel(1, 1);
  single_pixel.At(0, 0) = filtered;
  return GammaAtPixel(single_pixel, 0, 0, ramp);
}

bool IsExactOneToOne(uint32_t source_width, uint32_t source_height,
                     float output_width_inv, float output_height_inv) {
  const int32_t output_width = int32_t(std::lround(1.0f / output_width_inv));
  const int32_t output_height = int32_t(std::lround(1.0f / output_height_inv));
  return output_width == int32_t(source_width) &&
         output_height == int32_t(source_height);
}

bool Near(float a, float b, float tolerance = 1.0e-6f) {
  return std::abs(a - b) <= tolerance;
}

void TestRounded8BitIndexAndPackedChannelOrder() {
  for (uint32_t i = 0; i < kGammaEntries; ++i) {
    assert(GammaIndex(float(i) / 255.0f) == i);
  }
  for (uint32_t i = 0; i < 255; ++i) {
    const float below = (float(i) + 0.49f) / 255.0f;
    const float above = (float(i) + 0.51f) / 255.0f;
    assert(GammaIndex(below) == i);
    assert(GammaIndex(above) == i + 1);
  }
  assert(GammaIndex(0.0f) == 0);
  assert(GammaIndex(1.0f) == 255);

  GammaRamp ramp{};
  ramp[64] = PackGamma(100, 200, 300);
  ramp[128] = PackGamma(400, 500, 600);
  ramp[192] = PackGamma(700, 800, 900);
  Image source(1, 1);
  source.At(0, 0) = {64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f};
  const Rgb output = GammaAtPixel(source, 0, 0, ramp);
  assert(Near(output.r, Unorm10(100)));
  assert(Near(output.g, Unorm10(500)));
  assert(Near(output.b, Unorm10(900)));
}

void TestGammaRunsBeforeBilinearAndTapsAre10Bit() {
  GammaRamp ramp{};
  for (uint32_t i = 0; i < kGammaEntries; ++i) {
    const uint32_t code =
        (i * i * kUnorm10Max + (255u * 255u) / 2u) / (255u * 255u);
    ramp[i] = PackGamma(code, code, code);
  }

  Image source(2, 1);
  source.At(0, 0) = {0.0f, 0.0f, 0.0f};
  source.At(1, 0) = {1.0f, 1.0f, 1.0f};

  // At the exact midpoint the old chain maps black and white independently,
  // stores those 10-bit values, then filters them to 0.5. Filtering raw input
  // first maps index 128 through the nonlinear ramp and is clearly different.
  const Rgb gamma_then_filter = SampleGammaBilinear(source, 0.5f, 0.5f, ramp);
  const Rgb filter_then_gamma = GammaAfterBilinear(source, 0.5f, 0.5f, ramp);
  assert(Near(gamma_then_filter.r, 0.5f));
  assert(Near(gamma_then_filter.g, 0.5f));
  assert(Near(gamma_then_filter.b, 0.5f));
  assert(filter_then_gamma.r < 0.27f);
  assert(gamma_then_filter.r - filter_then_gamma.r > 0.23f);

  // Code values survive the UNORM10 store/read round-trip before filtering.
  ramp[0] = PackGamma(1, 1, 1);
  ramp[255] = PackGamma(2, 2, 2);
  const Rgb half_lsb_pair = SampleGammaBilinear(source, 0.5f, 0.5f, ramp);
  assert(Near(half_lsb_pair.r, 1.5f / float(kUnorm10Max)));
  assert(Near(half_lsb_pair.g, 1.5f / float(kUnorm10Max)));
  assert(Near(half_lsb_pair.b, 1.5f / float(kUnorm10Max)));
}

void TestClampEdgesAndExactSizeFastPath() {
  GammaRamp ramp{};
  for (uint32_t i = 0; i < kGammaEntries; ++i) {
    const uint32_t code = i * kUnorm10Max / 255u;
    ramp[i] = PackGamma(code, code, code);
  }

  Image source(2, 2);
  source.At(0, 0) = {0.0f, 0.25f, 0.5f};
  source.At(1, 0) = {1.0f, 0.75f, 0.5f};
  source.At(0, 1) = {0.5f, 0.25f, 0.0f};
  source.At(1, 1) = {0.5f, 0.75f, 1.0f};

  const Rgb left_edge = SampleGammaBilinear(source, 0.0f, 0.25f, ramp);
  const Rgb expected_left = GammaAtPixel(source, 0, 0, ramp);
  assert(Near(left_edge.r, expected_left.r));
  assert(Near(left_edge.g, expected_left.g));
  assert(Near(left_edge.b, expected_left.b));

  const Rgb right_edge = SampleGammaBilinear(source, 1.0f, 0.25f, ramp);
  const Rgb expected_right = GammaAtPixel(source, 1, 0, ramp);
  assert(Near(right_edge.r, expected_right.r));
  assert(Near(right_edge.g, expected_right.g));
  assert(Near(right_edge.b, expected_right.b));

  assert(IsExactOneToOne(2, 2, 0.5f, 0.5f));
  assert(!IsExactOneToOne(2, 2, 1.0f / 3.0f, 0.5f));
  for (uint32_t y = 0; y < source.height; ++y) {
    for (uint32_t x = 0; x < source.width; ++x) {
      const float u = (float(x) + 0.5f) / float(source.width);
      const float v = (float(y) + 0.5f) / float(source.height);
      const Rgb one_tap = GammaAtPixel(source, x, y, ramp);
      const Rgb four_tap = SampleGammaBilinear(source, u, v, ramp);
      assert(Near(one_tap.r, four_tap.r));
      assert(Near(one_tap.g, four_tap.g));
      assert(Near(one_tap.b, four_tap.b));
    }
  }
}

}  // namespace

int main() {
  TestRounded8BitIndexAndPackedChannelOrder();
  TestGammaRunsBeforeBilinearAndTapsAre10Bit();
  TestClampEdgesAndExactSizeFastPath();
  return 0;
}
