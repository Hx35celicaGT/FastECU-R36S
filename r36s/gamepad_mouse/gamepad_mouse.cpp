// gamepad_mouse.cpp
// R36S "GO-Super Gamepad" -> virtual mouse + virtual keyboard (via /dev/uinput)
// so FastECU (Qt, linuxfb) can be driven with the handheld's controls.
//
// Build:  g++ -O2 -std=c++17 -o gamepad_mouse gamepad_mouse.cpp
// Run:    ./gamepad_mouse [options]
//   --device PATH    gamepad event device (default /dev/input/event3)
//   --speed N        cursor speed in pixels/sec at full stick (default 400)
//   --deadzone F     stick deadzone 0..0.9 (default 0.15)
//   --swap-sticks    use RIGHT stick for cursor, LEFT stick for scrolling
//   --invert-x / --invert-y
//   --no-grab        don't take exclusive control of the gamepad
//   --debug          print every button press and stick position
// Quit:   hold SELECT + START together, or Ctrl+C.

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

enum class Action { MouseLeft, MouseRight, MouseMiddle, Key, ShiftTab };

// key   = the key to send
// ctrl  = hold Ctrl while sending it (Ctrl+L, Ctrl+H ...)
// tap   = send press+release at once when the button goes down, so holding the
//         button can never repeat it (used for on/off actions like Logging)
struct Mapping { Action action; int key; bool ctrl = false; bool tap = false; };

// FN (BTN_MODE) works like a second layer: while FN is held, buttons use kFnMap.
// FN on its own sends nothing.
bool g_fnHeld = false;

// ====================== BUTTON MAPPING (edit here, then rebuild) ======================
// Buttons on their own
const std::map<int, Mapping> kButtonMap = {
    { BTN_EAST,       { Action::MouseLeft,   0 } },                       // A: left click
    { BTN_SOUTH,      { Action::Key,         KEY_ESC } },                 // B: cancel / close dialog
    { BTN_NORTH,      { Action::MouseRight,  0 } },                       // X: right click
    { BTN_WEST,       { Action::Key,         KEY_ENTER } },               // Y: OK / confirm
    { BTN_TL,         { Action::ShiftTab,    0 } },                       // L1: previous field / view
    { BTN_TR,         { Action::Key,         KEY_TAB } },                 // R1: next field / view
    { BTN_TL2,        { Action::Key,         KEY_KPMINUS, true } },       // L2: Ctrl+-  fine step down
    { BTN_TR2,        { Action::Key,         KEY_KPPLUS,  true } },       // R2: Ctrl++  fine step up
    { BTN_SELECT,     { Action::Key,         KEY_SPACE } },               // toggle checkbox/button
    { BTN_START,      { Action::Key,         KEY_SPACE, false, true } },  // Space: Logging = 6 boxes on/off
    { BTN_THUMBL,     { Action::MouseLeft,   0 } },                       // L3 click
    { BTN_THUMBR,     { Action::MouseMiddle, 0 } },                       // R3 click
    { BTN_DPAD_UP,    { Action::Key,         KEY_UP } },
    { BTN_DPAD_DOWN,  { Action::Key,         KEY_DOWN } },
    { BTN_DPAD_LEFT,  { Action::Key,         KEY_LEFT } },
    { BTN_DPAD_RIGHT, { Action::Key,         KEY_RIGHT } },
};

// Buttons while FN is held (buttons not listed here do nothing while FN is held)
const std::map<int, Mapping> kFnMap = {
    { BTN_START,      { Action::Key, KEY_L,       true,  true } },   // Ctrl+L: Log to file (CSV) on/off
    { BTN_TR2,        { Action::Key, KEY_KPPLUS } },                 // +  coarse step up
    { BTN_TL2,        { Action::Key, KEY_KPMINUS } },                // -  coarse step down
    { BTN_EAST,       { Action::Key, KEY_S,       false, true } },   // A: S      Set value
    { BTN_SOUTH,      { Action::Key, KEY_I,       true,  true } },   // B: Ctrl+I interpolate both ways
    { BTN_NORTH,      { Action::Key, KEY_H,       true,  true } },   // X: Ctrl+H interpolate horizontal
    { BTN_WEST,       { Action::Key, KEY_J,       true,  true } },   // Y: Ctrl+J interpolate vertical
    { BTN_TR,         { Action::Key, KEY_F3,      false, true } },   // R1: F3 Connect
    { BTN_TL,         { Action::Key, KEY_F4,      false, true } },   // L1: F4 Disconnect
    { BTN_DPAD_UP,    { Action::Key, KEY_F6,      false, true } },   // D-pad up:    F6 Send to 2D/3D graph
    { BTN_DPAD_LEFT,  { Action::Key, KEY_F7,      false, true } },   // D-pad left:  F7 Send X axis to 2D graph
    { BTN_DPAD_RIGHT, { Action::Key, KEY_F8,      false, true } },   // D-pad right: F8 Send Y axis to 2D graph
    { BTN_DPAD_DOWN,  { Action::Key, KEY_F9,      false, true } },   // D-pad down:  F9 Copy Table
};
// =======================================================================================

// What each held button actually sent, so letting go of FN first can't leave a key stuck
std::map<int, Mapping> g_sent;


struct Settings {
    std::string device = "/dev/input/event3";
    float speed = 400.0f;     // pixels per second at full tilt
    float deadzone = 0.15f;
    float wheelRate = 10.0f;  // wheel notches per second at full tilt
    bool debug = false, invertX = false, invertY = false, grab = true, swapSticks = false;
};

struct Axis {
    int min = 0, max = 0, center = 0, raw = 0;
    bool valid = false;

    // Returns -1..1 after deadzone, with a squared curve for fine control near center.
    float normalized(float dz) const {
        if (!valid) return 0.0f;
        float n = 0.0f;
        if (raw >= center) {
            int span = max - center;
            n = span > 0 ? float(raw - center) / float(span) : 0.0f;
        } else {
            int span = center - min;
            n = span > 0 ? -float(center - raw) / float(span) : 0.0f;
        }
        if (n > 1.0f) n = 1.0f;
        if (n < -1.0f) n = -1.0f;
        float a = std::fabs(n);
        if (a < dz) return 0.0f;
        a = (a - dz) / (1.0f - dz);
        return std::copysign(a * a, n);
    }
};

Axis readAxis(int fd, int code) {
    Axis ax;
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) == 0 && info.maximum > info.minimum) {
        ax.min = info.minimum;
        ax.max = info.maximum;
        ax.raw = info.value;
        // Treat the resting position at startup as center (don't touch sticks while starting).
        ax.center = (info.value > info.minimum && info.value < info.maximum)
                        ? info.value
                        : (info.minimum + info.maximum) / 2;
        ax.valid = true;
    }
    return ax;
}

void emitEv(int fd, uint16_t type, uint16_t code, int32_t value) {
    input_event ie{};
    ie.type = type;
    ie.code = code;
    ie.value = value;
    if (write(fd, &ie, sizeof ie) < 0 && errno != EAGAIN) std::perror("uinput write");
}
void syn(int fd) { emitEv(fd, EV_SYN, SYN_REPORT, 0); }

bool finishDevice(int fd, const char* name, uint16_t product) {
    uinput_setup us{};
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x1209;
    us.id.product = product;
    us.id.version = 1;
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    return ioctl(fd, UI_DEV_SETUP, &us) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
}

int createMouse() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    if (!finishDevice(fd, "R36S Virtual Mouse", 0x0001)) { close(fd); return -1; }
    return fd;
}

int createKeyboard() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REP);  // kernel auto-repeat for held D-pad keys
    // Advertise a full standard key set so udev/Qt classify it as a real keyboard.
    for (int k = KEY_ESC; k <= KEY_MICMUTE; ++k) ioctl(fd, UI_SET_KEYBIT, k);
    if (!finishDevice(fd, "R36S Virtual Keyboard", 0x0002)) { close(fd); return -1; }
    return fd;
}

std::string eventNodeFor(int ufd) {
    char sys[64] = {0};
    if (ioctl(ufd, UI_GET_SYSNAME(sizeof sys), sys) < 0) return "?";
    std::string dir = std::string("/sys/devices/virtual/input/") + sys;
    std::string result = "?";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "event", 5) == 0) {
                result = std::string("/dev/input/") + e->d_name;
                break;
            }
        }
        closedir(d);
    }
    return result;
}

const char* btnName(int c) {
    switch (c) {
        case BTN_SOUTH: return "BTN_SOUTH";   case BTN_EAST: return "BTN_EAST";
        case BTN_NORTH: return "BTN_NORTH";   case BTN_WEST: return "BTN_WEST";
        case BTN_TL: return "BTN_TL (L1)";    case BTN_TR: return "BTN_TR (R1)";
        case BTN_TL2: return "BTN_TL2 (L2)";  case BTN_TR2: return "BTN_TR2 (R2)";
        case BTN_SELECT: return "BTN_SELECT"; case BTN_START: return "BTN_START";
        case BTN_MODE: return "BTN_MODE";     case BTN_THUMBL: return "BTN_THUMBL (L3)";
        case BTN_THUMBR: return "BTN_THUMBR (R3)";
        case BTN_DPAD_UP: return "DPAD_UP";   case BTN_DPAD_DOWN: return "DPAD_DOWN";
        case BTN_DPAD_LEFT: return "DPAD_LEFT"; case BTN_DPAD_RIGHT: return "DPAD_RIGHT";
        default: return "unknown";
    }
}

void sendMapping(const Mapping& m, int value, int mfd, int kfd) {
    switch (m.action) {
        case Action::MouseLeft:   emitEv(mfd, EV_KEY, BTN_LEFT, value);   syn(mfd); break;
        case Action::MouseRight:  emitEv(mfd, EV_KEY, BTN_RIGHT, value);  syn(mfd); break;
        case Action::MouseMiddle: emitEv(mfd, EV_KEY, BTN_MIDDLE, value); syn(mfd); break;
        case Action::Key:
            if (value) {
                if (m.ctrl) { emitEv(kfd, EV_KEY, KEY_LEFTCTRL, 1); syn(kfd); }
                emitEv(kfd, EV_KEY, m.key, 1); syn(kfd);
            } else {
                emitEv(kfd, EV_KEY, m.key, 0); syn(kfd);
                if (m.ctrl) { emitEv(kfd, EV_KEY, KEY_LEFTCTRL, 0); syn(kfd); }
            }
            break;
        case Action::ShiftTab:
            if (value) {
                emitEv(kfd, EV_KEY, KEY_LEFTSHIFT, 1); syn(kfd);
                emitEv(kfd, EV_KEY, KEY_TAB, 1);       syn(kfd);
            } else {
                emitEv(kfd, EV_KEY, KEY_TAB, 0);       syn(kfd);
                emitEv(kfd, EV_KEY, KEY_LEFTSHIFT, 0); syn(kfd);
            }
            break;
    }
}

void handleButton(int code, int value, int mfd, int kfd) {
    if (value) {
        // Button down: pick the FN layer or the normal layer
        const std::map<int, Mapping>& layer = g_fnHeld ? kFnMap : kButtonMap;
        auto it = layer.find(code);
        if (it == layer.end()) return;
        const Mapping& m = it->second;
        sendMapping(m, 1, mfd, kfd);
        if (m.tap)
            sendMapping(m, 0, mfd, kfd);   // release right away: no auto-repeat
        else
            g_sent[code] = m;              // remember it so the release matches
    } else {
        // Button up: release exactly what was pressed (even if FN changed meanwhile)
        auto it = g_sent.find(code);
        if (it == g_sent.end()) return;
        sendMapping(it->second, 0, mfd, kfd);
        g_sent.erase(it);
    }
}


void usage() {
    std::puts("Usage: gamepad_mouse [--device PATH] [--speed N] [--deadzone F] [--swap-sticks]\n"
              "                     [--invert-x] [--invert-y] [--no-grab] [--debug]");
}

} // namespace

int main(int argc, char** argv) {
    Settings s;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--device" && i + 1 < argc)        s.device = argv[++i];
        else if (a == "--speed" && i + 1 < argc)    s.speed = std::strtof(argv[++i], nullptr);
        else if (a == "--deadzone" && i + 1 < argc) s.deadzone = std::strtof(argv[++i], nullptr);
        else if (a == "--swap-sticks")              s.swapSticks = true;
        else if (a == "--invert-x")                 s.invertX = true;
        else if (a == "--invert-y")                 s.invertY = true;
        else if (a == "--no-grab")                  s.grab = false;
        else if (a == "--debug")                    s.debug = true;
        else if (a == "--help" || a == "-h")        { usage(); return 0; }
        else { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); usage(); return 1; }
    }
    if (s.deadzone < 0.0f) s.deadzone = 0.0f;
    if (s.deadzone > 0.9f) s.deadzone = 0.9f;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    int gfd = open(s.device.c_str(), O_RDONLY | O_NONBLOCK);
    if (gfd < 0) { std::perror(s.device.c_str()); return 1; }

    char name[128] = "?";
    ioctl(gfd, EVIOCGNAME(sizeof name), name);
    std::printf("Gamepad: %s (%s)\n", name, s.device.c_str());

    Axis lx = readAxis(gfd, ABS_X),  ly = readAxis(gfd, ABS_Y);
    Axis rx = readAxis(gfd, ABS_RX), ry = readAxis(gfd, ABS_RY);
    std::printf("Left stick  X: %d..%d center %d | Y: %d..%d center %d\n",
                lx.min, lx.max, lx.center, ly.min, ly.max, ly.center);
    std::printf("Right stick X: %d..%d center %d | Y: %d..%d center %d\n",
                rx.min, rx.max, rx.center, ry.min, ry.max, ry.center);

    int mfd = createMouse();
    int kfd = createKeyboard();
    if (mfd < 0 || kfd < 0) {
        std::perror("/dev/uinput");
        std::fputs("Could not create virtual devices. On the HOST run: modprobe uinput\n", stderr);
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let udev register them
    std::printf("Virtual mouse:    %s\n", eventNodeFor(mfd).c_str());
    std::printf("Virtual keyboard: %s\n", eventNodeFor(kfd).c_str());

    if (s.grab && ioctl(gfd, EVIOCGRAB, 1) < 0)
        std::perror("EVIOCGRAB (continuing without exclusive grab)");

    std::puts("Running. Hold SELECT + START to quit.");
    std::fflush(stdout);

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    auto lastDebug = last;
    float accX = 0, accY = 0, accW = 0;
    bool selectHeld = false, startHeld = false;

    while (g_running) {
        pollfd p{gfd, POLLIN, 0};
        int r = poll(&p, 1, 10);
        if (r < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }
        if (p.revents & (POLLERR | POLLHUP)) { std::fputs("Gamepad disconnected\n", stderr); break; }

        if (r > 0 && (p.revents & POLLIN)) {
            input_event ev;
            ssize_t n;
            while ((n = read(gfd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY) {
                    if (ev.value == 2) continue;  // ignore hardware repeats
                    if (s.debug)
                        std::printf("button %-16s code=%d %s\n", btnName(ev.code), ev.code,
                                    ev.value ? "DOWN" : "UP");
                    if (ev.code == BTN_SELECT) selectHeld = ev.value != 0;
                    if (ev.code == BTN_START)  startHeld = ev.value != 0;
                    if (selectHeld && startHeld) {
                        std::puts("SELECT + START held: quitting");
                        g_running = 0;
                        break;
                    }
                    if (ev.code == BTN_MODE) { g_fnHeld = ev.value != 0; continue; }  // FN = second layer
                    handleButton(ev.code, ev.value, mfd, kfd);
                } else if (ev.type == EV_ABS) {
                    switch (ev.code) {
                        case ABS_X:  lx.raw = ev.value; break;
                        case ABS_Y:  ly.raw = ev.value; break;
                        case ABS_RX: rx.raw = ev.value; break;
                        case ABS_RY: ry.raw = ev.value; break;
                        default: break;
                    }
                }
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) { std::perror("gamepad read"); break; }
            std::fflush(stdout);
        }

        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        const Axis& moveX = s.swapSticks ? rx : lx;
        const Axis& moveY = s.swapSticks ? ry : ly;
        const Axis& wheel = s.swapSticks ? ly : ry;

        float vx = moveX.normalized(s.deadzone);
        float vy = moveY.normalized(s.deadzone);
        if (s.invertX) vx = -vx;
        if (s.invertY) vy = -vy;

        if (vx == 0.0f) accX = 0; else accX += vx * s.speed * dt;
        if (vy == 0.0f) accY = 0; else accY += vy * s.speed * dt;
        int ix = (int)accX, iy = (int)accY;
        accX -= ix;
        accY -= iy;
        if (ix || iy) {
            if (ix) emitEv(mfd, EV_REL, REL_X, ix);
            if (iy) emitEv(mfd, EV_REL, REL_Y, iy);
            syn(mfd);
        }

        float w = -wheel.normalized(s.deadzone);  // stick up = scroll up
        if (w == 0.0f) accW = 0; else accW += w * s.wheelRate * dt;
        int iw = (int)accW;
        if (iw) {
            accW -= iw;
            emitEv(mfd, EV_REL, REL_WHEEL, iw);
            syn(mfd);
        }

        if (s.debug && (vx != 0 || vy != 0 || w != 0) &&
            std::chrono::duration<float>(now - lastDebug).count() > 0.5f) {
            std::printf("cursor stick %.2f,%.2f  scroll %.2f\n", vx, vy, w);
            std::fflush(stdout);
            lastDebug = now;
        }
    }

    if (s.grab) ioctl(gfd, EVIOCGRAB, 0);
    ioctl(mfd, UI_DEV_DESTROY);
    ioctl(kfd, UI_DEV_DESTROY);
    close(mfd);
    close(kfd);
    close(gfd);
    std::puts("Stopped.");
    return 0;
}
