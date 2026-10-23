// Offline compatibility smoke test for real XESH microcode records.
// Reads the cache only; all generated reports and GLSL are written to out/.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include "deko/deko_shaders.h"
#include "deko/deko_geometry_shaders.h"
#include <uam_api.h>

REXCVAR_DECLARE(bool, gpu_implicit_lod_sampling);

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kXeshMagic = 0x48534558;  // "XESH" on disk.
constexpr uint32_t kXeshVersion = 0x20201219;
constexpr uint32_t kMaxReasonableShaderWords = 1u << 20;

struct CacheShader {
  uint64_t hash = 0;
  uint32_t word_count = 0;
  rex::graphics::xenos::ShaderType type = rex::graphics::xenos::ShaderType::kVertex;
  std::vector<uint32_t> guest_words;
};

struct CacheFile {
  uint32_t version = 0;
  std::vector<CacheShader> shaders;
};

std::string Hex(uint64_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
  return stream.str();
}

bool ReadExact(std::ifstream& input, void* destination, size_t bytes) {
  input.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
  return input.good();
}

bool LoadXesh(const fs::path& path, CacheFile& cache, std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open XESH cache: " + path.string();
    return false;
  }

  uint32_t magic = 0;
  uint32_t swapped_version = 0;
  if (!ReadExact(input, &magic, sizeof(magic)) ||
      !ReadExact(input, &swapped_version, sizeof(swapped_version))) {
    error = "truncated XESH file header";
    return false;
  }
  if (magic != kXeshMagic || std::byteswap(swapped_version) != kXeshVersion) {
    error = "unexpected XESH magic or shader-storage version";
    return false;
  }
  cache.version = std::byteswap(swapped_version);

  while (true) {
    uint64_t hash = 0;
    input.read(reinterpret_cast<char*>(&hash), sizeof(hash));
    if (input.eof() && input.gcount() == 0) break;
    if (!input) {
      error = "truncated XESH shader record header";
      return false;
    }

    uint32_t packed_count_and_type = 0;
    if (!ReadExact(input, &packed_count_and_type, sizeof(packed_count_and_type))) {
      error = "truncated XESH shader record header";
      return false;
    }
    const uint32_t word_count = packed_count_and_type & 0x7fffffffu;
    if (word_count == 0 || word_count > kMaxReasonableShaderWords) {
      error = "invalid microcode word count " + std::to_string(word_count) +
              " for shader " + Hex(hash);
      return false;
    }

    CacheShader shader;
    shader.hash = hash;
    shader.word_count = word_count;
    shader.type = (packed_count_and_type >> 31) != 0
                      ? rex::graphics::xenos::ShaderType::kPixel
                      : rex::graphics::xenos::ShaderType::kVertex;
    shader.guest_words.resize(word_count);
    const size_t bytes = size_t(word_count) * sizeof(uint32_t);
    if (!ReadExact(input, shader.guest_words.data(), bytes)) {
      error = "truncated microcode body for shader " + Hex(hash);
      return false;
    }
    const uint64_t computed_hash = XXH3_64bits(shader.guest_words.data(), bytes);
    if (computed_hash != hash) {
      error = "microcode hash mismatch for shader " + Hex(hash) + " (got " +
              Hex(computed_hash) + ")";
      return false;
    }
    cache.shaders.push_back(std::move(shader));
  }

  if (cache.shaders.empty()) {
    error = "XESH cache contains no shaders";
    return false;
  }
  return true;
}

std::string Csv(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char ch : value) {
    if (ch == '"') escaped.push_back('"');
    if (ch == '\r') continue;
    if (ch == '\n') {
      escaped += "\\n";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

std::string TranslationErrors(const rex::graphics::Shader::Translation& translation) {
  std::string text;
  for (const auto& item : translation.errors()) {
    if (!text.empty()) text += " | ";
    text += item.is_fatal ? "fatal: " : "warning: ";
    text += item.message;
  }
  return text;
}

std::string JoinWarnings(const fear_nx::deko::GlslShader& shader) {
  std::string text;
  for (const auto& warning : shader.warnings) {
    if (!text.empty()) text += " | ";
    text += warning;
  }
  return text;
}

uam::Stage UamStage(fear_nx::deko::ShaderStage stage) {
  switch (stage) {
    case fear_nx::deko::ShaderStage::kVertex:
      return uam::Stage::Vertex;
    case fear_nx::deko::ShaderStage::kGeometry:
      return uam::Stage::Geometry;
    case fear_nx::deko::ShaderStage::kFragment:
      return uam::Stage::Fragment;
    case fear_nx::deko::ShaderStage::kCompute:
      return uam::Stage::Compute;
  }
  return uam::Stage::Vertex;
}

bool ValidateSystemConstantMetadata(const fear_nx::deko::GlslShader& shader,
                                    std::string& error) {
  const auto system_buffer = std::find_if(
      shader.uniform_buffers.begin(), shader.uniform_buffers.end(),
      [](const fear_nx::deko::UniformBufferBinding& binding) {
        return binding.source.set == 1 && binding.source.binding == 0;
      });
  const bool has_system_buffer = system_buffer != shader.uniform_buffers.end();
  if (has_system_buffer != (shader.system_constant_size != 0) ||
      has_system_buffer != !shader.system_constant_copy_ranges.empty()) {
    error = "system-constant UBO metadata is missing or unexpectedly present";
    return false;
  }
  if (!has_system_buffer) return true;
  if ((shader.system_constant_size & 15u) != 0 ||
      shader.source.find("std140) uniform XeSystemConstants") == std::string::npos) {
    error = "system-constant UBO is not emitted with a std140-aligned block";
    return false;
  }
  if (shader.source.find("GL_EXT_scalar_block_layout") != std::string::npos ||
      shader.source.find(", scalar)") != std::string::npos) {
    error = "system-constant GLSL still requires scalar block layout";
    return false;
  }

  auto ranges = shader.system_constant_copy_ranges;
  for (const auto& range : ranges) {
    if (!range.size || range.source_offset > sizeof(rex::graphics::SpirvShaderTranslator::SystemConstants) ||
        range.size > sizeof(rex::graphics::SpirvShaderTranslator::SystemConstants) -
                         range.source_offset ||
        range.destination_offset > shader.system_constant_size ||
        range.size > shader.system_constant_size - range.destination_offset) {
      error = "system-constant copy range exceeds the source or packed UBO bounds";
      return false;
    }
  }
  std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
    return a.destination_offset < b.destination_offset;
  });
  for (size_t index = 1; index < ranges.size(); ++index) {
    const auto& previous = ranges[index - 1];
    if (ranges[index].destination_offset < previous.destination_offset + previous.size) {
      error = "system-constant destination copy ranges overlap";
      return false;
    }
  }
  return true;
}

class UamLifetime {
 public:
  UamLifetime() { uam::init(true); }
  ~UamLifetime() { uam::shutdown(); }
  UamLifetime(const UamLifetime&) = delete;
  UamLifetime& operator=(const UamLifetime&) = delete;
};

bool ExtractRawString(const fs::path& path, std::string_view marker,
                      std::string_view closing, std::string& source,
                      std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot read shader source file: " + path.string();
    return false;
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const size_t marker_position = contents.find(marker);
  if (marker_position == std::string::npos) {
    error = "raw GLSL marker not found in " + path.string() + ": " + std::string(marker);
    return false;
  }
  const size_t source_begin = marker_position + marker.size();
  const size_t source_end = contents.find(closing, source_begin);
  if (source_end == std::string::npos || source_end == source_begin) {
    error = "raw GLSL terminator not found in " + path.string();
    return false;
  }
  source.assign(contents, source_begin, source_end - source_begin);
  return true;
}

struct SyntheticResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

SyntheticResult CompileInternalGlsl(std::string name, std::string source,
                                   fear_nx::deko::ShaderStage stage,
                                   const fs::path& output_dir) {
  SyntheticResult result{std::move(name)};
  if (source.empty()) {
    result.detail = "empty GLSL source";
    return result;
  }
  uam::Result compiled = uam::compile(UamStage(stage), source.c_str());
  result.passed = compiled.ok && !compiled.dksh.empty();
  if (!result.passed) {
    result.detail = compiled.log.empty() ? "UAM returned no DKSH" : std::move(compiled.log);
    std::ofstream glsl(output_dir / (result.name + ".glsl"),
                       std::ios::binary | std::ios::trunc);
    glsl << source;
    std::ofstream log(output_dir / (result.name + ".log"),
                      std::ios::binary | std::ios::trunc);
    log << result.detail << '\n';
  }
  return result;
}

SyntheticResult CompileInternalGeometry(std::string name,
                                       fear_nx::native::GeometryShaderKey key,
                                       const fs::path& output_dir) {
  SyntheticResult result{std::move(name)};
  const std::vector<uint32_t> spirv = fear_nx::deko::CreateGeometryShaderSpirv(key);
  if (spirv.empty()) {
    result.detail = "geometry shader generator returned empty SPIR-V";
    return result;
  }
  fear_nx::deko::GlslShader glsl = fear_nx::deko::ConvertSpirv(
      std::span<const uint32_t>(spirv), fear_nx::deko::ShaderStage::kGeometry);
  if (!glsl) {
    result.detail = "SPIRV-Cross: " + glsl.error;
  } else if (!ValidateSystemConstantMetadata(glsl, result.detail)) {
    // Metadata errors are reported directly by the shared validation helper.
  } else if (!glsl.warnings.empty()) {
    result.detail = JoinWarnings(glsl);
  } else {
    uam::Result compiled = uam::compile(uam::Stage::Geometry, glsl.source.c_str());
    result.passed = compiled.ok && !compiled.dksh.empty();
    if (!result.passed) {
      result.detail = compiled.log.empty() ? "UAM returned no DKSH" : std::move(compiled.log);
    }
  }
  if (!result.passed) {
    std::ofstream source(output_dir / (result.name + ".glsl"),
                         std::ios::binary | std::ios::trunc);
    source << glsl.source;
    std::ofstream log(output_dir / (result.name + ".log"),
                      std::ios::binary | std::ios::trunc);
    log << result.detail << '\n';
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4 || std::string_view(argv[1]) == "--help") {
    std::cerr << "Usage: deko_shader_smoke <xesh-cache.xsh> <output-dir> [expected-count]\n";
    return argc >= 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
  }

  const fs::path cache_path(argv[1]);
  const fs::path output_dir(argv[2]);
  size_t expected_count = 0;
  if (argc == 4) {
    try {
      expected_count = std::stoull(argv[3]);
    } catch (...) {
      std::cerr << "Invalid expected-count argument.\n";
      return 2;
    }
  }

  CacheFile cache;
  std::string load_error;
  if (!LoadXesh(cache_path, cache, load_error)) {
    std::cerr << load_error << '\n';
    return 2;
  }
  if (argc == 4 && cache.shaders.size() != expected_count) {
    std::cerr << "Expected " << expected_count << " shader records, found "
              << cache.shaders.size() << ".\n";
    return 2;
  }

  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << "Cannot create output directory: " << ec.message() << '\n';
    return 2;
  }
  std::ofstream report(output_dir / "results.csv", std::ios::binary | std::ios::trunc);
  if (!report) {
    std::cerr << "Cannot create results.csv in " << output_dir << '\n';
    return 2;
  }
  report << "hash,stage,ucode_words,translator_ok,spirv_words,glsl_ok,texture_slots,ubo_slots,"
            "ssbo_slots,storage_image_slots,system_ubo_bytes,system_copy_ranges,uam_ok,"
            "warning_count,details\n";

  rex::graphics::SpirvShaderTranslator::Features features(false);
  REXCVAR_SET(gpu_implicit_lod_sampling, true);
  features.spirv_version = 0x00010000;
  features.max_storage_buffer_range = 512u << 20;
  features.full_draw_index_uint32 = true;
  features.clip_distance = true;
  features.cull_distance = true;
  features.image_view_format_swizzle = true;
  rex::graphics::SpirvShaderTranslator translator(features, true, true, false);
  rex::string::StringBuffer disassembly;

  size_t vertex_count = 0;
  size_t pixel_count = 0;
  size_t translated_count = 0;
  size_t glsl_count = 0;
  size_t uam_count = 0;
  size_t warning_count = 0;
  size_t failure_count = 0;
  bool wrote_system_constants_example = false;
  UamLifetime uam_lifetime;

  for (size_t i = 0; i < cache.shaders.size(); ++i) {
    const CacheShader& cached = cache.shaders[i];
    const bool is_vertex = cached.type == rex::graphics::xenos::ShaderType::kVertex;
    const auto stage = is_vertex ? fear_nx::deko::ShaderStage::kVertex
                                 : fear_nx::deko::ShaderStage::kFragment;
    if (is_vertex) {
      ++vertex_count;
    } else {
      ++pixel_count;
    }

    rex::graphics::SpirvShader shader(cached.type, cached.hash, cached.guest_words.data(),
                                      cached.word_count, std::endian::big);
    disassembly.Reset();
    shader.AnalyzeUcode(disassembly);
    const uint32_t dynamic_register_count = shader.GetDynamicAddressableRegisterCount(127);
    const uint64_t modification = is_vertex
        ? translator.GetDefaultVertexShaderModification(
              dynamic_register_count, rex::graphics::Shader::HostVertexShaderType::kVertex)
        : translator.GetDefaultPixelShaderModification(dynamic_register_count);
    auto* translation = shader.GetOrCreateTranslation(modification);
    const bool translator_ok = translator.TranslateAnalyzedShader(*translation) &&
                               translation->is_valid() &&
                               translation->translated_binary().size() >= 20 &&
                               translation->translated_binary().size() % sizeof(uint32_t) == 0;
    std::string detail = translator_ok ? std::string() : TranslationErrors(*translation);
    if (!translator_ok && detail.empty()) detail = "translator returned invalid/empty SPIR-V";
    if (translator_ok) ++translated_count;

    fear_nx::deko::GlslShader glsl;
    bool glsl_ok = false;
    bool uam_ok = false;
    std::string uam_log;
    if (translator_ok) {
      const size_t word_count = translation->translated_binary().size() / sizeof(uint32_t);
      std::vector<uint32_t> spirv(word_count);
      std::memcpy(spirv.data(), translation->translated_binary().data(),
                  translation->translated_binary().size());
      glsl = fear_nx::deko::ConvertSpirv(std::span<const uint32_t>(spirv), stage);
      glsl_ok = static_cast<bool>(glsl);
      if (glsl_ok) {
        ++glsl_count;
        std::string metadata_error;
        if (!ValidateSystemConstantMetadata(glsl, metadata_error)) {
          glsl_ok = false;
          glsl.error = metadata_error;
          if (!detail.empty()) detail += " | ";
          detail += metadata_error;
        }
      }
      if (glsl_ok) {
        warning_count += glsl.warnings.size();
        if (!glsl.warnings.empty()) {
          if (!detail.empty()) detail += " | ";
          detail += JoinWarnings(glsl);
        }
        uam::Result compiled = uam::compile(UamStage(stage), glsl.source.c_str());
        uam_ok = compiled.ok && !compiled.dksh.empty();
        uam_log = std::move(compiled.log);
        if (uam_ok) {
          ++uam_count;
        } else {
          if (!detail.empty()) detail += " | ";
          detail += uam_log.empty() ? "UAM returned no DKSH" : uam_log;
        }
      } else {
        if (!detail.empty()) detail += " | ";
        detail += glsl.error;
      }
    }

    const bool compatible = translator_ok && glsl_ok && uam_ok && glsl.warnings.empty();
    if (!compatible) {
      ++failure_count;
      const std::string suffix = (is_vertex ? "-vs" : "-ps");
      const std::string file_stem = Hex(cached.hash) + suffix;
      if (!glsl.source.empty()) {
        std::ofstream source(output_dir / (file_stem + ".glsl"),
                             std::ios::binary | std::ios::trunc);
        source << glsl.source;
      }
      if (translator_ok && failure_count == 1) {
        std::ofstream binary(output_dir / (file_stem + ".spv"),
                             std::ios::binary | std::ios::trunc);
        binary.write(reinterpret_cast<const char*>(translation->translated_binary().data()),
                     static_cast<std::streamsize>(translation->translated_binary().size()));
      }
      std::ofstream failure(output_dir / (file_stem + ".log"),
                            std::ios::binary | std::ios::trunc);
      failure << "shader=" << Hex(cached.hash) << " stage=" << (is_vertex ? "vertex" : "fragment")
              << " ucode_words=" << cached.word_count << "\n"
              << "translator_ok=" << translator_ok << " glsl_ok=" << glsl_ok
              << " uam_ok=" << uam_ok << " warnings=" << glsl.warnings.size() << "\n"
              << detail << "\n";
    }

    report << Hex(cached.hash) << ',' << (is_vertex ? "vertex" : "fragment") << ','
           << cached.word_count << ',' << translator_ok << ','
           << (translator_ok ? translation->translated_binary().size() / sizeof(uint32_t) : 0)
           << ',' << glsl_ok << ',' << glsl.textures.size() << ',' << glsl.uniform_buffers.size()
           << ',' << glsl.storage_buffers.size() << ',' << glsl.storage_images.size() << ','
           << glsl.system_constant_size << ',' << glsl.system_constant_copy_ranges.size() << ','
           << uam_ok << ',' << glsl.warnings.size() << ',' << Csv(detail) << '\n';

    if (glsl_ok && glsl.system_constant_size && !wrote_system_constants_example) {
      std::ofstream example(output_dir / "system-constants-example.glsl",
                            std::ios::binary | std::ios::trunc);
      example << glsl.source;
      wrote_system_constants_example = true;
    }

    if ((i + 1) % 10 == 0 || i + 1 == cache.shaders.size()) {
      std::cout << "checked " << (i + 1) << '/' << cache.shaders.size() << " shaders\r"
                << std::flush;
    }
  }

  const fs::path source_root(FEAR_NX_SOURCE_DIR);
  const fs::path resolve_file = source_root / "src/deko/deko_resolve.inc";
  const fs::path renderer_file = source_root / "src/deko/deko_renderer.cpp";
  std::vector<SyntheticResult> synthetic_results;
  for (const auto& [name, marker, closing, file, stage] : {
           std::tuple<std::string_view, std::string_view, std::string_view, fs::path,
                      fear_nx::deko::ShaderStage>{
               "fullscreen-vertex", "const std::string vs=R\"(",
               ")\";", renderer_file, fear_nx::deko::ShaderStage::kVertex},
           std::tuple<std::string_view, std::string_view, std::string_view, fs::path,
                      fear_nx::deko::ShaderStage>{
               "resolve-depth-24", "constexpr char kDepthResolveGlsl[] = R\"glsl(",
               ")glsl\";", resolve_file, fear_nx::deko::ShaderStage::kCompute},
           {"resolve-depth-float", "constexpr char kDepthFloatResolveGlsl[] = R\"glsl(",
            ")glsl\";", resolve_file, fear_nx::deko::ShaderStage::kCompute},
           {"gamma-fragment", "const std::string fs=R\"(", ")\";", renderer_file,
            fear_nx::deko::ShaderStage::kFragment}}) {
    std::string source;
    std::string error;
    if (ExtractRawString(file, marker, closing, source, error)) {
      synthetic_results.push_back(CompileInternalGlsl(std::string(name), std::move(source),
                                                       stage, output_dir));
    } else {
      synthetic_results.push_back({std::string(name), false, std::move(error)});
    }
  }

  for (const auto [type, name] : {
           std::pair{fear_nx::native::PipelineGeometryShader::kPointList,
                     std::string_view("geometry-point")},
           {fear_nx::native::PipelineGeometryShader::kRectangleList,
            std::string_view("geometry-rectangle")},
           {fear_nx::native::PipelineGeometryShader::kQuadList,
            std::string_view("geometry-quad")}}) {
    fear_nx::native::GeometryShaderKey key{};
    key.type = type;
    key.interpolator_count = 16;
    key.user_clip_plane_count = 6;
    key.user_clip_plane_cull = false;
    key.has_vertex_kill_and = true;
    key.has_point_size = true;
    key.has_point_coordinates = true;
    key.point_ps_ucp_mode = 3;
    synthetic_results.push_back(CompileInternalGeometry(std::string(name), key, output_dir));
  }

  size_t synthetic_failure_count = 0;
  for (const auto& result : synthetic_results) {
    if (!result.passed) {
      ++synthetic_failure_count;
      std::cerr << "internal shader failed: " << result.name << ": " << result.detail << '\n';
    }
  }

  report.close();
  std::ofstream summary(output_dir / "summary.txt", std::ios::binary | std::ios::trunc);
  summary << "cache=" << cache_path.string() << '\n'
          << "xesh_version=0x" << std::hex << std::uppercase << cache.version << std::dec << '\n'
          << "records=" << cache.shaders.size() << '\n'
          << "vertex_records=" << vertex_count << '\n'
          << "pixel_records=" << pixel_count << '\n'
          << "translator_pass=" << translated_count << '\n'
          << "spirv_cross_glsl_pass=" << glsl_count << '\n'
          << "uam_dksh_pass=" << uam_count << '\n'
          << "glsl_warnings=" << warning_count << '\n'
          << "compatibility_failures=" << failure_count << '\n'
          << "internal_shader_cases=" << synthetic_results.size() << '\n'
          << "internal_shader_pass=" << (synthetic_results.size() - synthetic_failure_count)
          << '\n'
          << "internal_shader_failures=" << synthetic_failure_count << '\n';
  for (const auto& result : synthetic_results) {
    summary << "internal." << result.name << '=' << (result.passed ? "pass" : "fail");
    if (!result.detail.empty()) summary << " (" << result.detail << ')';
    summary << '\n';
  }

  std::cout << "\nrecords=" << cache.shaders.size() << " vertex=" << vertex_count
            << " pixel=" << pixel_count << " SPIR-V=" << translated_count
            << " GLSL=" << glsl_count << " DKSH=" << uam_count
            << " warnings=" << warning_count << " failures=" << failure_count
            << " internal=" << (synthetic_results.size() - synthetic_failure_count) << '/'
            << synthetic_results.size() << '\n'
            << "report: " << (output_dir / "summary.txt") << '\n';
  return failure_count == 0 && synthetic_failure_count == 0 ? 0 : 1;
}
