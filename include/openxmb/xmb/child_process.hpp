#pragma once

// A short-lived helper process (the pause capture's grim) run without a
// shell and never waited for on the render thread: poll() checks it,
// terminate() lets go of it at once, and reap_orphans() collects
// terminated children on later frames, killing any that outlive a grace
// period.

#include <chrono>
#include <csignal>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace openxmb::xmb {

class child_process {
 public:
  child_process() = default;
  child_process(const child_process &) = delete;
  child_process &operator=(const child_process &) = delete;
  ~child_process() { terminate(); }

  // Starts argv[0] from PATH. False if it could not be started.
  bool spawn(std::vector<std::string> argv) {
    terminate();
    result_.reset();
    std::vector<char *> args;
    for (auto &arg : argv) args.push_back(arg.data());
    args.push_back(nullptr);
    pid_t pid = -1;
    if (argv.empty() || posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return false;
    pid_ = pid;
    return true;
  }

  [[nodiscard]] bool running() const noexcept { return pid_ > 0; }

  // nullopt while it runs; then whether it exited with status 0.
  std::optional<bool> poll() {
    if (pid_ > 0) {
      int status = 0;
      const pid_t done = waitpid(pid_, &status, WNOHANG);
      if (done == pid_ || done < 0) {
        result_ = done == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        pid_ = -1;
      }
    }
    return result_;
  }

  // Lets go of the child without waiting: it is asked to stop and reaped
  // by reap_orphans().
  void terminate() noexcept {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    orphans().push_back({pid_, std::chrono::steady_clock::now() + grace, false});
    pid_ = -1;
  }

  // Collects terminated children; one past its grace period gets SIGKILL.
  // Returns how many are still outstanding. Never blocks.
  static std::size_t reap_orphans() noexcept {
    auto &list = orphans();
    const auto now = std::chrono::steady_clock::now();
    for (auto it = list.begin(); it != list.end();) {
      int status = 0;
      const pid_t done = waitpid(it->pid, &status, WNOHANG);
      if (done == it->pid || done < 0) {
        it = list.erase(it);
        continue;
      }
      if (!it->killed && now >= it->deadline) {
        kill(it->pid, SIGKILL);
        it->killed = true;
      }
      ++it;
    }
    return list.size();
  }

 private:
  static constexpr auto grace = std::chrono::milliseconds(500);
  struct orphan {
    pid_t pid;
    std::chrono::steady_clock::time_point deadline;
    bool killed;
  };
  static std::vector<orphan> &orphans() noexcept {
    static std::vector<orphan> list;
    return list;
  }
  pid_t pid_ = -1;
  std::optional<bool> result_;
};

}  // namespace openxmb::xmb
