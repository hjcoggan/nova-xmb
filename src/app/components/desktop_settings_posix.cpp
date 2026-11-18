#include "desktop_settings_posix.hpp"

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace app::desktop_settings::detail {

int open_devnull_rdwr() noexcept {
#ifdef _WIN32
  return -1;
#else
  return ::open("/dev/null", O_RDWR);
#endif
}

} // namespace app::desktop_settings::detail
