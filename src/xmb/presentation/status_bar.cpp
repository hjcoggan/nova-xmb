#include "openxmb/xmb/status_bar.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openxmb::xmb {
namespace {

[[nodiscard]] bool append_number(int value, char *&cursor,
                                 char *end) noexcept {
  const auto result = std::to_chars(cursor, end, value);
  if (result.ec != std::errc{}) {
    return false;
  }
  cursor = result.ptr;
  return true;
}

[[nodiscard]] bool append_character(char value, char *&cursor,
                                    char *end) noexcept {
  if (cursor == end) {
    return false;
  }
  *cursor++ = value;
  return true;
}

[[nodiscard]] bool append_two_digits(int value, char *&cursor,
                                     char *end) noexcept {
  return append_character(static_cast<char>('0' + value / 10), cursor, end) &&
         append_character(static_cast<char>('0' + value % 10), cursor, end);
}

} // namespace

std::size_t format_status_bar_time(const StatusBarTime &time,
                                   std::span<char> destination) noexcept {
  if (!valid_status_bar_time(time) || destination.empty()) {
    if (!destination.empty()) {
      destination.front() = '\0';
    }
    return 0;
  }

  auto *cursor = destination.data();
  // Reserve one byte throughout for the required terminator.
  auto *end = destination.data() + destination.size() - 1;
  const auto success = append_number(time.day, cursor, end) &&
                       append_character('/', cursor, end) &&
                       append_number(time.month, cursor, end) &&
                       append_character(' ', cursor, end) &&
                       append_number(time.hour, cursor, end) &&
                       append_character(':', cursor, end) &&
                       append_two_digits(time.minute, cursor, end);
  if (!success || cursor > end) {
    destination.front() = '\0';
    return 0;
  }
  *cursor = '\0';
  return static_cast<std::size_t>(cursor - destination.data());
}

StatusBarTime status_bar_time_from_tm(const std::tm &time) noexcept {
  return {
      .day = time.tm_mday,
      .month = time.tm_mon + 1,
      .hour = time.tm_hour,
      .minute = time.tm_min,
      .second = time.tm_sec,
  };
}

std::optional<StatusBarTime> resolve_local_status_bar_time(
    std::chrono::system_clock::time_point time) noexcept {
  const auto raw_time = std::chrono::system_clock::to_time_t(time);
  std::tm local{};
#if defined(_WIN32)
  if (::localtime_s(&local, &raw_time) != 0) {
    return std::nullopt;
  }
#else
  if (::localtime_r(&raw_time, &local) == nullptr) {
    return std::nullopt;
  }
#endif
  const auto result = status_bar_time_from_tm(local);
  return valid_status_bar_time(result) ? std::optional{result} : std::nullopt;
}

ClockHandAngles resolve_clock_hand_angles(const StatusBarTime &time) noexcept {
  if (!valid_status_bar_time(time)) {
    return {};
  }
  constexpr auto pi = 3.14159265358979323846F;
  constexpr auto full_turn = 2.0F * pi;
  constexpr auto twelve_oclock = -0.5F * pi;
  const auto minute = static_cast<float>(time.minute) +
                      static_cast<float>(time.second) / 60.0F;
  const auto hour = static_cast<float>(time.hour % 12) + minute / 60.0F;
  return {
      .hour_radians = twelve_oclock + hour / 12.0F * full_turn,
      .minute_radians = twelve_oclock + minute / 60.0F * full_turn,
  };
}

std::array<StatusBarElement, 4>
parse_status_bar_order(std::string_view spec) noexcept {
  std::array<StatusBarElement, 4> out{};
  std::size_t count = 0;
  auto push_unique = [&](StatusBarElement elem) {
    for (std::size_t i = 0; i < count; ++i) {
      if (out[i] == elem) {
        return;
      }
    }
    if (count < out.size()) {
      out[count++] = elem;
    }
  };
  while (!spec.empty()) {
    const auto comma = spec.find(',');
    auto token = spec.substr(0, comma);
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (token == "battery") {
      push_unique(StatusBarElement::battery);
    } else if (token == "wifi") {
      push_unique(StatusBarElement::wifi);
    } else if (token == "bluetooth") {
      push_unique(StatusBarElement::bluetooth);
    } else if (token == "clock") {
      push_unique(StatusBarElement::clock);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    spec.remove_prefix(comma + 1);
  }
  for (const auto fallback : kDefaultStatusBarOrder) {
    push_unique(fallback);
  }
  return out;
}

std::string
format_status_bar_order(const std::array<StatusBarElement, 4> &order) {
  std::string result;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (i > 0) {
      result.push_back(',');
    }
    switch (order[i]) {
      case StatusBarElement::battery: result += "battery"; break;
      case StatusBarElement::wifi: result += "wifi"; break;
      case StatusBarElement::bluetooth: result += "bluetooth"; break;
      case StatusBarElement::clock: result += "clock"; break;
    }
  }
  return result;
}

std::string_view status_bar_element_label(StatusBarElement element) noexcept {
  switch (element) {
    case StatusBarElement::battery: return "Battery";
    case StatusBarElement::wifi: return "Wi-Fi";
    case StatusBarElement::bluetooth: return "Bluetooth";
    case StatusBarElement::clock: return "Clock";
  }
  return {};
}

bool probe_wifi_internet(std::string_view iface) {
#ifdef _WIN32
  (void)iface;
  return true;
#else
  if (iface.empty()) {
    return false;
  }
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return false;
  }
  struct sockaddr_in v4_src{};
  bool has_v4 = false;
  struct sockaddr_in6 v6_src{};
  bool has_v6 = false;
  for (auto *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !ifa->ifa_name || iface != ifa->ifa_name) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET && !has_v4) {
      std::memcpy(&v4_src, ifa->ifa_addr, sizeof(v4_src));
      has_v4 = true;
    } else if (ifa->ifa_addr->sa_family == AF_INET6 && !has_v6) {
      const auto *addr6 = reinterpret_cast<const sockaddr_in6 *>(ifa->ifa_addr);
      if (!IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
        v6_src = *addr6;
        has_v6 = true;
      }
    }
  }
  freeifaddrs(ifaddr);

  auto try_connect = [](int family, const struct sockaddr *src, socklen_t src_len,
                        const struct sockaddr *dst, socklen_t dst_len) -> bool {
    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      return false;
    }
    if (::bind(fd, src, src_len) != 0) {
      ::close(fd);
      return false;
    }
    const int rc = ::connect(fd, dst, dst_len);
    bool ok = (rc == 0);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      if (::poll(&pfd, 1, 1500) > 0 && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
          ok = true;
        }
      }
    }
    ::close(fd);
    return ok;
  };

  if (has_v4) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(443);
    ::inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
    v4_src.sin_port = 0;
    if (try_connect(AF_INET, reinterpret_cast<const sockaddr *>(&v4_src), sizeof(v4_src),
                    reinterpret_cast<const sockaddr *>(&dst), sizeof(dst))) {
      return true;
    }
  }
  if (has_v6) {
    sockaddr_in6 dst6{};
    dst6.sin6_family = AF_INET6;
    dst6.sin6_port = htons(443);
    ::inet_pton(AF_INET6, "2606:4700:4700::1111", &dst6.sin6_addr);
    v6_src.sin6_port = 0;
    if (try_connect(AF_INET6, reinterpret_cast<const sockaddr *>(&v6_src), sizeof(v6_src),
                    reinterpret_cast<const sockaddr *>(&dst6), sizeof(dst6))) {
      return true;
    }
  }
  return false;
#endif
}

} // namespace openxmb::xmb
