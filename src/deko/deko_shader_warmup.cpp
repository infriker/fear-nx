#include "deko_shader_warmup.h"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fear_nx::deko::shader_warmup {
namespace {

constexpr std::array<uint8_t, 8> kMagic{'F', 'W', 'A', 'R', 'M', '\r', '\n', 0};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 48;
constexpr uint32_t kMaxMicrocodeBytes = 4u * 1024u * 1024u;
constexpr uint64_t kChecksumSeed = 0x465741524d000001ull;  // "FWARM" + version 1
constexpr size_t kChecksumInputHeaderBytes = 40;

void SetError(std::string& error, const char* message) { error = message; }

bool IsValidStage(uint32_t stage) { return stage == kVertexShader || stage == kPixelShader; }

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
  AppendU32(out, static_cast<uint32_t>(value));
  AppendU32(out, static_cast<uint32_t>(value >> 32));
}

uint32_t ReadU32(const uint8_t* data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
         (uint32_t(data[3]) << 24);
}

uint64_t ReadU64(const uint8_t* data) {
  return uint64_t(ReadU32(data)) | (uint64_t(ReadU32(data + 4)) << 32);
}

void WriteU32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

void WriteU64(uint8_t* data, uint64_t value) {
  WriteU32(data, static_cast<uint32_t>(value));
  WriteU32(data + 4, static_cast<uint32_t>(value >> 32));
}

std::vector<uint8_t> BigEndianMicrocode(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  bytes.reserve(words.size() * sizeof(uint32_t));
  for (uint32_t word : words) {
    bytes.push_back(static_cast<uint8_t>(word >> 24));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word));
  }
  return bytes;
}

uint64_t ComputeGuestHash(uint32_t shader_type, const std::vector<uint32_t>& words) {
  const std::vector<uint8_t> bytes = BigEndianMicrocode(words);
  return XXH3_64bits_withSeed(bytes.data(), bytes.size(), uint64_t(shader_type));
}

uint64_t ComputeFileChecksum(const std::vector<uint8_t>& file) {
  // The checksum covers every header field before the checksum plus the
  // canonical little-endian payload. The checksum slot itself is excluded.
  std::vector<uint8_t> bytes;
  bytes.reserve(kChecksumInputHeaderBytes + file.size() - kHeaderBytes);
  bytes.insert(bytes.end(), file.begin(), file.begin() + kChecksumInputHeaderBytes);
  bytes.insert(bytes.end(), file.begin() + kHeaderBytes, file.end());
  return XXH3_64bits_withSeed(bytes.data(), bytes.size(), kChecksumSeed);
}

bool ValidateEntry(const Entry& entry, std::string& error) {
  if (!IsValidStage(entry.shader_type)) {
    SetError(error, "shader stage must be 0 (vertex) or 1 (pixel)");
    return false;
  }
  if (entry.ucode.empty()) {
    SetError(error, "shader microcode is empty");
    return false;
  }
  if (entry.ucode.size() > kMaxMicrocodeBytes / sizeof(uint32_t)) {
    SetError(error, "shader microcode exceeds the 4 MiB limit");
    return false;
  }
  if (ComputeGuestHash(entry.shader_type, entry.ucode) != entry.shader_hash) {
    SetError(error, "shader hash does not match the big-endian microcode bytes");
    return false;
  }
  return true;
}

bool BuildFile(const Entry& entry, std::vector<uint8_t>& file, std::string& error) {
  if (!ValidateEntry(entry, error)) return false;

  const uint32_t word_count = static_cast<uint32_t>(entry.ucode.size());
  const size_t payload_bytes = size_t(word_count) * sizeof(uint32_t);
  file.clear();
  file.reserve(kHeaderBytes + payload_bytes);
  file.insert(file.end(), kMagic.begin(), kMagic.end());
  AppendU32(file, kVersion);
  AppendU32(file, kHeaderBytes);
  AppendU32(file, entry.shader_type);
  AppendU32(file, word_count);
  AppendU64(file, entry.shader_hash);
  AppendU64(file, entry.modification);
  AppendU64(file, 0);  // checksum filled after the payload
  for (uint32_t word : entry.ucode) AppendU32(file, word);
  if (file.size() != kHeaderBytes + payload_bytes) {
    SetError(error, "internal .fwarm size calculation failed");
    return false;
  }
  WriteU64(file.data() + kChecksumInputHeaderBytes, ComputeFileChecksum(file));
  return true;
}

std::string SystemMessage(unsigned long code) {
#if defined(_WIN32)
  return std::system_category().message(static_cast<int>(code));
#else
  return std::error_code(static_cast<int>(code), std::generic_category()).message();
#endif
}

std::atomic<uint64_t> g_temp_counter{0};

std::filesystem::path MakeTempPath(const std::filesystem::path& path, uint64_t counter) {
  std::filesystem::path temporary = path;
#if defined(_WIN32)
  const auto process_id = static_cast<uint64_t>(GetCurrentProcessId());
#else
  const auto process_id = static_cast<uint64_t>(::getpid());
#endif
  temporary += ".tmp." + std::to_string(process_id) + "." + std::to_string(counter);
  return temporary;
}

bool WriteTempExclusive(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                        bool& already_exists, std::string& error) {
  already_exists = false;
#if defined(_WIN32)
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
      already_exists = true;
      return false;
    }
    error = "could not create temporary .fwarm file: " + SystemMessage(code);
    return false;
  }

  size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset,
                                                            std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
      const DWORD code = GetLastError();
      CloseHandle(file);
      DeleteFileW(path.c_str());
      error = "could not write temporary .fwarm file: " + SystemMessage(code);
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(file)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    DeleteFileW(path.c_str());
    error = "could not flush temporary .fwarm file: " + SystemMessage(code);
    return false;
  }
  if (!CloseHandle(file)) {
    const DWORD code = GetLastError();
    DeleteFileW(path.c_str());
    error = "could not close temporary .fwarm file: " + SystemMessage(code);
    return false;
  }
  return true;
#else
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (file < 0) {
    const int code = errno;
    if (code == EEXIST) {
      already_exists = true;
      return false;
    }
    error = "could not create temporary .fwarm file: " + SystemMessage(code);
    return false;
  }

  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(file, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      const int code = errno;
      ::close(file);
      ::unlink(path.c_str());
      error = "could not write temporary .fwarm file: " + SystemMessage(code);
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  if (::fsync(file) != 0) {
    const int code = errno;
    ::close(file);
    ::unlink(path.c_str());
    error = "could not flush temporary .fwarm file: " + SystemMessage(code);
    return false;
  }
  if (::close(file) != 0) {
    const int code = errno;
    ::unlink(path.c_str());
    error = "could not close temporary .fwarm file: " + SystemMessage(code);
    return false;
  }
  return true;
#endif
}

void RemoveFile(const std::filesystem::path& path) {
#if defined(_WIN32)
  DeleteFileW(path.c_str());
#else
  ::unlink(path.c_str());
#endif
}

bool ReplaceFile(const std::filesystem::path& temporary, const std::filesystem::path& path,
                 std::string& error) {
#if defined(_WIN32)
  if (MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  const DWORD code = GetLastError();
  error = "could not atomically replace .fwarm file: " + SystemMessage(code);
  return false;
#else
  if (::rename(temporary.c_str(), path.c_str()) == 0) return true;
  const int code = errno;
  error = "could not atomically replace .fwarm file: " + SystemMessage(code);
  return false;
#endif
}

}  // namespace

std::string FileName(const Entry& entry) {
  if (!IsValidStage(entry.shader_type)) return {};
  std::ostringstream name;
  name << (entry.shader_type == kVertexShader ? "vs-" : "ps-") << std::hex << std::setfill('0')
       << std::setw(16) << entry.shader_hash << '-' << std::setw(16) << entry.modification
       << ".fwarm";
  return name.str();
}

bool Save(const std::filesystem::path& path, const Entry& entry, std::string& error) {
  error.clear();
  if (path.empty() || path.filename().empty()) {
    SetError(error, "destination path is empty");
    return false;
  }

  std::vector<uint8_t> bytes;
  if (!BuildFile(entry, bytes, error)) return false;

  std::error_code fs_error;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, fs_error);
    if (fs_error) {
      error = "could not create .fwarm directory: " + fs_error.message();
      return false;
    }
  }

  std::filesystem::path temporary;
  bool wrote = false;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    temporary = MakeTempPath(path, g_temp_counter.fetch_add(1, std::memory_order_relaxed));
    bool already_exists = false;
    if (WriteTempExclusive(temporary, bytes, already_exists, error)) {
      wrote = true;
      break;
    }
    if (!already_exists) return false;
  }
  if (!wrote) {
    SetError(error, "could not reserve a unique temporary .fwarm path");
    return false;
  }

  if (ReplaceFile(temporary, path, error)) return true;
  RemoveFile(temporary);
  return false;
}

bool Load(const std::filesystem::path& path, Entry& out, std::string& error) {
  error.clear();
  std::error_code fs_error;
  const uintmax_t file_bytes = std::filesystem::file_size(path, fs_error);
  if (fs_error) {
    error = "could not read .fwarm file size: " + fs_error.message();
    return false;
  }
  if (file_bytes < kHeaderBytes || file_bytes > kHeaderBytes + kMaxMicrocodeBytes) {
    SetError(error, ".fwarm file size is outside the supported bounds");
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SetError(error, "could not open .fwarm file");
    return false;
  }
  std::vector<uint8_t> file(static_cast<size_t>(file_bytes));
  input.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (input.gcount() != static_cast<std::streamsize>(file.size())) {
    SetError(error, ".fwarm file ended before the recorded size");
    return false;
  }
  char trailing_byte = 0;
  if (input.get(trailing_byte)) {
    SetError(error, ".fwarm file changed size while loading");
    return false;
  }

  if (!std::equal(kMagic.begin(), kMagic.end(), file.begin())) {
    SetError(error, ".fwarm magic does not match");
    return false;
  }
  if (ReadU32(file.data() + 8) != kVersion) {
    SetError(error, "unsupported .fwarm version");
    return false;
  }
  if (ReadU32(file.data() + 12) != kHeaderBytes) {
    SetError(error, "unsupported .fwarm header size");
    return false;
  }

  Entry loaded;
  loaded.shader_type = ReadU32(file.data() + 16);
  if (!IsValidStage(loaded.shader_type)) {
    SetError(error, "shader stage must be 0 (vertex) or 1 (pixel)");
    return false;
  }
  const uint32_t word_count = ReadU32(file.data() + 20);
  if (word_count == 0 || word_count > kMaxMicrocodeBytes / sizeof(uint32_t)) {
    SetError(error, "shader microcode word count is outside the supported bounds");
    return false;
  }
  const size_t payload_bytes = size_t(word_count) * sizeof(uint32_t);
  if (file.size() != kHeaderBytes + payload_bytes) {
    SetError(error, ".fwarm payload length does not match its header");
    return false;
  }

  loaded.shader_hash = ReadU64(file.data() + 24);
  loaded.modification = ReadU64(file.data() + 32);
  const uint64_t stored_checksum = ReadU64(file.data() + kChecksumInputHeaderBytes);
  if (ComputeFileChecksum(file) != stored_checksum) {
    SetError(error, ".fwarm checksum does not match");
    return false;
  }

  loaded.ucode.resize(word_count);
  const uint8_t* payload = file.data() + kHeaderBytes;
  for (uint32_t i = 0; i < word_count; ++i) {
    loaded.ucode[i] = ReadU32(payload + size_t(i) * sizeof(uint32_t));
  }
  if (ComputeGuestHash(loaded.shader_type, loaded.ucode) != loaded.shader_hash) {
    SetError(error, "shader hash does not match the big-endian microcode bytes");
    return false;
  }

  out = std::move(loaded);
  return true;
}

}  // namespace fear_nx::deko::shader_warmup
