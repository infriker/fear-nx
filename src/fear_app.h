// F.E.A.R. Xbox 360 ReXGlue application entry and Switch platform setup.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <rex/crypto/sha256.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#if REX_PLATFORM_SWITCH
#include <rex/filesystem.h>
#endif

#include "native/native_system.h"

class FearNxApp final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FearNxApp>(
        new FearNxApp(ctx, "fear-nx", PPCImageConfig));
  }

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The app-owned Xenos Vulkan renderer is the Switch default. A config
    // override may select the SDK's reference Xenos backend for diagnosis.
    if (fear_nx::native::Enabled()) {
      config.graphics = fear_nx::native::CreateGraphicsSystem();
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_SWITCH
    auto package = rex::filesystem::GetExecutableFolder();
    if (package.empty()) {
      // Eden's direct NRO loader may supply argc=0, unlike hbloader. Keep the
      // standard SD package usable without inventing a command-line argument.
      package = "/switch/fear-nx";
      paths.config_path = package / "fear-nx.toml";
    }
    // SetupEnvironment pre-fills user_data_root and cache_root from the generic
    // SDK defaults even when their cvars are unset, so test the cvars instead
    // of testing whether the incoming paths happen to be empty.
    if (REXCVAR_GET(game_data_root).empty()) paths.game_data_root = package / "game";
    if (REXCVAR_GET(user_data_root).empty()) paths.user_data_root = package / "userdata";
    if (REXCVAR_GET(update_data_root).empty()) paths.update_data_root = package / "update";
    if (REXCVAR_GET(cache_root).empty()) paths.cache_root = package / "cache";
    if (REXCVAR_GET(metadata_root).empty()) paths.metadata_root = package / "metadata";
#endif
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    auto paths = defaults;

    // SetupEnvironment loads the TOML after OnConfigurePaths. Reapply any
    // explicit path cvars here, before ConstructRuntime consumes PathConfig.
    ApplyConfigPathOverride(REXCVAR_GET(game_data_root), paths.game_data_root);
    ApplyConfigPathOverride(REXCVAR_GET(user_data_root), paths.user_data_root);
    ApplyConfigPathOverride(REXCVAR_GET(update_data_root), paths.update_data_root);
    ApplyConfigPathOverride(REXCVAR_GET(cache_root), paths.cache_root);
    ApplyConfigPathOverride(REXCVAR_GET(metadata_root), paths.metadata_root);

#if REX_PLATFORM_SWITCH
    EnsureDirectory(paths.user_data_root, "user data");
    EnsureDirectory(paths.update_data_root, "update data");
    EnsureDirectory(paths.cache_root, "cache");
    EnsureDirectory(paths.metadata_root, "metadata");
#endif
    ValidateGameExecutable(paths.game_data_root);
    return paths;
  }

 private:
  static constexpr char kExpectedDefaultXexSha256[] =
      "e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792";

  static void EnsureDirectory(const std::filesystem::path& path, const char* label) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
      REXLOG_WARN("F.E.A.R. NX: could not create {} directory {}: {}", label,
                  path.string(), ec.message());
    }
  }

  static void ApplyConfigPathOverride(const std::string& configured,
                                     std::filesystem::path& resolved) {
    if (!configured.empty()) resolved = configured;
  }

  static void ValidateGameExecutable(std::filesystem::path& game_root) {
    if (game_root.empty()) {
      REXLOG_ERROR("F.E.A.R. NX: game data path is not configured");
      return;
    }

    const auto executable = game_root / "default.xex";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executable, ec)) {
      REXLOG_ERROR("F.E.A.R. NX: expected the supported Xbox 360 default.xex at {}",
                   executable.string());
      game_root.clear();
      return;
    }

    const auto actual = rex::crypto::sha256_file(executable);
    if (actual != kExpectedDefaultXexSha256) {
      REXLOG_ERROR("F.E.A.R. NX: default.xex does not match the executable used for this build; "
                   "expected SHA-256 {}, got {}",
                   kExpectedDefaultXexSha256, actual.empty() ? "<unreadable>" : actual);
      game_root.clear();
      return;
    }

    REXLOG_INFO("F.E.A.R. NX: verified supported default.xex ({})",
                kExpectedDefaultXexSha256);
  }
};
