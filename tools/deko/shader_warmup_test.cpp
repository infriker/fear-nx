#include "deko/deko_shader_warmup.h"

#include <xxhash.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace warmup = fear_nx::deko::shader_warmup;
namespace {

constexpr size_t kHeaderBytes = 48;
constexpr size_t kChecksumOffset = 40;
constexpr size_t kPayloadOffset = 48;
constexpr uint64_t kChecksumSeed = 0x465741524d000001ull;

void PutU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  bytes[offset + 0] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void PutU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  PutU32(bytes, offset, static_cast<uint32_t>(value));
  PutU32(bytes, offset + 4, static_cast<uint32_t>(value >> 32));
}

uint64_t GuestHash(uint32_t type, const std::vector<uint32_t>& words) {
  std::vector<uint8_t> big_endian;
  big_endian.reserve(words.size() * sizeof(uint32_t));
  for (uint32_t word : words) {
    big_endian.push_back(static_cast<uint8_t>(word >> 24));
    big_endian.push_back(static_cast<uint8_t>(word >> 16));
    big_endian.push_back(static_cast<uint8_t>(word >> 8));
    big_endian.push_back(static_cast<uint8_t>(word));
  }
  return XXH3_64bits_withSeed(big_endian.data(), big_endian.size(), uint64_t(type));
}

uint64_t FileChecksum(const std::vector<uint8_t>& file) {
  std::vector<uint8_t> input;
  input.reserve(kChecksumOffset + file.size() - kHeaderBytes);
  input.insert(input.end(), file.begin(), file.begin() + kChecksumOffset);
  input.insert(input.end(), file.begin() + kPayloadOffset, file.end());
  return XXH3_64bits_withSeed(input.data(), input.size(), kChecksumSeed);
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

bool ReadBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto size = input.tellg();
  if (size < 0) return false;
  bytes.resize(static_cast<size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return input.gcount() == static_cast<std::streamsize>(bytes.size());
}

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

bool ExpectRejected(const std::filesystem::path& path, const char* message) {
  warmup::Entry output;
  output.shader_hash = 0x123456789abcdef0ull;
  output.ucode = {0xdecafbad};
  const auto before = output;
  std::string error;
  const bool loaded = warmup::Load(path, output, error);
  return Expect(!loaded, message) && Expect(!error.empty(), "rejection should include a reason") &&
         Expect(output.shader_hash == before.shader_hash && output.ucode == before.ucode,
                "failed load must leave output unchanged");
}

bool WriteTampered(const std::filesystem::path& from, const std::filesystem::path& to,
                   const auto& mutate, bool update_checksum) {
  std::vector<uint8_t> bytes;
  if (!ReadBytes(from, bytes)) return false;
  mutate(bytes);
  if (update_checksum) PutU64(bytes, kChecksumOffset, FileChecksum(bytes));
  return WriteBytes(to, bytes);
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "fear-nx-shader-warmup-test";
  std::error_code fs_error;
  std::filesystem::remove_all(root, fs_error);
  fs_error.clear();
  std::filesystem::create_directories(root, fs_error);
  if (!Expect(!fs_error, "create temporary test directory")) return 1;

  const std::filesystem::path good_path = root / "observed.fwarm";
  const std::filesystem::path bad_path = root / "bad.fwarm";
  warmup::Entry entry;
  entry.shader_type = warmup::kVertexShader;
  entry.modification = 0x0123456789abcdefull;
  entry.ucode = {0x12345678u, 0x00000000u, 0xffffffffu, 0x89abcdefu, 0x13579bdfu};
  entry.shader_hash = GuestHash(entry.shader_type, entry.ucode);

  std::string error;
  bool ok = true;
  const std::array<uint8_t, 4> expected_guest_bytes{0x12, 0x34, 0x56, 0x78};
  ok &= Expect(GuestHash(warmup::kVertexShader, {0x12345678u}) ==
                   XXH3_64bits_withSeed(expected_guest_bytes.data(), expected_guest_bytes.size(), 0),
               "runtime shader hash uses big-endian guest bytes and the stage seed");
  std::ostringstream expected_name;
  expected_name << "vs-" << std::hex << std::setfill('0') << std::setw(16) << entry.shader_hash
                << "-0123456789abcdef.fwarm";
  ok &= Expect(warmup::FileName(entry) == expected_name.str(), "deterministic VS filename");
  ok &= Expect(warmup::Save(good_path, entry, error), "save valid vertex fixture");
  if (!ok) {
    std::cerr << error << '\n';
    std::filesystem::remove_all(root, fs_error);
    return 1;
  }

  warmup::Entry loaded;
  ok &= Expect(warmup::Load(good_path, loaded, error), "load valid fixture");
  ok &= Expect(loaded.shader_type == entry.shader_type && loaded.shader_hash == entry.shader_hash &&
                   loaded.modification == entry.modification && loaded.ucode == entry.ucode,
               "round-trip preserves stage, hash, modification, and host-order words");

  // Replacing an existing manifest must leave a complete new file.
  warmup::Entry replacement = entry;
  replacement.shader_type = warmup::kPixelShader;
  replacement.modification++;
  replacement.ucode[0] ^= 0x01020304u;
  replacement.shader_hash = GuestHash(replacement.shader_type, replacement.ucode);
  ok &= Expect(warmup::Save(good_path, replacement, error), "atomically replace existing manifest");
  loaded = {};
  ok &= Expect(warmup::Load(good_path, loaded, error), "load replaced manifest");
  ok &= Expect(loaded.shader_type == replacement.shader_type &&
                   loaded.shader_hash == replacement.shader_hash &&
                   loaded.modification == replacement.modification && loaded.ucode == replacement.ucode,
               "replacement is complete and internally consistent");

  // A payload bit flip without updating the file checksum is rejected.
  ok &= Expect(WriteTampered(good_path, bad_path,
                             [](std::vector<uint8_t>& bytes) { bytes[kPayloadOffset] ^= 0x40; }, false),
               "write corruption fixture");
  ok &= Expect(ExpectRejected(bad_path, "reject corrupted payload checksum"),
               "corruption rejection");

  // Deliberately re-checksum header mutations so the loader reaches the stage
  // validator and the guest-code hash validator, rather than stopping at CRC.
  ok &= Expect(WriteTampered(good_path, bad_path,
                             [](std::vector<uint8_t>& bytes) { PutU32(bytes, 16, 2); }, true),
               "write invalid-stage fixture");
  ok &= Expect(ExpectRejected(bad_path, "reject unsupported shader stage"), "stage rejection");

  ok &= Expect(WriteTampered(good_path, bad_path,
                             [](std::vector<uint8_t>& bytes) { bytes[kPayloadOffset] ^= 0x01; }, true),
               "write wrong-hash fixture");
  ok &= Expect(ExpectRejected(bad_path, "reject microcode that no longer matches shader hash"),
               "shader-hash rejection");

  ok &= Expect(WriteTampered(good_path, bad_path,
                             [](std::vector<uint8_t>& bytes) {
                               for (size_t i = kPayloadOffset; i + 3 < bytes.size(); i += 4) {
                                 std::swap(bytes[i], bytes[i + 3]);
                                 std::swap(bytes[i + 1], bytes[i + 2]);
                               }
                             }, true),
               "write endian-mismatch fixture");
  ok &= Expect(ExpectRejected(bad_path, "reject swapped microcode byte order"),
               "endian rejection");

  ok &= Expect(WriteTampered(good_path, bad_path,
                             [](std::vector<uint8_t>& bytes) { PutU32(bytes, 20, 0); }, true),
               "write empty-code fixture");
  ok &= Expect(ExpectRejected(bad_path, "reject empty microcode"), "empty-code rejection");

  warmup::Entry invalid = entry;
  invalid.shader_type = 2;
  ok &= Expect(!warmup::Save(bad_path, invalid, error) && !error.empty(),
               "Save rejects unsupported stage");
  invalid = entry;
  invalid.shader_hash ^= 1;
  ok &= Expect(!warmup::Save(bad_path, invalid, error) && !error.empty(),
               "Save rejects mismatched runtime hash");

  std::filesystem::remove_all(root, fs_error);
  if (!ok) return 1;
  std::cout << "shader warmup manifest tests passed\n";
  return 0;
}
