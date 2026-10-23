#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <uam_api.h>
#include <xxhash.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/string/buffer.h>
#include <rex/ui/deko3d/cache_dksh.h>

#include "../../src/deko/deko_shader_cache.h"
#include "../../src/deko/deko_shader_warmup.h"
#include "../../src/deko/deko_shaders.h"

REXCVAR_DECLARE(bool, gpu_implicit_lod_sampling);

namespace fs = std::filesystem;
namespace {

constexpr uint32_t kXeshMagic = 0x48534558;  // "XESH" on disk.
constexpr uint32_t kXeshVersion = 0x20201219;
constexpr uint32_t kMaxReasonableShaderWords = 1u << 20;
constexpr size_t kMaxWarmupRecords = 100000;
constexpr uintmax_t kMaxXeshFileBytes = 256u << 20;
constexpr uint64_t kMaxDkshBytes = 4u << 20;

struct InputShader {
  uint32_t shader_type = 0;
  uint64_t shader_hash = 0;
  uint64_t modification = 0;
  std::vector<uint32_t> ucode;
  std::endian source_endian = std::endian::native;
  bool choose_default_modification = false;
  std::string source;
};

struct Counts {
  size_t records = 0;
  size_t translated = 0;
  size_t converted = 0;
  size_t cache_hits = 0;
  size_t compiled = 0;
  size_t failures = 0;
  size_t internal_compiled = 0;
  size_t internal_cache_hits = 0;
  size_t internal_failures = 0;
};

struct LegacyCacheHeader {
  uint64_t key;
  uint64_t bytes;
  uint64_t checksum;
};
static_assert(sizeof(LegacyCacheHeader) == 24);
static_assert(std::endian::native == std::endian::little,
              "runtime .fcache files use the Switch little-endian header format");

class UamLifetime {
 public:
  UamLifetime() { uam::init(true); }
  ~UamLifetime() { Shutdown(); }
  void Shutdown() {
    if (active_) {
      uam::shutdown();
      active_ = false;
    }
  }
  UamLifetime(const UamLifetime&) = delete;
  UamLifetime& operator=(const UamLifetime&) = delete;

 private:
  bool active_ = true;
};

struct CacheShader {
  uint64_t hash = 0;
  uint32_t word_count = 0;
  uint32_t type = 0;
  std::vector<uint32_t> guest_words;
};

bool ReadExact(std::ifstream& input, void* destination, size_t bytes) {
  input.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
  return input.good();
}

bool LoadXesh(const fs::path& path, std::vector<CacheShader>& shaders, std::string& error) {
  std::error_code size_error;
  const uintmax_t file_size = fs::file_size(path, size_error);
  if (size_error || file_size > kMaxXeshFileBytes) {
    error = size_error ? "cannot stat XESH cache: " + size_error.message()
                       : "XESH cache exceeds the 256 MiB input limit";
    return false;
  }
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

    CacheShader shader;
    shader.hash = hash;
    shader.word_count = packed_count_and_type & 0x7fffffffu;
    shader.type = (packed_count_and_type >> 31) != 0
                      ? fear_nx::deko::shader_warmup::kPixelShader
                      : fear_nx::deko::shader_warmup::kVertexShader;
    if (shader.word_count == 0 || shader.word_count > kMaxReasonableShaderWords) {
      error = "invalid microcode word count " + std::to_string(shader.word_count);
      return false;
    }
    shader.guest_words.resize(shader.word_count);
    const size_t bytes = size_t(shader.word_count) * sizeof(uint32_t);
    if (!ReadExact(input, shader.guest_words.data(), bytes)) {
      error = "truncated microcode body for shader " + std::to_string(hash);
      return false;
    }
    if (XXH3_64bits(shader.guest_words.data(), bytes) != hash) {
      error = "microcode hash mismatch for shader " + std::to_string(hash);
      return false;
    }
    shaders.push_back(std::move(shader));
    if (shaders.size() > kMaxWarmupRecords) {
      error = "XESH cache exceeds the record limit";
      return false;
    }
  }

  if (shaders.empty()) {
    error = "XESH cache contains no shaders";
    return false;
  }
  return true;
}

std::string Hex(uint64_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
  return stream.str();
}

std::string Csv(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char ch : value) {
    if (ch == '"') escaped.push_back('"');
    if (ch == '\r') continue;
    if (ch == '\n') escaped += "\\n";
    else escaped.push_back(ch);
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
    if (!range.size ||
        range.source_offset > sizeof(rex::graphics::SpirvShaderTranslator::SystemConstants) ||
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
    if (ranges[index].destination_offset <
        ranges[index - 1].destination_offset + ranges[index - 1].size) {
      error = "system-constant destination copy ranges overlap";
      return false;
    }
  }
  return true;
}

bool SameBinding(const fear_nx::deko::DescriptorLocation& a,
                 const fear_nx::deko::DescriptorLocation& b) {
  return a.set == b.set && a.binding == b.binding;
}

bool SameGlslMetadata(const fear_nx::deko::GlslShader& a,
                      const fear_nx::deko::GlslShader& b) {
  if (a.stage != b.stage || a.source != b.source ||
      a.system_constant_size != b.system_constant_size ||
      a.warnings != b.warnings ||
      a.system_constant_copy_ranges.size() != b.system_constant_copy_ranges.size() ||
      a.textures.size() != b.textures.size() ||
      a.uniform_buffers.size() != b.uniform_buffers.size() ||
      a.storage_buffers.size() != b.storage_buffers.size() ||
      a.storage_images.size() != b.storage_images.size()) {
    return false;
  }
  for (size_t i = 0; i < a.system_constant_copy_ranges.size(); ++i) {
    const auto& x = a.system_constant_copy_ranges[i];
    const auto& y = b.system_constant_copy_ranges[i];
    if (x.source_offset != y.source_offset || x.destination_offset != y.destination_offset ||
        x.size != y.size) return false;
  }
  for (size_t i = 0; i < a.textures.size(); ++i) {
    const auto& x = a.textures[i];
    const auto& y = b.textures[i];
    if (!SameBinding(x.image, y.image) || !SameBinding(x.sampler, y.sampler) ||
        x.slot != y.slot || x.count != y.count || x.dimension != y.dimension ||
        x.arrayed != y.arrayed || x.multisampled != y.multisampled ||
        x.sampler_is_combined != y.sampler_is_combined) return false;
  }
  for (size_t i = 0; i < a.uniform_buffers.size(); ++i) {
    const auto& x = a.uniform_buffers[i];
    const auto& y = b.uniform_buffers[i];
    if (!SameBinding(x.source, y.source) || x.slot != y.slot || x.count != y.count) return false;
  }
  for (size_t i = 0; i < a.storage_buffers.size(); ++i) {
    const auto& x = a.storage_buffers[i];
    const auto& y = b.storage_buffers[i];
    if (!SameBinding(x.source, y.source) || x.slot != y.slot || x.count != y.count) return false;
  }
  for (size_t i = 0; i < a.storage_images.size(); ++i) {
    const auto& x = a.storage_images[i];
    const auto& y = b.storage_images[i];
    if (!SameBinding(x.source, y.source) || x.slot != y.slot || x.count != y.count ||
        x.dimension != y.dimension || x.arrayed != y.arrayed ||
        x.multisampled != y.multisampled) return false;
  }
  return true;
}

struct ShaderResult {
  bool success = false;
  bool cache_hit = false;
  bool translated = false;
  bool converted = false;
  uint64_t key = 0;
  uint64_t modification = 0;
  std::string detail;
};

ShaderResult Precompile(InputShader input, const fs::path& cache_dir,
                        std::string_view compiler_id,
                        rex::graphics::SpirvShaderTranslator& translator) {
  ShaderResult result;
  using fear_nx::deko::ShaderStage;
  using rex::graphics::SpirvShader;
  using rex::graphics::xenos::ShaderType;

  if (input.shader_type != fear_nx::deko::shader_warmup::kVertexShader &&
      input.shader_type != fear_nx::deko::shader_warmup::kPixelShader) {
    result.detail = "unsupported Xenos shader type " + std::to_string(input.shader_type);
    return result;
  }
  if (input.ucode.empty() || input.ucode.size() > kMaxReasonableShaderWords) {
    result.detail = "invalid or oversized Xenos microcode";
    return result;
  }
  const bool vertex = input.shader_type == fear_nx::deko::shader_warmup::kVertexShader;
  const ShaderType shader_type = vertex ? ShaderType::kVertex : ShaderType::kPixel;
  const ShaderStage stage = vertex ? ShaderStage::kVertex : ShaderStage::kFragment;

  SpirvShader shader(shader_type, input.shader_hash, input.ucode.data(), input.ucode.size(),
                     input.source_endian);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  uint64_t modification = input.modification;
  if (input.choose_default_modification) {
    const uint32_t dynamic_register_count = shader.GetDynamicAddressableRegisterCount(127);
    modification = vertex
        ? translator.GetDefaultVertexShaderModification(
              dynamic_register_count, rex::graphics::Shader::HostVertexShaderType::kVertex)
        : translator.GetDefaultPixelShaderModification(dynamic_register_count);
  }
  result.modification = modification;

  auto* translation = static_cast<SpirvShader::SpirvTranslation*>(
      shader.GetOrCreateTranslation(modification));
  if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
    result.detail = TranslationErrors(*translation);
    if (result.detail.empty()) result.detail = "SPIR-V translator returned an invalid result";
    return result;
  }
  result.translated = true;
  const auto& translated_binary = translation->translated_binary();
  if (translated_binary.size() < 20 || translated_binary.size() % sizeof(uint32_t) != 0) {
    result.detail = "translator returned malformed SPIR-V bytecode";
    return result;
  }
  std::vector<uint32_t> spirv(translated_binary.size() / sizeof(uint32_t));
  std::memcpy(spirv.data(), translated_binary.data(), translated_binary.size());
  fear_nx::deko::GlslShader glsl = fear_nx::deko::ConvertSpirv(
      std::span<const uint32_t>(spirv), stage);
  if (!glsl) {
    result.detail = "SPIRV-Cross: " + glsl.error;
    return result;
  }
  if (!glsl.warnings.empty()) {
    result.detail = "converter warnings must be resolved before packaging: ";
    for (const auto& warning : glsl.warnings) {
      if (result.detail.back() != ' ') result.detail += " | ";
      result.detail += warning;
    }
    return result;
  }
  if (!ValidateSystemConstantMetadata(glsl, result.detail)) return result;
  result.converted = true;

  result.key = fear_nx::deko::shader_cache::SpirvKey(
      std::span<const uint32_t>(spirv), stage, compiler_id);
  const fs::path cache_path = cache_dir / (Hex(result.key) + ".fshader");
  fear_nx::deko::shader_cache::Entry cached;
  std::string load_error;
  if (fear_nx::deko::shader_cache::Load(cache_path, result.key, stage, compiler_id,
                                        cached, load_error)) {
    if (cached.dksh.empty() || !SameGlslMetadata(glsl, cached.info)) {
      result.detail = "stage-cache hit has empty DKSH or mismatched GLSL metadata";
      return result;
    }
    result.success = true;
    result.cache_hit = true;
    return result;
  }

  uam::Result compiled = uam::compile(UamStage(stage), glsl.source.c_str());
  if (!compiled.ok || compiled.dksh.empty()) {
    result.detail = compiled.log.empty() ? "UAM returned no DKSH" : std::move(compiled.log);
    return result;
  }
  fear_nx::deko::shader_cache::Entry entry;
  entry.info = std::move(glsl);
  entry.dksh = std::move(compiled.dksh);
  std::string save_error;
  if (!fear_nx::deko::shader_cache::Save(cache_path, result.key, stage, compiler_id,
                                         entry, save_error)) {
    result.detail = "cannot save stage cache: " + save_error;
    return result;
  }
  result.success = true;
  return result;
}

bool ParseBool(std::string_view value, bool& parsed) {
  if (value == "true" || value == "1") {
    parsed = true;
    return true;
  }
  if (value == "false" || value == "0") {
    parsed = false;
    return true;
  }
  return false;
}

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

bool IsValidLegacyCache(const fs::path& path, uint64_t key, std::string& error) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    error = "cache miss";
    return false;
  }
  const auto length = input.tellg();
  input.seekg(0);
  LegacyCacheHeader header{};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!input || header.key != key || header.bytes < sizeof(header) ||
      header.bytes > kMaxDkshBytes ||
      length != std::streamoff(sizeof(header) + header.bytes)) {
    error = "legacy cache header or length is invalid";
    return false;
  }
  std::vector<uint8_t> dksh(size_t(header.bytes));
  input.read(reinterpret_cast<char*>(dksh.data()),
             static_cast<std::streamsize>(dksh.size()));
  if (!input || XXH3_64bits(dksh.data(), dksh.size()) != header.checksum) {
    error = "legacy cache checksum is invalid";
    return false;
  }
  return true;
}

bool WriteLegacyCache(const fs::path& path, uint64_t key,
                      std::span<const uint8_t> dksh, std::string& error) {
  if (dksh.size() < sizeof(LegacyCacheHeader) || dksh.size() > kMaxDkshBytes) {
    error = "UAM DKSH size is outside runtime cache limits";
    return false;
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "cannot create legacy cache directory: " + ec.message();
    return false;
  }

  const LegacyCacheHeader header{key, dksh.size(), XXH3_64bits(dksh.data(), dksh.size())};
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "cannot create temporary legacy cache file";
      return false;
    }
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(dksh.data()),
                 static_cast<std::streamsize>(dksh.size()));
    output.flush();
    if (!output) {
      error = "cannot write complete legacy cache file";
      output.close();
      fs::remove(temporary, ec);
      return false;
    }
  }

#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "cannot atomically install legacy cache file: " +
            std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
#else
  fs::rename(temporary, path, ec);
  if (ec) {
    error = "cannot atomically install legacy cache file: " + ec.message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
#endif
  return true;
}

struct InternalResult {
  bool success = false;
  bool cache_hit = false;
  uint64_t key = 0;
  std::string detail;
};

InternalResult PrecompileInternal(std::string source,
                                  fear_nx::deko::ShaderStage stage,
                                  const fs::path& legacy_cache_dir,
                                  std::string_view compiler_id) {
  InternalResult result;
  if (source.empty()) {
    result.detail = "empty GLSL source";
    return result;
  }
  const uint64_t seed = XXH3_64bits(compiler_id.data(), compiler_id.size()) ^
                        uint64_t(stage);
  result.key = XXH3_64bits_withSeed(source.data(), source.size(), seed);
  const fs::path cache_path = legacy_cache_dir /
                              (std::to_string(result.key) + ".fcache");
  std::string cache_error;
  if (IsValidLegacyCache(cache_path, result.key, cache_error)) {
    result.success = true;
    result.cache_hit = true;
    result.detail = "existing runtime-compatible DKSH cache validated";
    return result;
  }

  uam::Result compiled = uam::compile(UamStage(stage), source.c_str());
  if (!compiled.ok || compiled.dksh.empty()) {
    result.detail = compiled.log.empty() ? "UAM returned no DKSH" : std::move(compiled.log);
    return result;
  }
  if (!WriteLegacyCache(cache_path, result.key, compiled.dksh, result.detail)) return result;
  result.success = true;
  result.detail = "compiled and saved runtime-compatible DKSH cache";
  return result;
}

void PrintUsage() {
  std::cerr << "Usage: deko_shader_precompile --warmup-dir DIR --cache-dir DIR "
               "[--implicit-lod true|false] [--xsh FILE] "
               "[--internal-shaders --legacy-cache-dir DIR]\n"
               "Reads observed .fwarm variants and optionally the XESH shader cache, "
               "then writes reusable stage-cache files. --internal-shaders also builds "
               "the fixed resolve and presentation GLSL into runtime-compatible .fcache files.\n";
}

}  // namespace

int main(int argc, char** argv) {
  fs::path warmup_dir;
  fs::path cache_dir;
  fs::path legacy_cache_dir;
  fs::path xsh_path;
  bool implicit_lod = true;
  bool internal_shaders = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    }
    if (arg == "--internal-shaders") {
      internal_shaders = true;
      continue;
    }
    if (i + 1 >= argc) {
      PrintUsage();
      return 2;
    }
    const std::string_view value(argv[++i]);
    if (arg == "--warmup-dir") warmup_dir = value;
    else if (arg == "--cache-dir") cache_dir = value;
    else if (arg == "--legacy-cache-dir") legacy_cache_dir = value;
    else if (arg == "--xsh") xsh_path = value;
    else if (arg == "--implicit-lod") {
      if (!ParseBool(value, implicit_lod)) {
        std::cerr << "--implicit-lod expects true or false\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown option: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }
  if (cache_dir.empty() || (warmup_dir.empty() && xsh_path.empty() && !internal_shaders) ||
      (internal_shaders && legacy_cache_dir.empty())) {
    PrintUsage();
    return 2;
  }

  std::error_code ec;
  if (!warmup_dir.empty() && (!fs::is_directory(warmup_dir, ec) || ec)) {
    std::cerr << "Warmup directory does not exist: " << warmup_dir << '\n';
    return 2;
  }
  fs::create_directories(cache_dir, ec);
  if (ec) {
    std::cerr << "Cannot create cache directory: " << ec.message() << '\n';
    return 2;
  }

  std::vector<fs::path> warmup_files;
  if (!warmup_dir.empty()) {
    fs::recursive_directory_iterator it(warmup_dir, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
      std::cerr << "Cannot enumerate warmup directory: " << ec.message() << '\n';
      return 2;
    }
    for (; it != end; it.increment(ec)) {
      if (ec) {
        std::cerr << "Cannot enumerate warmup directory: " << ec.message() << '\n';
        return 2;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      std::string extension = it->path().extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char ch) { return char(std::tolower(ch)); });
      if (extension == ".fwarm") warmup_files.push_back(it->path());
      if (warmup_files.size() > kMaxWarmupRecords) {
        std::cerr << "Warmup directory exceeds the record limit\n";
        return 2;
      }
    }
  }
  std::sort(warmup_files.begin(), warmup_files.end());

  std::vector<CacheShader> xsh_shaders;
  if (!xsh_path.empty()) {
    std::string xsh_error;
    if (!LoadXesh(xsh_path, xsh_shaders, xsh_error)) {
      std::cerr << "Cannot load XESH cache: " << xsh_error << '\n';
      return 2;
    }
  }
  if (warmup_files.empty() && xsh_shaders.empty() && !internal_shaders) {
    std::cerr << "No .fwarm records found; provide --xsh or --internal-shaders\n";
    return 2;
  }

  REXCVAR_SET(gpu_implicit_lod_sampling, implicit_lod);
  rex::graphics::SpirvShaderTranslator::Features features(false);
  features.spirv_version = 0x00010000;
  features.max_storage_buffer_range = 512u << 20;
  features.full_draw_index_uint32 = true;
  features.clip_distance = true;
  features.cull_distance = true;
  features.image_view_format_swizzle = true;
  rex::graphics::SpirvShaderTranslator translator(features, true, true, false);
  const std::string compiler_id = rex::deko3d::IdCompilador();

  std::ofstream report(cache_dir / "precompile-results.csv", std::ios::binary | std::ios::trunc);
  if (!report) {
    std::cerr << "Cannot create precompile-results.csv in " << cache_dir << '\n';
    return 2;
  }
  report << "source,stage,shader_hash,modification,stage_key,result,detail\n";

  UamLifetime uam_lifetime;
  Counts counts;
  auto record = [&](InputShader input) {
    ++counts.records;
    const std::string source = input.source;
    const std::string stage = input.shader_type == fear_nx::deko::shader_warmup::kVertexShader
                                  ? "vertex" : "fragment";
    const uint64_t shader_hash = input.shader_hash;
    ShaderResult result;
    try {
      result = Precompile(std::move(input), cache_dir, compiler_id, translator);
    } catch (const std::exception& e) {
      result.detail = std::string("exception: ") + e.what();
    } catch (...) {
      result.detail = "unknown exception while compiling shader";
    }
    if (result.translated) ++counts.translated;
    if (result.converted) ++counts.converted;
    if (result.success && result.cache_hit) ++counts.cache_hits;
    else if (result.success) ++counts.compiled;
    else {
      ++counts.failures;
    }
    report << Csv(source) << ',' << stage << ',' << Hex(shader_hash) << ','
           << Hex(result.modification) << ',' << Hex(result.key) << ','
           << (result.success ? (result.cache_hit ? "cache-hit" : "compiled") : "failed")
           << ',' << Csv(result.detail) << '\n';
    if (!result.success) std::cerr << source << ": " << result.detail << '\n';
  };

  for (const fs::path& file : warmup_files) {
    fear_nx::deko::shader_warmup::Entry entry;
    std::string error;
    if (!fear_nx::deko::shader_warmup::Load(file, entry, error)) {
      ++counts.records;
      ++counts.failures;
      report << Csv(file.string()) << ",unknown,,,,failed," << Csv(error) << '\n';
      std::cerr << file << ": invalid warmup entry: " << error << '\n';
      continue;
    }
    InputShader input;
    input.shader_type = entry.shader_type;
    input.shader_hash = entry.shader_hash;
    input.modification = entry.modification;
    input.ucode = std::move(entry.ucode);
    input.source_endian = std::endian::native;
    input.source = file.string();
    record(std::move(input));
  }

  for (CacheShader& shader : xsh_shaders) {
    InputShader input;
    input.shader_type = shader.type;
    input.shader_hash = shader.hash;
    input.ucode = std::move(shader.guest_words);
    input.source_endian = std::endian::big;
    input.choose_default_modification = true;
    input.source = xsh_path.string() + "#" + Hex(shader.hash);
    record(std::move(input));
  }

  if (internal_shaders) {
    const fs::path source_root(FEAR_NX_SOURCE_DIR);
    const fs::path resolve_file = source_root / "src/deko/deko_resolve.inc";
    const fs::path renderer_file = source_root / "src/deko/deko_renderer.cpp";
    struct InternalSource {
      std::string_view name;
      std::string_view marker;
      std::string_view closing;
      fs::path file;
      fear_nx::deko::ShaderStage stage;
    };
    const std::vector<InternalSource> internal_sources{
        {"resolve-depth-24", "constexpr char kDepthResolveGlsl[] = R\"glsl(",
         ")glsl\";", resolve_file, fear_nx::deko::ShaderStage::kCompute},
        {"resolve-depth-float", "constexpr char kDepthFloatResolveGlsl[] = R\"glsl(",
         ")glsl\";", resolve_file, fear_nx::deko::ShaderStage::kCompute},
        {"fullscreen-vertex", "const std::string vs=R\"(", ")\";", renderer_file,
         fear_nx::deko::ShaderStage::kVertex},
        {"gamma-fragment", "const std::string fs=R\"(", ")\";", renderer_file,
         fear_nx::deko::ShaderStage::kFragment},
    };
    for (const auto& internal : internal_sources) {
      std::string glsl;
      std::string detail;
      InternalResult result;
      if (ExtractRawString(internal.file, internal.marker, internal.closing, glsl, detail)) {
        try {
          result = PrecompileInternal(std::move(glsl), internal.stage,
                                      legacy_cache_dir, compiler_id);
        } catch (const std::exception& e) {
          result.detail = std::string("exception: ") + e.what();
        } catch (...) {
          result.detail = "unknown exception while compiling internal shader";
        }
      } else {
        result.detail = std::move(detail);
      }
      if (result.success && result.cache_hit) ++counts.internal_cache_hits;
      else if (result.success) ++counts.internal_compiled;
      else ++counts.internal_failures;
      const char* stage_name = internal.stage == fear_nx::deko::ShaderStage::kCompute
                                   ? "compute"
                               : internal.stage == fear_nx::deko::ShaderStage::kVertex
                                   ? "vertex" : "fragment";
      report << Csv(std::string("internal:") + std::string(internal.name)) << ','
             << stage_name << ",,," << Hex(result.key) << ','
             << (result.success ? (result.cache_hit ? "cache-hit" : "compiled") : "failed")
             << ',' << Csv(result.detail) << '\n';
      if (!result.success) {
        std::cerr << "internal shader " << internal.name << ": " << result.detail << '\n';
      }
    }
  }
  report.close();
  // Destroy the UAM compiler before writing the final summary.
  uam_lifetime.Shutdown();

  std::ofstream summary(cache_dir / "precompile-summary.txt", std::ios::binary | std::ios::trunc);
  if (!summary) {
    std::cerr << "Cannot create precompile-summary.txt in " << cache_dir << '\n';
    return 2;
  }
  summary << "warmup_dir=" << warmup_dir.string() << '\n'
          << "xsh=" << (xsh_path.empty() ? "" : xsh_path.string()) << '\n'
          << "cache_dir=" << cache_dir.string() << '\n'
          << "implicit_lod=" << (implicit_lod ? "true" : "false") << '\n'
          << "compiler_id=" << compiler_id << '\n'
          << "records=" << counts.records << '\n'
          << "translated=" << counts.translated << '\n'
          << "converted=" << counts.converted << '\n'
          << "cache_hits=" << counts.cache_hits << '\n'
          << "compiled=" << counts.compiled << '\n'
          << "failures=" << counts.failures << '\n'
          << "internal_shaders=" << (internal_shaders ? 4 : 0) << '\n'
          << "legacy_cache_dir=" << (internal_shaders ? legacy_cache_dir.string() : "") << '\n'
          << "internal_compiled=" << counts.internal_compiled << '\n'
          << "internal_cache_hits=" << counts.internal_cache_hits << '\n'
          << "internal_failures=" << counts.internal_failures << '\n';

  std::cout << "records=" << counts.records << " cache_hits=" << counts.cache_hits
            << " compiled=" << counts.compiled << " failures=" << counts.failures
            << " internal_compiled=" << counts.internal_compiled
            << " internal_cache_hits=" << counts.internal_cache_hits
            << " internal_failures=" << counts.internal_failures << '\n'
            << "summary: " << (cache_dir / "precompile-summary.txt") << '\n';
  return counts.failures == 0 && counts.internal_failures == 0 ? 0 : 1;
}
