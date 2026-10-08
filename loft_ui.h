#pragma once
// Screen-side state for olimpia-lcd-loft.yaml's second page: the activity log
// and the room-temperature history drawn as an LVGL line.
//
// This is display bookkeeping only. Nothing here talks to the fan coil, and
// none of it survives a reboot - it is a "what happened recently" view, not a
// record. Home Assistant keeps the real history.

#include <cmath>
#include <deque>
#include <string>

#include "lvgl.h"

namespace loft {

// ---- activity log ----------------------------------------------------------

static const size_t LOG_MAX = 40;

struct Event {
  std::string when;
  std::string what;
};

inline std::deque<Event> &events() {
  static std::deque<Event> e;
  return e;
}

inline void log_event(const std::string &when, const std::string &what) {
  auto &e = events();
  e.push_front({when, what});
  while (e.size() > LOG_MAX)
    e.pop_back();
}

// Newest first, one line per event.
inline std::string events_text() {
  auto &e = events();
  if (e.empty())
    return "Nothing yet";
  std::string out;
  for (size_t i = 0; i < e.size(); i++) {
    if (i)
      out += "\n";
    out += e[i].when;
    out += "   ";
    out += e[i].what;
  }
  return out;
}

// ---- room temperature history ----------------------------------------------
// One sample every 5 min, 72 samples = the last 6 h. Ring buffer; NaN marks a
// sample taken while the coil was offline and is skipped when drawing.

static const int HIST_N = 72;

struct History {
  float room[HIST_N];
  float setpoint[HIST_N];
  int count = 0;  // valid samples, <= HIST_N
  int next = 0;   // write position
};

inline History &history() {
  static History h;
  return h;
}

inline void hist_push(float room, float setpoint) {
  auto &h = history();
  h.room[h.next] = room;
  h.setpoint[h.next] = setpoint;
  h.next = (h.next + 1) % HIST_N;
  if (h.count < HIST_N)
    h.count++;
}

// Redraws both lines inside a w x h box and writes the y-axis range into the
// two labels. The point arrays must outlive the call - LVGL keeps the pointer.
inline void hist_draw(lv_obj_t *line_room, lv_obj_t *line_sp, lv_obj_t *lbl_top, lv_obj_t *lbl_bottom,
                      int w, int h) {
  static lv_point_precise_t pts_room[HIST_N];
  static lv_point_precise_t pts_sp[HIST_N];
  auto &hist = history();

  float lo = INFINITY, hi = -INFINITY;
  for (int i = 0; i < hist.count; i++) {
    for (float v : {hist.room[i], hist.setpoint[i]}) {
      if (std::isnan(v))
        continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  if (!std::isfinite(lo)) {
    lv_line_set_points(line_room, pts_room, 0);
    lv_line_set_points(line_sp, pts_sp, 0);
    lv_label_set_text(lbl_top, "");
    lv_label_set_text(lbl_bottom, "");
    return;
  }
  // Never zoom in closer than 2 degC, or sensor noise looks like weather.
  float mid = (lo + hi) / 2.0f;
  float span = std::max(hi - lo, 2.0f) * 1.15f;
  lo = mid - span / 2.0f;
  hi = mid + span / 2.0f;

  // Oldest sample sits on the left edge of a full 6 h window, so a fresh
  // boot draws a short line on the right rather than stretching 10 minutes
  // across the whole card.
  int start = (hist.next - hist.count + HIST_N) % HIST_N;
  int n_room = 0, n_sp = 0;
  for (int i = 0; i < hist.count; i++) {
    int idx = (start + i) % HIST_N;
    float x = (float) (HIST_N - hist.count + i) * (float) (w - 1) / (float) (HIST_N - 1);
    float r = hist.room[idx], s = hist.setpoint[idx];
    if (!std::isnan(r))
      pts_room[n_room++] = {(lv_value_precise_t) x, (lv_value_precise_t) ((hi - r) / (hi - lo) * (float) (h - 1))};
    if (!std::isnan(s))
      pts_sp[n_sp++] = {(lv_value_precise_t) x, (lv_value_precise_t) ((hi - s) / (hi - lo) * (float) (h - 1))};
  }
  lv_line_set_points(line_room, pts_room, n_room);
  lv_line_set_points(line_sp, pts_sp, n_sp);

  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f°", hi);
  lv_label_set_text(lbl_top, buf);
  snprintf(buf, sizeof(buf), "%.1f°", lo);
  lv_label_set_text(lbl_bottom, buf);
}

}  // namespace loft
