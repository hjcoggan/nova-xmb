#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace openxmb::xmb {

inline std::string strip_ansi(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\x1b' && i + 1 < input.size() && input[i + 1] == '[') {
      i += 2;
      while (i < input.size() && !(input[i] >= '@' && input[i] <= '~')) {
        ++i;
      }
      continue;
    }
    out.push_back(input[i]);
  }
  return out;
}

inline std::string trim_ascii(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return std::string(sv);
}

inline std::optional<std::string> parse_iwctl_field(std::string_view output, std::string_view key) {
  const auto clean = strip_ansi(output);
  std::istringstream in(clean);
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = trim_ascii(line);
    if (!trimmed.starts_with(key)) {
      continue;
    }
    std::string_view rest = std::string_view(trimmed).substr(key.size());
    if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t' && rest.front() != ':') {
      continue;
    }
    if (!rest.empty() && rest.front() == ':') {
      rest.remove_prefix(1);
    }
    return trim_ascii(rest);
  }
  return std::nullopt;
}

inline bool parse_iwctl_powered(std::string_view output) {
  const auto val = parse_iwctl_field(output, "Powered");
  return val.has_value() && *val == "on";
}

inline std::string parse_iwctl_connected_network(std::string_view output) {
  return parse_iwctl_field(output, "Connected network").value_or(std::string{});
}

}  // namespace openxmb::xmb
