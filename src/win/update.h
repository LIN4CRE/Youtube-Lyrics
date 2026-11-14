#pragma once

#include <cstdint>
#include <string>

namespace ytl {

struct App;

struct UpdateRelease {
  std::string version;
  std::string tag;
  std::string asset_url;
  uint64_t asset_size = 0;
  std::string sha256;
};

struct UpdateStage {
  std::string path;
  std::string version;
  std::string sha256;
};

// Starts a lightweight worker and performs one silent stable-release check.
// `launched_by_update_helper`: this process was started by the replacement
// helper, which still owns the backup copy beside the executable.
void update_start(App& app, bool automatic_check = true,
                  bool launched_by_update_helper = false);
// Manual checks show a current-version result; automatic checks stay silent.
void update_request_check(App& app, bool manual);
// Starts the verified executable download after the user accepts the prompt.
void update_request_download(App& app, const UpdateRelease& release);
void update_cancel_download();
// Launches the built-in hidden replacement helper; the running process should
// close immediately after this succeeds.
bool update_apply_staged(App& app, const UpdateStage& stage, std::string* error);
void update_stop();
// One line describing the last check or download, for the diagnostic report.
std::string update_status_text();

}  // namespace ytl
