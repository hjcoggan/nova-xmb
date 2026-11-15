#pragma once

// One line from a control-socket connection (the pause socket: PAUSE <id>,
// RESUME, QUIT, POWER_MENU). The listening socket wakes the menu as soon as
// a peer connects, which can be before the peer has written anything, so
// the reader waits up to timeout_ms for the line instead of dropping it.

#include <cerrno>
#include <chrono>
#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace openxmb::xmb {

// fd may be non-blocking. Returns the text before the first newline (or
// everything sent before the peer closed), without trailing whitespace;
// nullopt if nothing arrived in time or the line exceeds limit bytes.
inline std::optional<std::string> read_ipc_message(int fd, int timeout_ms, std::size_t limit = 256) {
  if (fd < 0 || limit == 0) return std::nullopt;
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string text;
  bool oversized = false;
  for (;;) {
    char buf[128];
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      text.append(buf, static_cast<std::size_t>(n));
      const auto newline = text.find('\n');
      if (newline != std::string::npos) {
        if (newline > limit) {
          oversized = true;
          break;
        }
        text.resize(newline);
        break;
      }
      if (text.size() > limit) {
        oversized = true;
        break;
      }
      continue;
    }
    if (n == 0) break;  // the peer closed
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) break;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
    if (left <= 0) break;
    pollfd p{fd, POLLIN, 0};
    const int rc = poll(&p, 1, static_cast<int>(left));
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) break;
    if ((p.revents & (POLLERR | POLLNVAL)) && !(p.revents & POLLIN)) break;
    if ((p.revents & (POLLIN | POLLHUP)) == 0) break;
  }
  if (oversized) return std::nullopt;
  while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\n')) text.pop_back();
  if (text.empty()) return std::nullopt;
  return text;
}

}  // namespace openxmb::xmb
