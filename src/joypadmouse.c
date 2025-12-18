#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
  (void) sig;
  g_stop = 1;
}

static void ignore_sigchld(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGCHLD, &sa, NULL);
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long) (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static int emit_event(int fd, uint16_t type, uint16_t code, int32_t value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.code = code;
  ev.value = value;
  return write(fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev) ? 0 : -1;
}

static int emit_syn(int fd) {
  return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static double norm_axis(int v, int deadzone) {
  int av = v < 0 ? -v : v;
  if (av <= deadzone) {
    return 0.0;
  }
  if (deadzone >= 32767) {
    return 0.0;
  }
  double sign = v < 0 ? -1.0 : 1.0;
  return sign * ((double) (av - deadzone) / (double) (32767 - deadzone));
}

static void usage(const char *argv0) {
  fprintf(
    stderr,
    "Usage: %s [--device auto|/dev/input/jsN] [--mouse-toggle start+lb|start|start+rb|lb+rb] [--hold-ms 4000] [--speed 300] [--wheel-rate 4.5] [--deadzone 8000] [--poll-hz 125]\n"
    "\n"
    "Mouse mode toggle (default): hold START+LB for hold-ms.\n"
    "MangoHud toggle HUD: LB + RB + START (sends Shift_R+F12).\n"
    "Mapping (Xbox-style): LS=move, RS-Y=wheel, A=left click, B=right click, X=middle click, LB=slow, RB=fast.\n",
    argv0
  );
}

static int create_uinput_mouse(const char *name) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) {
    close(fd);
    return -1;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
      ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
      ioctl(fd, UI_SET_RELBIT, REL_Y) < 0 ||
      ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0) {
    close(fd);
    return -1;
  }

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof(uidev));
  snprintf(uidev.name, sizeof(uidev.name), "%s", name);
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor = 0x0001;
  uidev.id.product = 0x0001;
  uidev.id.version = 1;

  if (write(fd, &uidev, sizeof(uidev)) < 0) {
    close(fd);
    return -1;
  }

  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static int create_uinput_keyboard(const char *name) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTSHIFT) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, KEY_F12) < 0) {
    close(fd);
    return -1;
  }

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof(uidev));
  snprintf(uidev.name, sizeof(uidev.name), "%s", name);
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor = 0x0001;
  uidev.id.product = 0x0002;
  uidev.id.version = 1;

  if (write(fd, &uidev, sizeof(uidev)) < 0) {
    close(fd);
    return -1;
  }

  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void destroy_uinput(int fd) {
  if (fd >= 0) {
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
  }
}

static bool looks_like_gamepad(const char *name, unsigned char axes, unsigned char buttons) {
  if (buttons < 4 || axes < 2) {
    return false;
  }
  if (name && strstr(name, "Mouse passthrough") != NULL) {
    return false;
  }
  return true;
}

static int open_joystick_auto(char *opened_path, size_t opened_path_len) {
  for (;;) {
    int best_fd = -1;
    unsigned char best_axes = 0;
    unsigned char best_buttons = 0;
    char best_path[64];
    best_path[0] = '\0';

    for (int idx = 0; idx < 32; idx++) {
      char path[64];
      snprintf(path, sizeof(path), "/dev/input/js%d", idx);

      int fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) {
        continue;
      }

      unsigned char axes = 0;
      unsigned char buttons = 0;
      char name[128];
      memset(name, 0, sizeof(name));
      ioctl(fd, JSIOCGAXES, &axes);
      ioctl(fd, JSIOCGBUTTONS, &buttons);
      ioctl(fd, JSIOCGNAME(sizeof(name)), name);

      if (!looks_like_gamepad(name, axes, buttons)) {
        close(fd);
        continue;
      }

      if (best_fd < 0 ||
          buttons > best_buttons ||
          (buttons == best_buttons && axes > best_axes)) {
        if (best_fd >= 0) {
          close(best_fd);
        }
        best_fd = fd;
        best_axes = axes;
        best_buttons = buttons;
        snprintf(best_path, sizeof(best_path), "%s", path);
      } else {
        close(fd);
      }
    }

    if (best_fd >= 0) {
      if (opened_path && opened_path_len > 0) {
        snprintf(opened_path, opened_path_len, "%s", best_path);
      }
      return best_fd;
    }

    if (g_stop) {
      return -1;
    }
    sleep_ms(250);
  }
}

static int open_joystick_path_blocking(const char *path) {
  for (;;) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      return fd;
    }
    if (errno != ENOENT && errno != ENODEV) {
      return -1;
    }
    if (g_stop) {
      return -1;
    }
    sleep_ms(250);
  }
}

static int open_joystick_blocking(const char *device, char *opened_path, size_t opened_path_len) {
  if (device && !strcmp(device, "auto")) {
    return open_joystick_auto(opened_path, opened_path_len);
  }
  if (opened_path && opened_path_len > 0) {
    snprintf(opened_path, opened_path_len, "%s", device);
  }
  return open_joystick_path_blocking(device);
}

static void notify_toggle(bool mouse_mode, int hold_ms) {
  pid_t pid = fork();
  if (pid != 0) {
    return;
  }

  const char *bus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (!bus_addr || !*bus_addr) {
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    char addr[256];
    if (xdg_runtime && *xdg_runtime) {
      snprintf(addr, sizeof(addr), "unix:path=%s/bus", xdg_runtime);
    } else {
      snprintf(addr, sizeof(addr), "unix:path=/run/user/%d/bus", (int) getuid());
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
  }

  char msg[128];
  if (mouse_mode) {
    (void) hold_ms;
    snprintf(msg, sizeof(msg), "Mouse mode: ON");
  } else {
    snprintf(msg, sizeof(msg), "Mouse mode: OFF");
  }

  char *const argv[] = {
    "notify-send",
    "-t",
    "1500",
    "joypadmouse",
    msg,
    NULL
  };
  execvp("notify-send", argv);
  _exit(0);
}

static void send_key_combo(int fd, uint16_t modifier_key, uint16_t key) {
  if (fd < 0) {
    return;
  }

  emit_event(fd, EV_KEY, modifier_key, 1);
  emit_event(fd, EV_KEY, key, 1);
  emit_syn(fd);

  emit_event(fd, EV_KEY, key, 0);
  emit_event(fd, EV_KEY, modifier_key, 0);
  emit_syn(fd);
}

int main(int argc, char **argv) {
  const char *joy_path = "auto";
  const char *mouse_toggle = "start+lb";
  int hold_ms = 4000;
  int deadzone = 8000;
  int speed = 300;
  double wheel_rate = 4.5;
  int poll_hz = 125;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--device") && i + 1 < argc) {
      joy_path = argv[++i];
    } else if (!strcmp(argv[i], "--mouse-toggle") && i + 1 < argc) {
      mouse_toggle = argv[++i];
    } else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) {
      hold_ms = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--deadzone") && i + 1 < argc) {
      deadzone = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
      speed = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--wheel-rate") && i + 1 < argc) {
      wheel_rate = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--poll-hz") && i + 1 < argc) {
      poll_hz = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  bool toggle_requires_start = false;
  bool toggle_requires_lb = false;
  bool toggle_requires_rb = false;
  bool toggle_requires_back = false;

  if (!strcmp(mouse_toggle, "start+lb") || !strcmp(mouse_toggle, "lb+start")) {
    toggle_requires_start = true;
    toggle_requires_lb = true;
  } else if (!strcmp(mouse_toggle, "start")) {
    toggle_requires_start = true;
  } else if (!strcmp(mouse_toggle, "start+rb") || !strcmp(mouse_toggle, "rb+start")) {
    toggle_requires_start = true;
    toggle_requires_rb = true;
  } else if (!strcmp(mouse_toggle, "lb+rb") || !strcmp(mouse_toggle, "rb+lb")) {
    toggle_requires_lb = true;
    toggle_requires_rb = true;
  } else if (!strcmp(mouse_toggle, "start+back") || !strcmp(mouse_toggle, "back+start") || !strcmp(mouse_toggle, "start+select") ||
             !strcmp(mouse_toggle, "select+start")) {
    toggle_requires_start = true;
    toggle_requires_back = true;
  } else {
    fprintf(stderr, "joypadmouse: invalid --mouse-toggle value: %s\n", mouse_toggle);
    usage(argv[0]);
    return 2;
  }

  if (hold_ms < 0) {
    hold_ms = 0;
  }
  if (deadzone < 0) {
    deadzone = 0;
  }
  if (deadzone > 32767) {
    deadzone = 32767;
  }
  if (speed < 0) {
    speed = 0;
  }
  if (wheel_rate < 0.0) {
    wheel_rate = 0.0;
  }
  if (poll_hz < 10) {
    poll_hz = 10;
  }
  if (poll_hz > 1000) {
    poll_hz = 1000;
  }

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  ignore_sigchld();

  int ufd = create_uinput_mouse("joypadmouse");
  if (ufd < 0) {
    fprintf(stderr, "joypadmouse: can't open/create /dev/uinput (%s)\n", strerror(errno));
    return 1;
  }

  int kfd = create_uinput_keyboard("joypadmouse hotkeys");
  if (kfd < 0) {
    fprintf(stderr, "joypadmouse: warning: can't create uinput keyboard, MangoHud hotkeys disabled (%s)\n", strerror(errno));
  }

  char opened_path[64];
  memset(opened_path, 0, sizeof(opened_path));
  int jfd = open_joystick_blocking(joy_path, opened_path, sizeof(opened_path));
  if (jfd < 0) {
    fprintf(stderr, "joypadmouse: can't open joystick %s (%s)\n", joy_path, strerror(errno));
    destroy_uinput(ufd);
    return 1;
  }

  unsigned char axes_count = 0;
  unsigned char buttons_count = 0;
  char name[128];
  memset(name, 0, sizeof(name));
  ioctl(jfd, JSIOCGAXES, &axes_count);
  ioctl(jfd, JSIOCGBUTTONS, &buttons_count);
  ioctl(jfd, JSIOCGNAME(sizeof(name)), name);
  fprintf(
    stderr,
    "joypadmouse: joystick=%s axes=%u buttons=%u name=%s\n",
    opened_path[0] ? opened_path : joy_path,
    axes_count,
    buttons_count,
    name
  );

  int16_t axes[32];
  uint8_t buttons[32];
  uint8_t prev_buttons[32];
  memset(axes, 0, sizeof(axes));
  memset(buttons, 0, sizeof(buttons));
  memset(prev_buttons, 0, sizeof(prev_buttons));

  const int axis_lx = 0;
  const int axis_ly = 1;
  const int axis_ry = 4;

  const int btn_a = 0;
  const int btn_b = 1;
  const int btn_x = 2;
  const int btn_lb = 4;
  const int btn_rb = 5;
  const int btn_back = 6;
  const int btn_start = 7;

  bool mouse_mode = false;
  int64_t start_down_at = -1;
  bool start_consumed = false;

  double dx_acc = 0.0;
  double dy_acc = 0.0;
  double wheel_acc = 0.0;

  const double slow_mult = 0.35;
  const double fast_mult = 2.00;

  const int sleep_us = 1000000 / poll_hz;

  while (!g_stop) {
    struct js_event e;
    for (;;) {
      ssize_t r = read(jfd, &e, sizeof(e));
      if (r == (ssize_t) sizeof(e)) {
        uint8_t type = e.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_AXIS) {
          if (e.number < (uint8_t) (sizeof(axes) / sizeof(axes[0]))) {
            axes[e.number] = e.value;
          }
        } else if (type == JS_EVENT_BUTTON) {
          if (e.number < (uint8_t) (sizeof(buttons) / sizeof(buttons[0]))) {
            buttons[e.number] = e.value ? 1 : 0;
          }
        }
        continue;
      }

      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }

      if (r < 0 && (errno == ENODEV || errno == EIO)) {
        close(jfd);
        memset(opened_path, 0, sizeof(opened_path));
        jfd = open_joystick_blocking(joy_path, opened_path, sizeof(opened_path));
        if (jfd < 0) {
          g_stop = 1;
        } else {
          memset(name, 0, sizeof(name));
          ioctl(jfd, JSIOCGAXES, &axes_count);
          ioctl(jfd, JSIOCGBUTTONS, &buttons_count);
          ioctl(jfd, JSIOCGNAME(sizeof(name)), name);
          fprintf(
            stderr,
            "joypadmouse: joystick=%s axes=%u buttons=%u name=%s\n",
            opened_path[0] ? opened_path : joy_path,
            axes_count,
            buttons_count,
            name
          );
        }
        break;
      }

      if (r == 0) {
        break;
      }
      break;
    }

    bool modifiers = false;
    if (btn_lb < (int) sizeof(buttons) && btn_rb < (int) sizeof(buttons)) {
      modifiers = buttons[btn_lb] && buttons[btn_rb];
    }

    if (kfd >= 0 && modifiers && btn_start < (int) sizeof(buttons) &&
        buttons[btn_start] && !prev_buttons[btn_start]) {
      send_key_combo(kfd, KEY_RIGHTSHIFT, KEY_F12);
      start_down_at = -1;
      start_consumed = true;
    }

    bool toggle_chord_active = true;
    if (toggle_requires_start) {
      toggle_chord_active = toggle_chord_active && btn_start < (int) sizeof(buttons) && buttons[btn_start];
    }
    if (toggle_requires_lb) {
      toggle_chord_active = toggle_chord_active && btn_lb < (int) sizeof(buttons) && buttons[btn_lb];
    }
    if (toggle_requires_rb) {
      toggle_chord_active = toggle_chord_active && btn_rb < (int) sizeof(buttons) && buttons[btn_rb];
    }
    if (toggle_requires_back) {
      toggle_chord_active = toggle_chord_active && btn_back < (int) sizeof(buttons) && buttons[btn_back];
    }

    if (toggle_chord_active) {
      if (modifiers) {
        // Used for hotkeys, don't treat this as the mouse toggle.
        start_down_at = -1;
      } else if (start_down_at < 0) {
        start_down_at = now_ms();
        start_consumed = false;
      } else if (!start_consumed && (now_ms() - start_down_at) >= hold_ms) {
        mouse_mode = !mouse_mode;
        start_consumed = true;
        fprintf(stderr, "joypadmouse: mouse_mode=%s\n", mouse_mode ? "on" : "off");
        notify_toggle(mouse_mode, hold_ms);

        if (!mouse_mode) {
          emit_event(ufd, EV_KEY, BTN_LEFT, 0);
          emit_event(ufd, EV_KEY, BTN_RIGHT, 0);
          emit_event(ufd, EV_KEY, BTN_MIDDLE, 0);
          emit_syn(ufd);
        }

        memcpy(prev_buttons, buttons, sizeof(prev_buttons));
        dx_acc = dy_acc = wheel_acc = 0.0;
      }
    } else {
      start_down_at = -1;
      start_consumed = false;
    }

    if (mouse_mode) {
      double mult = 1.0;
      if (btn_lb < (int) sizeof(buttons) && buttons[btn_lb]) {
        mult = slow_mult;
      } else if (btn_rb < (int) sizeof(buttons) && buttons[btn_rb]) {
        mult = fast_mult;
      }

      double lx = axis_lx < (int) (sizeof(axes) / sizeof(axes[0])) ? norm_axis(axes[axis_lx], deadzone) : 0.0;
      double ly = axis_ly < (int) (sizeof(axes) / sizeof(axes[0])) ? norm_axis(axes[axis_ly], deadzone) : 0.0;
      double ry = axis_ry < (int) (sizeof(axes) / sizeof(axes[0])) ? norm_axis(axes[axis_ry], deadzone) : 0.0;

      dx_acc += lx * (double) speed * mult / (double) poll_hz;
      dy_acc += ly * (double) speed * mult / (double) poll_hz;
      wheel_acc += -ry * wheel_rate / (double) poll_hz;

      int dx = (int) dx_acc;
      int dy = (int) dy_acc;
      if (dx != 0) {
        dx_acc -= (double) dx;
      }
      if (dy != 0) {
        dy_acc -= (double) dy;
      }

      if (dx != 0 || dy != 0) {
        emit_event(ufd, EV_REL, REL_X, dx);
        emit_event(ufd, EV_REL, REL_Y, dy);
        emit_syn(ufd);
      }

      int wheel_steps = (int) wheel_acc;
      if (wheel_steps != 0) {
        wheel_acc -= (double) wheel_steps;
        emit_event(ufd, EV_REL, REL_WHEEL, wheel_steps);
        emit_syn(ufd);
      }

      if (btn_a < (int) sizeof(buttons) && buttons[btn_a] != prev_buttons[btn_a]) {
        emit_event(ufd, EV_KEY, BTN_LEFT, buttons[btn_a] ? 1 : 0);
        emit_syn(ufd);
      }
      if (btn_b < (int) sizeof(buttons) && buttons[btn_b] != prev_buttons[btn_b]) {
        emit_event(ufd, EV_KEY, BTN_RIGHT, buttons[btn_b] ? 1 : 0);
        emit_syn(ufd);
      }
      if (btn_x < (int) sizeof(buttons) && buttons[btn_x] != prev_buttons[btn_x]) {
        emit_event(ufd, EV_KEY, BTN_MIDDLE, buttons[btn_x] ? 1 : 0);
        emit_syn(ufd);
      }
    }

    memcpy(prev_buttons, buttons, sizeof(prev_buttons));

    if (sleep_us > 0) {
      usleep((useconds_t) sleep_us);
    }
  }

  close(jfd);
  destroy_uinput(ufd);
  destroy_uinput(kfd);
  return 0;
}
