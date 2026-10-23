#include "deko_shader_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#include <xxhash.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fear_nx::deko::shader_cache {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'F', 'E', 'A', 'R', 'F', 'S', 'H', 0};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxFileBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxSourceBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxDkshBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxCompilerIdBytes = 1024;
constexpr size_t kMaxWarningCount = 1024;
constexpr size_t kMaxWarningBytes = 16u * 1024u;
constexpr size_t kMaxTextureBindings = 32;
constexpr size_t kMaxUniformBindings = 16;
constexpr size_t kMaxStorageBindings = 16;
constexpr size_t kMaxStorageImageBindings = 8;
constexpr size_t kMaxSystemConstantRanges = 4096;
constexpr uint32_t kMaxSystemConstantBytes = 64u * 1024u;
constexpr uint32_t kMaxDescriptorSet = 255;
constexpr uint32_t kMaxDescriptorBinding = 65535;
constexpr uint32_t kDkshMagic = 0x48534B44u;
constexpr uint32_t kDkshHeaderBytes = 24;
constexpr uint32_t kDkshProgramHeaderBytes = 64;

bool Fail(std::string& error, std::string message) {
  error = std::move(message);
  return false;
}

bool ValidStage(ShaderStage stage) {
  return stage >= ShaderStage::kVertex && stage <= ShaderStage::kCompute;
}

bool ValidDimension(ImageDimension dimension) {
  return dimension >= ImageDimension::kUnknown && dimension <= ImageDimension::kSubpass;
}

bool ValidLocation(DescriptorLocation location, bool allow_unused) {
  const bool set_unused = location.set == DescriptorLocation::kUnused;
  const bool binding_unused = location.binding == DescriptorLocation::kUnused;
  if (set_unused || binding_unused) return allow_unused && set_unused && binding_unused;
  return location.set <= kMaxDescriptorSet && location.binding <= kMaxDescriptorBinding;
}

template <size_t N>
bool ValidateSlots(uint32_t slot, uint32_t count, std::array<bool, N>& occupied) {
  if (!count || slot >= N || count > N - slot) return false;
  for (uint32_t i = slot; i < slot + count; ++i) {
    if (occupied[i]) return false;
  }
  for (uint32_t i = slot; i < slot + count; ++i) occupied[i] = true;
  return true;
}

bool ValidateEntry(const Entry& entry, ShaderStage stage, std::string& error) {
  const GlslShader& info = entry.info;
  if (!ValidStage(stage) || info.stage != stage) return Fail(error, "shader stage is invalid or does not match the cache identity");
  if (!info.error.empty() || info.source.empty()) return Fail(error, "only successfully converted GLSL can be cached");
  if (info.source.size() > kMaxSourceBytes) return Fail(error, "GLSL source exceeds the cache limit");
  if (entry.dksh.empty() || entry.dksh.size() > kMaxDkshBytes) return Fail(error, "DKSH binary is empty or exceeds the cache limit");
  const auto word = [&entry](size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(entry.dksh[offset + i]) << (8 * i);
    return value;
  };
  if (entry.dksh.size() < kDkshHeaderBytes) return Fail(error, "DKSH header is truncated");
  const uint32_t header_bytes = word(4);
  const uint32_t control_bytes = word(8);
  const uint32_t code_bytes = word(12);
  const uint32_t programs_offset = word(16);
  const uint32_t program_count = word(20);
  if (word(0) != kDkshMagic || header_bytes != kDkshHeaderBytes ||
      program_count != 1 || control_bytes < header_bytes || !code_bytes ||
      (control_bytes & 255u) != 0 || (code_bytes & 255u) != 0 ||
      uint64_t(control_bytes) + code_bytes != entry.dksh.size() ||
      programs_offset < header_bytes ||
      uint64_t(programs_offset) + kDkshProgramHeaderBytes > control_bytes) {
    return Fail(error, "DKSH header sizes, program count, or control/code bounds are invalid");
  }
  const uint32_t program_type = word(programs_offset);
  const uint32_t expected_program_type = stage == ShaderStage::kVertex ? 0u
      : stage == ShaderStage::kFragment ? 1u
      : stage == ShaderStage::kGeometry ? 2u : 5u;
  if (program_type != expected_program_type || word(programs_offset + 4) >= code_bytes) {
    return Fail(error, "DKSH program stage or entry point does not match the cache identity");
  }
  if (info.textures.size() > kMaxTextureBindings ||
      info.uniform_buffers.size() > kMaxUniformBindings ||
      info.storage_buffers.size() > kMaxStorageBindings ||
      info.storage_images.size() > kMaxStorageImageBindings ||
      info.system_constant_copy_ranges.size() > kMaxSystemConstantRanges ||
      info.warnings.size() > kMaxWarningCount) {
    return Fail(error, "shader metadata count exceeds the cache limit");
  }
  for (const std::string& warning : info.warnings) {
    if (warning.size() > kMaxWarningBytes) return Fail(error, "shader warning exceeds the cache limit");
  }

  std::array<bool, kMaxTextureBindings> texture_slots{};
  for (const TextureBinding& binding : info.textures) {
    if (!ValidLocation(binding.image, false) || !ValidLocation(binding.sampler, true) ||
        !ValidateSlots(binding.slot, binding.count, texture_slots) ||
        !ValidDimension(binding.dimension)) {
      return Fail(error, "texture binding has an invalid location, slot range, or dimension");
    }
  }

  std::array<bool, kMaxUniformBindings> uniform_slots{};
  for (const UniformBufferBinding& binding : info.uniform_buffers) {
    if (!ValidLocation(binding.source, false) ||
        !ValidateSlots(binding.slot, binding.count, uniform_slots)) {
      return Fail(error, "uniform-buffer binding has an invalid location or slot range");
    }
  }

  std::array<bool, kMaxStorageBindings> storage_slots{};
  for (const StorageBufferBinding& binding : info.storage_buffers) {
    if (!ValidLocation(binding.source, false) ||
        !ValidateSlots(binding.slot, binding.count, storage_slots)) {
      return Fail(error, "storage-buffer binding has an invalid location or slot range");
    }
  }

  std::array<bool, kMaxStorageImageBindings> image_slots{};
  for (const StorageImageBinding& binding : info.storage_images) {
    if (!ValidLocation(binding.source, false) ||
        !ValidateSlots(binding.slot, binding.count, image_slots) ||
        !ValidDimension(binding.dimension)) {
      return Fail(error, "storage-image binding has an invalid location, slot range, or dimension");
    }
  }

  if (info.system_constant_size > kMaxSystemConstantBytes ||
      (info.system_constant_size & 15u) != 0 ||
      (info.system_constant_size == 0) != info.system_constant_copy_ranges.empty()) {
    return Fail(error, "system-constant size or range metadata is inconsistent");
  }
  std::vector<std::pair<uint32_t, uint32_t>> destination_ranges;
  destination_ranges.reserve(info.system_constant_copy_ranges.size());
  for (const SystemConstantCopyRange& range : info.system_constant_copy_ranges) {
    if (!range.size || range.source_offset > kMaxSystemConstantBytes ||
        range.size > kMaxSystemConstantBytes - range.source_offset ||
        range.destination_offset > info.system_constant_size ||
        range.size > info.system_constant_size - range.destination_offset) {
      return Fail(error, "system-constant copy range exceeds its bounds");
    }
    destination_ranges.emplace_back(range.destination_offset,
                                    range.destination_offset + range.size);
  }
  std::sort(destination_ranges.begin(), destination_ranges.end());
  for (size_t i = 1; i < destination_ranges.size(); ++i) {
    if (destination_ranges[i].first < destination_ranges[i - 1].second) {
      return Fail(error, "system-constant destination copy ranges overlap");
    }
  }
  return true;
}

class Writer {
 public:
  bool U8(uint8_t value) { return Bytes(&value, sizeof(value)); }
  bool U32(uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
    return Bytes(bytes, sizeof(bytes));
  }
  bool U64(uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (i * 8));
    return Bytes(bytes, sizeof(bytes));
  }
  bool Bytes(const void* data, size_t size) {
    if (size > kMaxFileBytes || bytes_.size() > kMaxFileBytes - size) return false;
    if (!size) return true;
    const auto* begin = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), begin, begin + size);
    return true;
  }
  bool String(std::string_view value, size_t limit) {
    return value.size() <= limit && value.size() <= UINT32_MAX &&
           U32(static_cast<uint32_t>(value.size())) && Bytes(value.data(), value.size());
  }
  const std::vector<uint8_t>& data() const { return bytes_; }
  std::vector<uint8_t> Take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  bool U8(uint8_t& value) {
    if (!CanRead(1)) return false;
    value = bytes_[position_++];
    return true;
  }
  bool U32(uint32_t& value) {
    if (!CanRead(4)) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes_[position_++]) << (i * 8);
    return true;
  }
  bool U64(uint64_t& value) {
    if (!CanRead(8)) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes_[position_++]) << (i * 8);
    return true;
  }
  bool Bytes(void* destination, size_t size) {
    if (!CanRead(size)) return false;
    if (size) std::memcpy(destination, bytes_.data() + position_, size);
    position_ += size;
    return true;
  }
  bool String(std::string& value, size_t limit) {
    uint32_t size = 0;
    if (!U32(size) || size > limit || !CanRead(size)) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + position_), size);
    position_ += size;
    return true;
  }
  bool End() const { return position_ == bytes_.size(); }
  size_t Remaining() const { return bytes_.size() - position_; }

 private:
  bool CanRead(size_t size) const { return size <= bytes_.size() - position_; }
  std::span<const uint8_t> bytes_;
  size_t position_ = 0;
};

bool WriteLocation(Writer& writer, DescriptorLocation value) {
  return writer.U32(value.set) && writer.U32(value.binding);
}

bool ReadLocation(Reader& reader, DescriptorLocation& value) {
  return reader.U32(value.set) && reader.U32(value.binding);
}

bool SerializePayload(const Entry& entry, Writer& payload) {
  const GlslShader& info = entry.info;
  if (!payload.String(info.source, kMaxSourceBytes) ||
      !payload.U32(static_cast<uint32_t>(info.textures.size()))) return false;
  for (const TextureBinding& b : info.textures) {
    if (!WriteLocation(payload, b.image) || !WriteLocation(payload, b.sampler) ||
        !payload.U32(b.slot) || !payload.U32(b.count) ||
        !payload.U8(static_cast<uint8_t>(b.dimension)) ||
        !payload.U8(b.arrayed ? 1 : 0) || !payload.U8(b.multisampled ? 1 : 0) ||
        !payload.U8(b.sampler_is_combined ? 1 : 0)) return false;
  }
  if (!payload.U32(static_cast<uint32_t>(info.uniform_buffers.size()))) return false;
  for (const UniformBufferBinding& b : info.uniform_buffers) {
    if (!WriteLocation(payload, b.source) || !payload.U32(b.slot) || !payload.U32(b.count)) return false;
  }
  if (!payload.U32(static_cast<uint32_t>(info.storage_buffers.size()))) return false;
  for (const StorageBufferBinding& b : info.storage_buffers) {
    if (!WriteLocation(payload, b.source) || !payload.U32(b.slot) || !payload.U32(b.count)) return false;
  }
  if (!payload.U32(static_cast<uint32_t>(info.storage_images.size()))) return false;
  for (const StorageImageBinding& b : info.storage_images) {
    if (!WriteLocation(payload, b.source) || !payload.U32(b.slot) || !payload.U32(b.count) ||
        !payload.U8(static_cast<uint8_t>(b.dimension)) || !payload.U8(b.arrayed ? 1 : 0) ||
        !payload.U8(b.multisampled ? 1 : 0)) return false;
  }
  if (!payload.U32(info.system_constant_size) ||
      !payload.U32(static_cast<uint32_t>(info.system_constant_copy_ranges.size()))) return false;
  for (const SystemConstantCopyRange& range : info.system_constant_copy_ranges) {
    if (!payload.U32(range.source_offset) || !payload.U32(range.destination_offset) ||
        !payload.U32(range.size)) return false;
  }
  if (!payload.U32(static_cast<uint32_t>(info.warnings.size()))) return false;
  for (const std::string& warning : info.warnings) {
    if (!payload.String(warning, kMaxWarningBytes)) return false;
  }
  return entry.dksh.size() <= UINT32_MAX &&
         payload.U32(static_cast<uint32_t>(entry.dksh.size())) &&
         payload.Bytes(entry.dksh.data(), entry.dksh.size());
}

bool ReadBool(Reader& reader, bool& value) {
  uint8_t raw = 0;
  if (!reader.U8(raw) || raw > 1) return false;
  value = raw != 0;
  return true;
}

bool DeserializePayload(std::span<const uint8_t> bytes, Entry& entry,
                        std::string& error) {
  Reader reader(bytes);
  GlslShader& info = entry.info;
  if (!reader.String(info.source, kMaxSourceBytes) || info.source.empty())
    return Fail(error, "cache GLSL source is truncated, empty, or too large");

  uint32_t count = 0;
  if (!reader.U32(count) || count > kMaxTextureBindings)
    return Fail(error, "cache texture-binding count is invalid");
  info.textures.resize(count);
  for (TextureBinding& b : info.textures) {
    uint8_t dimension = 0;
    if (!ReadLocation(reader, b.image) || !ReadLocation(reader, b.sampler) ||
        !reader.U32(b.slot) || !reader.U32(b.count) || !reader.U8(dimension) ||
        !ReadBool(reader, b.arrayed) || !ReadBool(reader, b.multisampled) ||
        !ReadBool(reader, b.sampler_is_combined))
      return Fail(error, "cache texture-binding record is malformed");
    b.dimension = static_cast<ImageDimension>(dimension);
  }

  if (!reader.U32(count) || count > kMaxUniformBindings)
    return Fail(error, "cache uniform-buffer count is invalid");
  info.uniform_buffers.resize(count);
  for (UniformBufferBinding& b : info.uniform_buffers) {
    if (!ReadLocation(reader, b.source) || !reader.U32(b.slot) || !reader.U32(b.count))
      return Fail(error, "cache uniform-buffer record is malformed");
  }

  if (!reader.U32(count) || count > kMaxStorageBindings)
    return Fail(error, "cache storage-buffer count is invalid");
  info.storage_buffers.resize(count);
  for (StorageBufferBinding& b : info.storage_buffers) {
    if (!ReadLocation(reader, b.source) || !reader.U32(b.slot) || !reader.U32(b.count))
      return Fail(error, "cache storage-buffer record is malformed");
  }

  if (!reader.U32(count) || count > kMaxStorageImageBindings)
    return Fail(error, "cache storage-image count is invalid");
  info.storage_images.resize(count);
  for (StorageImageBinding& b : info.storage_images) {
    uint8_t dimension = 0;
    if (!ReadLocation(reader, b.source) || !reader.U32(b.slot) || !reader.U32(b.count) ||
        !reader.U8(dimension) || !ReadBool(reader, b.arrayed) ||
        !ReadBool(reader, b.multisampled))
      return Fail(error, "cache storage-image record is malformed");
    b.dimension = static_cast<ImageDimension>(dimension);
  }

  if (!reader.U32(info.system_constant_size) || !reader.U32(count) ||
      count > kMaxSystemConstantRanges)
    return Fail(error, "cache system-constant metadata count is invalid");
  info.system_constant_copy_ranges.resize(count);
  for (SystemConstantCopyRange& range : info.system_constant_copy_ranges) {
    if (!reader.U32(range.source_offset) || !reader.U32(range.destination_offset) ||
        !reader.U32(range.size))
      return Fail(error, "cache system-constant range is truncated");
  }

  if (!reader.U32(count) || count > kMaxWarningCount)
    return Fail(error, "cache warning count is invalid");
  info.warnings.resize(count);
  for (std::string& warning : info.warnings) {
    if (!reader.String(warning, kMaxWarningBytes))
      return Fail(error, "cache warning is truncated or too large");
  }

  uint32_t dksh_size = 0;
  if (!reader.U32(dksh_size) || !dksh_size || dksh_size > kMaxDkshBytes ||
      dksh_size > reader.Remaining())
    return Fail(error, "cache DKSH length is invalid");
  entry.dksh.resize(dksh_size);
  if (!reader.Bytes(entry.dksh.data(), entry.dksh.size()) || !reader.End())
    return Fail(error, "cache payload is truncated or has trailing bytes");
  return true;
}

bool SerializeFile(uint64_t key, ShaderStage stage, std::string_view compiler_id,
                   const Entry& entry, std::vector<uint8_t>& file,
                   std::string& error) {
  if (compiler_id.size() > kMaxCompilerIdBytes || compiler_id.empty())
    return Fail(error, "compiler identity is empty or too large");
  if (!ValidateEntry(entry, stage, error)) return false;

  Writer payload;
  if (!SerializePayload(entry, payload)) return Fail(error, "shader metadata exceeds the cache size limit");

  Writer writer;
  if (!writer.Bytes(kMagic.data(), kMagic.size()) || !writer.U32(kFormatVersion) ||
      !writer.U32(kConverterAbiRevision) || !writer.U8(static_cast<uint8_t>(stage)) ||
      !writer.U8(0) || !writer.U8(0) || !writer.U8(0) || !writer.U64(key) ||
      !writer.String(compiler_id, kMaxCompilerIdBytes) ||
      !writer.U64(payload.data().size()) ||
      !writer.Bytes(payload.data().data(), payload.data().size())) {
    return Fail(error, "shader cache file exceeds the size limit");
  }
  const uint64_t checksum = XXH3_64bits(writer.data().data(), writer.data().size());
  if (!writer.U64(checksum)) return Fail(error, "shader cache file exceeds the size limit");
  file = writer.Take();
  return true;
}

bool AtomicReplace(const std::filesystem::path& temporary,
                   const std::filesystem::path& destination,
                   std::string& error) {
#ifdef _WIN32
  if (MoveFileExW(temporary.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  return Fail(error, "atomic cache replacement failed (Windows error " +
                         std::to_string(GetLastError()) + ")");
#else
  std::error_code ec;
  std::filesystem::rename(temporary, destination, ec);
  if (ec) return Fail(error, "atomic cache replacement failed: " + ec.message());
  return true;
#endif
}

std::filesystem::path TemporaryPath(const std::filesystem::path& destination) {
  static std::atomic<uint64_t> sequence{0};
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path temporary = destination;
  temporary += ".tmp." + std::to_string(static_cast<uint64_t>(ticks)) + "." +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  return temporary;
}

bool ParseFile(std::span<const uint8_t> file, uint64_t expected_key,
               ShaderStage expected_stage, std::string_view expected_compiler,
               Entry& out, std::string& error) {
  if (file.size() < kMagic.size() + 4 + 4 + 4 + 8 + 4 + 8 + 8 ||
      file.size() > kMaxFileBytes) return Fail(error, "shader cache file size is invalid");

  uint64_t stored_checksum = 0;
  Reader trailer(file.last(sizeof(stored_checksum)));
  if (!trailer.U64(stored_checksum) || !trailer.End() ||
      stored_checksum != XXH3_64bits(file.data(), file.size() - sizeof(stored_checksum))) {
    return Fail(error, "shader cache checksum mismatch");
  }

  Reader reader(file.first(file.size() - sizeof(stored_checksum)));
  std::array<uint8_t, kMagic.size()> magic{};
  uint32_t format = 0, abi = 0, compiler_size = 0;
  uint8_t stage_raw = 0, reserved0 = 0, reserved1 = 0, reserved2 = 0;
  uint64_t key = 0, payload_size = 0;
  if (!reader.Bytes(magic.data(), magic.size()) || magic != kMagic ||
      !reader.U32(format) || !reader.U32(abi) || !reader.U8(stage_raw) ||
      !reader.U8(reserved0) || !reader.U8(reserved1) || !reader.U8(reserved2) ||
      !reader.U64(key) || !reader.U32(compiler_size)) {
    return Fail(error, "shader cache header is malformed");
  }
  if (format != kFormatVersion || abi != kConverterAbiRevision)
    return Fail(error, "shader cache format or converter ABI revision is unsupported");
  if (reserved0 || reserved1 || reserved2) return Fail(error, "shader cache reserved header bits are nonzero");
  if (stage_raw > static_cast<uint8_t>(ShaderStage::kCompute))
    return Fail(error, "shader cache stage enum is invalid");
  if (key != expected_key || static_cast<ShaderStage>(stage_raw) != expected_stage)
    return Fail(error, "shader cache key or stage does not match");
  if (compiler_size > kMaxCompilerIdBytes || compiler_size != expected_compiler.size())
    return Fail(error, "shader cache compiler identity length does not match");
  std::string compiler_id(compiler_size, '\0');
  if (!reader.Bytes(compiler_id.data(), compiler_id.size()) || compiler_id != expected_compiler ||
      !reader.U64(payload_size) || payload_size > kMaxFileBytes ||
      payload_size != reader.Remaining()) {
    return Fail(error, "shader cache compiler identity or payload length is invalid");
  }

  Entry candidate;
  candidate.info.stage = expected_stage;
  if (!DeserializePayload(std::span<const uint8_t>(file.data() +
                          (file.size() - sizeof(stored_checksum) - reader.Remaining()),
                          static_cast<size_t>(payload_size)), candidate, error)) return false;
  if (!ValidateEntry(candidate, expected_stage, error)) return false;
  out = std::move(candidate);
  return true;
}

}  // namespace

uint64_t SpirvKey(std::span<const uint32_t> spirv, ShaderStage stage,
                  std::string_view compiler_id) {
  if (!ValidStage(stage) || spirv.empty() ||
      compiler_id.empty() || compiler_id.size() > kMaxCompilerIdBytes ||
      spirv.size() > (kMaxFileBytes / sizeof(uint32_t))) return 0;

  Writer writer;
  if (!writer.U32(kConverterAbiRevision) || !writer.U8(static_cast<uint8_t>(stage)) ||
      !writer.String(compiler_id, kMaxCompilerIdBytes) ||
      !writer.U64(static_cast<uint64_t>(spirv.size()))) return 0;
  for (uint32_t word : spirv) {
    if (!writer.U32(word)) return 0;
  }
  return XXH3_64bits(writer.data().data(), writer.data().size());
}

bool Load(const std::filesystem::path& path, uint64_t expected_key,
          ShaderStage stage, std::string_view compiler_id, Entry& out,
          std::string& error) {
  error.clear();
  if (!ValidStage(stage) || compiler_id.empty() || compiler_id.size() > kMaxCompilerIdBytes)
    return Fail(error, "requested shader cache identity is invalid");
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return Fail(error, "cannot stat shader cache file: " + ec.message());
  if (size > kMaxFileBytes || size > std::numeric_limits<size_t>::max())
    return Fail(error, "shader cache file exceeds the size limit");
  std::ifstream input(path, std::ios::binary);
  if (!input) return Fail(error, "cannot open shader cache file");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
    return Fail(error, "shader cache file is truncated while reading");
  return ParseFile(bytes, expected_key, stage, compiler_id, out, error);
}

bool Save(const std::filesystem::path& path, uint64_t key, ShaderStage stage,
          std::string_view compiler_id, const Entry& entry,
          std::string& error) {
  error.clear();
  if (path.empty()) return Fail(error, "shader cache path is empty");
  std::vector<uint8_t> bytes;
  if (!SerializeFile(key, stage, compiler_id, entry, bytes, error)) return false;

  std::error_code ec;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) return Fail(error, "cannot create shader cache directory: " + ec.message());
  }
  const std::filesystem::path temporary = TemporaryPath(path);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return Fail(error, "cannot create temporary shader cache file");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(temporary, ec);
      return Fail(error, "failed to write temporary shader cache file");
    }
    output.close();
    if (!output) {
      std::filesystem::remove(temporary, ec);
      return Fail(error, "failed to close temporary shader cache file");
    }
  }
  if (!AtomicReplace(temporary, path, error)) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

}  // namespace fear_nx::deko::shader_cache
