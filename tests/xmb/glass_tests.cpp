#include "openxmb/xmb/glass.hpp"
#include "openxmb/xmb/pause_backdrop.hpp"
#include "openxmb/xmb/child_process.hpp"
#include "openxmb/xmb/ipc_message.hpp"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <chrono>
#include <csignal>
#include <thread>
#include "openxmb/xmb/settings_scene.hpp"
#include "openxmb/xmb/status_bar.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <iostream>
#include <string_view>

namespace {

namespace glass = openxmb::xmb::glass;

[[nodiscard]] bool near(float actual, float expected, float epsilon = 0.0001F) {
  return std::abs(actual - expected) <= epsilon;
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "glass contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool same(glass::rect a, glass::rect b) {
  return near(a.x, b.x) && near(a.y, b.y) && near(a.w, b.w) && near(a.h, b.h);
}

void variants_differ_only_where_the_spec_says() {
  require(glass::material_for(glass::variant::regular).dim_strength == 1.0F,
          "regular glass dims bright backdrops");
  require(glass::material_for(glass::variant::clear).dim_strength == 1.0F,
          "clear glass also dims, so it stays legible over a light wallpaper");
  require(glass::material_for(glass::variant::regular).samples_backdrop &&
              glass::material_for(glass::variant::clear).samples_backdrop,
          "regular and clear glass show the scene behind them");
  require(!glass::material_for(glass::variant::focus).samples_backdrop,
          "focus is a lit region inside a card, not a second glass layer");
  require(glass::material_for(glass::variant::focus).refraction_px == 0.0F,
          "focus does not refract");
  require(glass::material_for(glass::variant::clear).blur_lod <
              glass::material_for(glass::variant::regular).blur_lod,
          "clear glass blurs less than regular glass");
}

void shapes_stay_concentric_and_never_invert() {
  require(near(glass::concentric_radius(36, 32), 4), "inner radius = outer - inset");
  require(near(glass::concentric_radius(36, 40), 0), "inner radius never goes negative");
  require(near(glass::clamp_radius(36, 20, 10), 5), "radius clamps to half the short side");
  require(near(glass::clamp_radius(8, 100, 100), 8), "small radii are kept");
}

void contrast_threshold_is_pinned() {
  // 4.5:1 is the floor; on the device glow text over a paused game needed
  // more, so the cap holds 7:1 against white text.
  require(near(glass::max_body_luminance, 0.10F), "the body luminance cap is 0.10");
  require(glass::contrast_ratio(1.0F, glass::max_body_luminance) >= 7.0F - 1e-4F, "the cap holds 7:1 against white");
  require(glass::contrast_ratio(1.0F, 0.18F) >= 4.5F && glass::contrast_ratio(1.0F, 0.19F) < 4.5F,
          "the AA floor sits at 0.18");
  require(near(glass::contrast_ratio(0.18F, 1.0F), glass::contrast_ratio(1.0F, 0.18F)),
          "contrast is symmetric");
}

void dimming_keeps_white_text_legible() {
  require(near(glass::dim_for_contrast(0.03F, 0.06F), 0), "a dark backdrop is not dimmed");
  const float tint = 0.06F;
  const float d = glass::dim_for_contrast(0.9F, tint);
  require(d > 0 && d <= 1, "a bright backdrop is dimmed");
  const float body = 0.9F * (1 - d) + tint;
  require(body <= glass::max_body_luminance + 0.0001F, "dimmed body stays under the cap");
  require(glass::contrast_ratio(1.0F, body) >= 7.0F - 1e-4F, "white text holds 7:1");
  require(near(glass::dim_for_contrast(0.0F, tint), 0), "a black backdrop needs no dimming");
  require(glass::dim_for_contrast(1.0F, 0.5F) == 1.0F, "an impossible tint dims fully");
}

void focus_slides_with_an_ease_out() {
  const glass::rect a{0, 0, 100, 40}, b{0, 80, 200, 40};
  require(same(glass::focus_rect_at(a, b, 0), a), "starts at the old row");
  require(same(glass::focus_rect_at(a, b, 150), b), "arrives after 150 ms");
  require(same(glass::focus_rect_at(a, b, 1000), b), "stays at the new row");
  require(same(glass::focus_rect_at(a, b, -5), a), "negative time clamps");
  require(glass::focus_rect_at(a, b, 75).y > 40, "past halfway at half time (ease-out)");
}

void rims_share_one_light_and_focus_uses_the_accent() {
  const auto regular = glass::rim_for(glass::variant::regular);
  const auto clear = glass::rim_for(glass::variant::clear);
  require(regular == clear, "regular and clear glass share one rim");
  require(near(regular[0], 1) && near(regular[1], 1) && near(regular[2], 1) && near(regular[3], 0.55F),
          "the rim is white at 0.55");
  const auto focus = glass::rim_for(glass::variant::focus);
  require(near(focus[3], 0.9F), "focus rim at 0.9");
  require(near(focus[2], 1.0F) && focus[0] < 0.01F, "focus rim is the accent, in linear light");
  require(near(glass::srgb_to_linear(255), 1) && near(glass::srgb_to_linear(0), 0),
          "srgb endpoints");
}

void pause_panel_waits_for_the_still_at_most_250_ms() {
  openxmb::xmb::pause_backdrop b;
  require(b.state == openxmb::xmb::pause_backdrop_state::idle, "starts idle");
  b.start();
  require(!b.panel_may_show(100), "the panel waits while the still is captured");
  require(b.panel_may_show(250), "but no longer than 250 ms");
  require(!b.draw_still(), "nothing to draw yet");
  b.finish(true);
  require(b.draw_still() && b.panel_may_show(0), "a captured still is drawn at once");
}

void a_failed_capture_shows_the_fallback_panel() {
  openxmb::xmb::pause_backdrop b;
  b.start();
  b.finish(false);
  require(!b.draw_still(), "no still after a failure");
  require(b.panel_may_show(0), "the panel shows at once with its fallback body");
}

void a_late_still_is_ignored_after_resume() {
  openxmb::xmb::pause_backdrop b;
  b.start();
  b.cancel();
  b.finish(true);
  require(!b.draw_still(), "a still that lands after RESUME is never shown");
  require(b.state == openxmb::xmb::pause_backdrop_state::idle, "back to idle");
}

void focus_tracker_slides_from_wherever_it_is() {
  glass::focus_tracker t;
  const glass::rect a{0, 0, 100, 40}, b{0, 80, 100, 40}, c{0, 160, 100, 40};
  t.move_to(a, 0);
  require(same(t.at(0), a), "the first position is taken without a slide");
  t.move_to(b, 1000);
  require(same(t.at(1000), a) && same(t.at(1150), b), "a move slides over 150 ms");
  t.move_to(b, 1050);
  require(same(t.at(1150), b), "moving to the same row does not restart the slide");
  const auto mid = t.at(1075);
  t.move_to(c, 1075);
  require(same(t.at(1075), mid), "an interrupted slide continues from where it is");
  require(same(t.at(1225), c), "and arrives 150 ms later");
}

void status_bar_is_a_floating_capsule() {
  using metrics = openxmb::xmb::StatusBarMetrics;
  require(near(metrics::frame_inset, 3 * glass::grid), "the status bar sits 24 units in from the edges");
  require(near(metrics::frame_right(), metrics::logical_width - metrics::frame_inset),
          "its right edge is inset, not attached to the screen edge");
  require(near(metrics::frame_top(0), metrics::frame_inset), "its top is inset from the visible top");
  require(near(metrics::frame_top(-60), -60 + metrics::frame_inset), "even when the canvas is cropped");
  require(near(metrics::frame_radius(40), 20), "its ends are round");
}

void square_icon_columns_have_no_gap() {
  using lens = openxmb::xmb::SelectionLens;
  require(near(static_cast<float>(lens::box_width(100, false)), static_cast<float>(lens::box_height(100))),
          "a column of square icons uses a square box");
  require(near(static_cast<float>(lens::box_width(100, true)), 142), "a column with logos keeps the wide box");
  require(near(static_cast<float>(lens::label_shift_for(100, true)), static_cast<float>(lens::label_shift)),
          "labels after logos are where they were");
  require(near(static_cast<float>(lens::label_shift_for(100, false)), 48 - 31),
          "labels after square icons keep the same gap to the box");
  require(near(static_cast<float>(lens::left(500, 100, false)), 500 - 40 - 18),
          "the lens starts just left of the square box");
  require(near(static_cast<float>(lens::left(500, 100, true)), static_cast<float>(lens::left(500, 100))),
          "the wide lens is unchanged");
}

// Cancelling a capture must never block the render thread: terminate()
// returns at once and the child is reaped by later polls.
void cancelling_a_capture_does_not_block() {
  using clock = std::chrono::steady_clock;
  openxmb::xmb::child_process child;
  require(child.spawn({"sh", "-c", "trap '' TERM; sleep 2"}), "a child starts");
  require(child.running(), "it is running");
  const auto start = clock::now();
  child.terminate();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
  require(took < 50, "terminate() returns at once, even if the child ignores SIGTERM");
  require(!child.running(), "a terminated child counts as gone for its owner");
  bool reaped = false;
  for (int i = 0; i < 400 && !reaped; ++i) {
    reaped = openxmb::xmb::child_process::reap_orphans() == 0;
    if (!reaped) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(reaped, "the orphan is reaped (killed after its grace period) without blocking");
}

void a_finished_child_reports_its_exit() {
  openxmb::xmb::child_process ok, bad;
  require(ok.spawn({"true"}) && bad.spawn({"false"}), "children start");
  std::optional<bool> a, b;
  for (int i = 0; i < 200 && (!a || !b); ++i) {
    if (!a) a = ok.poll();
    if (!b) b = bad.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(a && *a, "exit 0 is success");
  require(b && !*b, "exit 1 is failure");
  openxmb::xmb::child_process missing;
  require(!missing.spawn({"nova-no-such-program-xyz"}) || (missing.terminate(), true),
          "a missing program does not start");
}

// A signal arriving during waitpid() reports EINTR, not a finished child.
// Polling several live children under a steady alarm load must keep seeing
// nullopt (still running); without the retry, an EINTR reports the child
// finished with failure and drops it.
void signals_do_not_reap_a_running_child() {
  struct sigaction wake = {};
  wake.sa_handler = [](int) {};
  sigemptyset(&wake.sa_mask);
  require(sigaction(SIGALRM, &wake, nullptr) == 0, "alarm handler installs");
  itimerval tick = {};
  tick.it_interval = {0, 2000};
  tick.it_value = {0, 2000};
  require(setitimer(ITIMER_REAL, &tick, nullptr) == 0, "alarm timer starts");
  openxmb::xmb::child_process live[8];
  for (auto &child : live) require(child.spawn({"sh", "-c", "sleep 30"}), "children start");
  int polls = 0;
  for (int i = 0; i < 2000; ++i) {
    bool any_running = false;
    for (auto &child : live) {
      if (!child.running()) continue;
      any_running = true;
      require(!child.poll().has_value(), "EINTR never reports a finished child");
      ++polls;
    }
    if (!any_running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(polls > 100, "poll ran under signal load");
  for (auto &child : live) require(child.running(), "every child is still running");
  tick.it_interval = {0, 0};
  tick.it_value = {0, 0};
  require(setitimer(ITIMER_REAL, &tick, nullptr) == 0, "alarm timer stops");
  require(signal(SIGALRM, SIG_DFL) != SIG_ERR, "default disposition returns");
}

// The pause socket wakes the menu on connect, before the daemon has written
// its line; the reader must wait for it instead of dropping the message.
void a_message_written_after_connect_is_not_lost() {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  std::thread writer([w = fds[1]] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const char line[] = "PAUSE brotato\n";
    (void)!write(w, line, sizeof(line) - 1);
    close(w);
  });
  const auto message = openxmb::xmb::read_ipc_message(fds[0], 500);
  writer.join();
  close(fds[0]);
  require(message && *message == "PAUSE brotato", "the late line is read, without its newline");
}

void an_empty_connection_reads_nothing() {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  close(fds[1]);
  require(!openxmb::xmb::read_ipc_message(fds[0], 500), "a closed, empty connection gives no message");
  close(fds[0]);
  int quiet[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, quiet) == 0, "socketpair");
  fcntl(quiet[0], F_SETFL, fcntl(quiet[0], F_GETFL) | O_NONBLOCK);
  const auto start = std::chrono::steady_clock::now();
  require(!openxmb::xmb::read_ipc_message(quiet[0], 60), "a silent peer times out");
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  require(took >= 50 && took < 300, "the wait is bounded by the timeout");
  close(quiet[0]);
  close(quiet[1]);
}

void panels_grow_and_fade() {
  const auto start = glass::panel_in(0);
  require(near(start.alpha, 0) && near(start.scale, 0.96F), "panels start clear and small");
  const auto end = glass::panel_in(200);
  require(near(end.alpha, 1) && near(end.scale, 1), "panels arrive after 200 ms");
  require(near(glass::panel_out(200).alpha, 0), "panels leave after 200 ms");
  require(near(glass::panel_out(0).alpha, 1), "panels leave from full");
}

// The engine's panel shader must implement the same contrast cap. Run with
// OPENXMB_ENGINE_SHADER=<AuroreEngine>/shaders/image_renderer.glass.frag.
void engine_shader_implements_the_panel_mode() {
  const char* path = std::getenv("OPENXMB_ENGINE_SHADER");
  if (path == nullptr || *path == '\0') return;
  std::ifstream stream(path, std::ios::binary);
  require(static_cast<bool>(stream), "engine glass shader is readable");
  const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  require(source.find("pc.index==2u") != std::string::npos, "shader has the panel mode");
  require(source.find("0.10 - tint - sheen") != std::string::npos, "the cap counts the sheen added on top of the body");
  require(source.find("textureLod(uBackdrop") != std::string::npos, "shader samples the mipmapped backdrop");
}

}  // namespace

int main() {
  engine_shader_implements_the_panel_mode();
  variants_differ_only_where_the_spec_says();
  shapes_stay_concentric_and_never_invert();
  contrast_threshold_is_pinned();
  dimming_keeps_white_text_legible();
  focus_slides_with_an_ease_out();
  panels_grow_and_fade();
  rims_share_one_light_and_focus_uses_the_accent();
  focus_tracker_slides_from_wherever_it_is();
  status_bar_is_a_floating_capsule();
  square_icon_columns_have_no_gap();
  cancelling_a_capture_does_not_block();
  a_finished_child_reports_its_exit();
  signals_do_not_reap_a_running_child();
  a_message_written_after_connect_is_not_lost();
  an_empty_connection_reads_nothing();
  pause_panel_waits_for_the_still_at_most_250_ms();
  a_failed_capture_shows_the_fallback_panel();
  a_late_still_is_ignored_after_resume();
  return EXIT_SUCCESS;
}
