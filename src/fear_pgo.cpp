// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from infriker/mini-ninjas-nx v1.0.8, src/minininjas_pgo.cpp.
#include "fear_pgo.h"
#include "native/native_system.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory_resource>
#include <mutex>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>

extern "C" {
#include <gcov.h>
extern const gcov_info* const __fear_gcov_begin[];
extern const gcov_info* const __fear_gcov_end[];
}

namespace fear_nx::pgo {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Stream {
  std::FILE* file;
  std::pmr::monotonic_buffer_resource scratch;
  bool failed = false;
};
void Write(const void* data, unsigned size, void* context) {
  auto& out = *static_cast<Stream*>(context);
  if (!out.failed && size && std::fwrite(data, 1, size, out.file) != size) out.failed = true;
}
void Filename(const char* filename, void* context) {
  // Depending on its working directory, GCC separates path components with '/' or '#'.
  const char* slash = std::strrchr(filename, '/');
  const char* basename = slash ? slash + 1 : filename;
  const char* encoded = std::strrchr(basename, '#');
  __gcov_filename_to_gcfn(encoded ? encoded + 1 : basename, Write, context);
}
void* Allocate(unsigned size, void* context) {
  return static_cast<Stream*>(context)->scratch.allocate(size);
}

fs::path NewSession(std::error_code& ec) {
  auto folder = rex::filesystem::GetExecutableFolder();
  if (folder.empty()) folder = "/switch/fear-nx";
  const auto root = folder / "pgo";
  fs::create_directories(root, ec);
  for (unsigned i = 1; !ec; ++i) {
    auto dir = root / ("run-" + std::to_string(i));
    // Some libnx filesystems report a successful mkdir for an existing folder.
    if (fs::exists(dir, ec)) continue;
    if (!ec && fs::create_directory(dir, ec)) return dir;
  }
  return {};
}
fs::path Snapshot(const fs::path& dir, unsigned n) {
  return dir / ("profile-" + std::to_string(n) + ".pgo");
}
}  // namespace

bool Save() {
  // Focus/exit arrive on the UI thread, periodic checkpoints on the GPU ring thread.
  static std::mutex mutex;
  static fs::path session;
  static uint64_t saved_at_frame = 0;
  static unsigned completed = 0;
  const std::lock_guard lock(mutex);
  const uint64_t frame = native::SwapsPresented();
  if (!frame || frame == saved_at_frame) return true;
  std::error_code ec;
  if (session.empty()) session = NewSession(ec);
  if (ec || session.empty()) {
    REXLOG_ERROR("F.E.A.R. PGO directory: {}", ec.message());
    return false;
  }
  const auto temp = session / "writing.tmp";
  auto* file = std::fopen(temp.c_str(), "wb");
  if (!file) {
    REXLOG_ERROR("F.E.A.R. PGO could not open {}", temp.string());
    return false;
  }
  std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);
  Stream stream{file, {}};
  for (auto item = __fear_gcov_begin; item != __fear_gcov_end && !stream.failed; ++item) {
    __gcov_info_to_gcda(*item, Filename, Write, Allocate, &stream);
    stream.scratch.release();
  }
  const bool short_write = stream.failed || std::ferror(file);
  if (std::fclose(file) != 0 || short_write) {
    REXLOG_ERROR("F.E.A.R. PGO could not finish writing {}", temp.string());
    return false;
  }
  fs::rename(temp, Snapshot(session, completed + 1), ec);
  if (ec) {
    REXLOG_ERROR("F.E.A.R. PGO could not publish the snapshot: {}", ec.message());
    return false;
  }
  if (completed) fs::remove(Snapshot(session, completed), ec);
  ++completed;
  saved_at_frame = frame;
  return true;
}

void OnFrame() {
  using namespace std::chrono_literals;
  // Called only on the ring thread, after counting the guest swap.
  const auto now = Clock::now();
  static auto next = now + 30s;
  if (now < next) return;
  const bool written = Save();
  next = Clock::now() + (written ? 120s : 10s);
}
}  // namespace fear_nx::pgo
