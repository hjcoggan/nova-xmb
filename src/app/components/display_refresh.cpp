#include "display_refresh.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace app::display_refresh {
namespace {

#ifndef _WIN32
[[nodiscard]] std::string sway_socket_path() {
  if (const char *env = std::getenv("SWAYSOCK"); env && *env) {
    return env;
  }
  if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
    const auto candidate = std::filesystem::path(runtime) / "sway-ipc.sock";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate.string();
    }
  }
  return {};
}

[[nodiscard]] bool run_swaymsg(const std::vector<std::string> &args,
                               std::string *stdout_out = nullptr) {
  if (stdout_out) {
    stdout_out->clear();
  }
  const auto sock = sway_socket_path();
  if (sock.empty() || args.empty()) {
    return false;
  }

  int pipefd[2]{-1, -1};
  if (stdout_out != nullptr && ::pipe2(pipefd, O_CLOEXEC) != 0) {
    return false;
  }

  std::vector<std::string> argv_storage;
  argv_storage.reserve(args.size() + 3);
  argv_storage.emplace_back("swaymsg");
  argv_storage.emplace_back("-s");
  argv_storage.push_back(sock);
  for (const auto &a : args) {
    argv_storage.push_back(a);
  }
  std::vector<char *> c_args;
  c_args.reserve(argv_storage.size() + 1);
  for (auto &s : argv_storage) {
    c_args.push_back(s.data());
  }
  c_args.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
    if (stdout_out != nullptr) {
      ::close(pipefd[0]);
      ::dup2(pipefd[1], STDOUT_FILENO);
      if (pipefd[1] > STDOUT_FILENO) {
        ::close(pipefd[1]);
      }
    } else if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
    }
    ::execvp("swaymsg", c_args.data());
    ::_exit(127);
  }

  if (stdout_out != nullptr) {
    ::close(pipefd[1]);
    if (pid > 0) {
      char buf[4096];
      for (;;) {
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
          if (stdout_out->size() < 4U * 1024U * 1024U) {
            stdout_out->append(buf, static_cast<std::size_t>(n));
          }
        } else if (n == 0) {
          break;
        } else if (errno != EINTR) {
          break;
        }
      }
    }
    ::close(pipefd[0]);
  }
  if (pid < 0) {
    return false;
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

[[nodiscard]] std::string format_mode(const ModeInfo &mode) {
  if (mode.refresh_mhz % 1000 == 0) {
    return std::format("{}x{}@{}Hz", mode.width, mode.height, mode.refresh_mhz / 1000);
  }
  return std::format("{}x{}@{:.3f}Hz", mode.width, mode.height,
                     static_cast<double>(mode.refresh_mhz) / 1000.0);
}

[[nodiscard]] const ModeInfo *pick_mode_for_hz(const DisplayCapabilities &caps,
                                               int target_hz, bool vrr) {
  if (caps.modes.empty()) {
    return nullptr;
  }
  const auto highest_it = std::max_element(
      caps.modes.begin(), caps.modes.end(),
      [](const ModeInfo &a, const ModeInfo &b) { return a.refresh_mhz < b.refresh_mhz; });
  if (vrr || target_hz <= 0) {
    return &(*highest_it);
  }
  const int target_mhz = target_hz * 1000;
  const ModeInfo *best_exact = nullptr;
  int best_diff = 1000000;
  for (const auto &m : caps.modes) {
    const int diff = std::abs(m.refresh_mhz - target_mhz);
    if (diff <= 1500 && diff < best_diff) {
      best_diff = diff;
      best_exact = &m;
    }
  }
  if (best_exact != nullptr) {
    return best_exact;
  }
  // No exact panel mode (for example 30 Hz or 90 Hz on a 60/120 Hz panel):
  // choose the smallest panel mode >= target_hz (so 30 -> 60 Hz, 90 -> 120 Hz).
  const ModeInfo *best_above = nullptr;
  for (const auto &m : caps.modes) {
    if (m.refresh_mhz + 1500 >= target_mhz &&
        (best_above == nullptr || m.refresh_mhz < best_above->refresh_mhz)) {
      best_above = &m;
    }
  }
  return best_above != nullptr ? best_above : &(*highest_it);
}

void apply_mode(int target_hz, bool vrr) {
#ifdef _WIN32
  (void)target_hz;
  (void)vrr;
#else
  const auto caps = query_display_capabilities();
  if (caps.output_name.empty()) {
    return;
  }
  if (const auto *mode = pick_mode_for_hz(caps, target_hz, vrr)) {
    static_cast<void>(run_swaymsg(
        {"output " + caps.output_name + " mode " + format_mode(*mode)}));
  }
  if (caps.adaptive_sync) {
    static_cast<void>(run_swaymsg(
        {"output " + caps.output_name + " adaptive_sync " + (vrr ? "on" : "off")}));
  }
#endif
}

} // namespace

DisplayCapabilities query_display_capabilities() {
  DisplayCapabilities caps{};
#ifndef _WIN32
  std::string json_text;
  if (!run_swaymsg({"-t", "get_outputs"}, &json_text) || json_text.empty()) {
    return caps;
  }
  const auto outputs = nlohmann::json::parse(json_text, nullptr, false);
  if (!outputs.is_array()) {
    return caps;
  }
  for (const auto &out : outputs) {
    if (!out.is_object() || !out.value("active", false)) {
      continue;
    }
    caps.output_name = out.value("name", std::string{});
    if (out.contains("features") && out["features"].is_object()) {
      caps.adaptive_sync = out["features"].value("adaptive_sync", false);
    }
    int current_w = 0;
    int current_h = 0;
    if (out.contains("current_mode") && out["current_mode"].is_object()) {
      current_w = out["current_mode"].value("width", 0);
      current_h = out["current_mode"].value("height", 0);
    }
    if (out.contains("modes") && out["modes"].is_array()) {
      for (const auto &m : out["modes"]) {
        if (!m.is_object()) {
          continue;
        }
        const int w = m.value("width", 0);
        const int h = m.value("height", 0);
        const int r = m.value("refresh", 0);
        if (w <= 0 || h <= 0 || r <= 0) {
          continue;
        }
        if (current_w > 0 && current_h > 0 && (w != current_w || h != current_h)) {
          continue;
        }
        caps.modes.push_back({w, h, r});
      }
    }
    if (!caps.output_name.empty()) {
      break;
    }
  }
#endif
  return caps;
}

std::vector<std::string> available_refresh_rate_choices(const DisplayCapabilities &caps) {
  std::vector<std::string> choices{"30 Hz", "60 Hz", "90 Hz", "120 Hz"};
  if (caps.adaptive_sync) {
    choices.emplace_back("VRR");
  }
  return choices;
}

bool is_valid_refresh_choice(std::string_view choice, const DisplayCapabilities &caps) {
  for (const auto &option : available_refresh_rate_choices(caps)) {
    if (option == choice) {
      return true;
    }
  }
  return false;
}

int refresh_choice_target_hz(std::string_view choice) noexcept {
  if (choice == "VRR" || choice == "vrr") {
    return 0;
  }
  if (choice.starts_with("30")) {
    return 30;
  }
  if (choice.starts_with("60")) {
    return 60;
  }
  if (choice.starts_with("90")) {
    return 90;
  }
  if (choice.starts_with("120")) {
    return 120;
  }
  return 120;
}

void apply_launcher_refresh_rate() {
  apply_mode(60, false);
}

void apply_in_game_refresh_rate(std::string_view choice) {
  if (choice == "VRR" || choice == "vrr") {
    const auto caps = query_display_capabilities();
    if (caps.adaptive_sync) {
      apply_mode(0, true);
    } else {
      apply_mode(120, false);
    }
    return;
  }
  apply_mode(refresh_choice_target_hz(choice), false);
}

} // namespace app::display_refresh
