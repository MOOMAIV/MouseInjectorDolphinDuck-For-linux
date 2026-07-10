//==========================================================================
// Mouse Injector – Linux Port
// Copyright (C) 2019-2020 Carnivorous / Linux port (C) 2024
//==========================================================================
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "mouse.h"
#include "./manymouse/manymouse.h"

#ifdef HAVE_X11
#  include <X11/Xlib.h>
#  ifdef Status
#    undef Status
#  endif
static Display *x11display = NULL;
static Window   x11root;
#endif

int32_t xmouse, ymouse;

#ifdef HAVE_X11
static int lock_x = 0, lock_y = 0;
#endif
static ManyMouseEvent mm_event;

/* -----------------------------------------------------------------------
 * Wayland detection
 * On pure Wayland, XWarpPointer does nothing for Wayland-native windows
 * (e.g. Dolphin's native Wayland window). We instead exclusively grab
 * the mouse evdev devices so the compositor stops receiving pointer
 * events — the desktop cursor simply can't move while grabbed.
 *
 * IMPORTANT: the grab MUST happen on the exact same fds that ManyMouse
 * itself is reading from (via ManyMouse_LinuxEvdevGrab, implemented in
 * linux_evdev.c) — NOT on separately opened fds to the same device
 * nodes. EVIOCGRAB is per file descriptor: grabbing through a second,
 * independently-opened fd to the same /dev/input/eventN silently cuts
 * off every OTHER fd to that device, including ManyMouse's own.
 * ----------------------------------------------------------------------- */
static int on_wayland      = 0;  /* WAYLAND_DISPLAY is set          */
static int grab_warned     = 0;  /* printed the Wayland notice once */
static int devices_grabbed = 0;

/* -----------------------------------------------------------------------
 * Mouse button / scroll wheel relay (Wayland only).
 *
 * EVIOCGRAB claims the ENTIRE device, not just motion — once grabbed,
 * left/right/middle click and the scroll wheel stop reaching anything
 * else too, including Dolphin's OWN input system. Dolphin binds mouse
 * buttons to GameCube controller inputs independently of this
 * injector (which only ever touches the look/aim angle via memory
 * writes) — so without forwarding, clicks and the wheel just vanish
 * system-wide while injection is active.
 *
 * Fix: create a virtual uinput device that exposes only buttons and
 * the scroll wheel (no motion axes, so the desktop cursor is
 * unaffected). Every BUTTON/SCROLL event ManyMouse reads from the
 * grabbed real device gets re-emitted on this virtual device, which
 * Dolphin can then bind to in its own Controller Settings, exactly
 * as it would bind to a normal mouse.
 * ----------------------------------------------------------------------- */
static int uinput_fd = -1;

static const int uinput_button_codes[] =
{
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK
};
#define NUM_UINPUT_BUTTONS \
    ((int)(sizeof(uinput_button_codes) / sizeof(uinput_button_codes[0])))

static void uinput_emit(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = (uint16_t)type;
    ev.code  = (uint16_t)code;
    ev.value = value;
    if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
        /* Non-fatal — device may have been removed; just drop the event. */
    }
}

static void uinput_init(void)
{
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        fprintf(stderr,
            "[mi] Note: /dev/uinput not accessible (%s).\n"
            "     Mouse buttons/wheel won't reach the emulator while\n"
            "     injection is active. See README_LINUX.md for the\n"
            "     one-time udev rule that fixes this.\n",
            strerror(errno));
        uinput_fd = -1;
        return;
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < NUM_UINPUT_BUTTONS; i++)
        ioctl(uinput_fd, UI_SET_KEYBIT, uinput_button_codes[i]);

    ioctl(uinput_fd, UI_SET_EVBIT, EV_REL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1209; /* pid.codes test/generic vendor ID */
    usetup.id.product = 0x4D49; /* "MI" */
    snprintf(usetup.name, sizeof(usetup.name), "Mouse Injector Button Relay");

    if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(uinput_fd, UI_DEV_CREATE) < 0)
    {
        fprintf(stderr, "[mi] Warning: failed to create virtual uinput device (%s).\n",
                strerror(errno));
        close(uinput_fd);
        uinput_fd = -1;
        return;
    }

    /* Let the device settle before anything tries to enumerate it */
    usleep(100000);

    fprintf(stderr,
        "[mi] Created virtual device \"Mouse Injector Button Relay\" —\n"
        "     bind Dolphin's Controller Settings to it for clicks/wheel.\n");
}

static void uinput_quit(void)
{
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
}

static void uinput_relay_button(int item, int pressed)
{
    if (uinput_fd < 0 || item < 0 || item >= NUM_UINPUT_BUTTONS) return;
    uinput_emit(EV_KEY, uinput_button_codes[item], pressed ? 1 : 0);
    uinput_emit(EV_SYN, SYN_REPORT, 0);
}

static void uinput_relay_scroll(int item, int value)
{
    if (uinput_fd < 0) return;
    uinput_emit(EV_REL, (item == 1) ? REL_HWHEEL : REL_WHEEL, value);
    uinput_emit(EV_SYN, SYN_REPORT, 0);
}

/* -----------------------------------------------------------------------
 * MOUSE_Init
 * ----------------------------------------------------------------------- */
uint8_t MOUSE_Init(void)
{
    const char *wayland_disp = getenv("WAYLAND_DISPLAY");
    on_wayland = (wayland_disp != NULL && wayland_disp[0] != '\0');

#ifdef HAVE_X11
    const char *x11_disp = getenv("DISPLAY");
    if (x11_disp && x11_disp[0] != '\0') {
        x11display = XOpenDisplay(NULL);
        if (x11display)
            x11root = XDefaultRootWindow(x11display);
    }
    if (!x11display && !on_wayland)
        fprintf(stderr, "[mi] Warning: no display detected (DISPLAY/WAYLAND_DISPLAY)\n");
#endif

    int mice = ManyMouse_Init();
    if (mice <= 0) {
        fprintf(stderr, "[mi] ManyMouse_Init failed – no mice found.\n"
                        "     Are you in the 'input' group?  "
                        "sudo usermod -aG input $USER\n");
        return 0;
    }

    fprintf(stderr, "[mi] ManyMouse: %d device(s) via %s\n",
            mice, ManyMouse_DriverName() ? ManyMouse_DriverName() : "?");

    /* Only Wayland needs the relay — on X11, clicks pass through to
     * whatever has focus normally since we never exclusively grab. */
    if (on_wayland)
        uinput_init();

    return 1;
}

/* -----------------------------------------------------------------------
 * MOUSE_Quit
 * ----------------------------------------------------------------------- */
void MOUSE_Quit(void)
{
    if (devices_grabbed) {
        ManyMouse_LinuxEvdevGrab(0);
        devices_grabbed = 0;
    }
    uinput_quit();
    ManyMouse_Quit();
#ifdef HAVE_X11
    if (x11display) { XCloseDisplay(x11display); x11display = NULL; }
#endif
}

/* -----------------------------------------------------------------------
 * MOUSE_Lock  – called when the user presses '4' to enable injection.
 * On Wayland: grab ManyMouse's own evdev fds so the compositor stops
 *             receiving pointer events; ManyMouse keeps reading fine
 *             because it owns the fd doing the grabbing.
 * On X11:     record position and warp back each frame.
 * ----------------------------------------------------------------------- */
void MOUSE_Lock(void)
{
    if (on_wayland) {
        int n = ManyMouse_LinuxEvdevGrab(1);
        devices_grabbed = (n > 0);
        if (!grab_warned) {
            if (devices_grabbed)
                fprintf(stderr, "[mi] Wayland: %d mouse device(s) grabbed – cursor locked.\n", n);
            else
                fprintf(stderr, "[mi] Wayland: could not grab any mouse device – "
                                "cursor may drift.\n"
                                "     Try: sudo usermod -aG input $USER (re-login after)\n");
            grab_warned = 1;
        }
        return;
    }

#ifdef HAVE_X11
    if (x11display) {
        Window root_ret, child_ret;
        int win_x, win_y;
        unsigned int mask;
        XQueryPointer(x11display, x11root, &root_ret, &child_ret,
                      &lock_x, &lock_y, &win_x, &win_y, &mask);
    }
#endif
}

/* -----------------------------------------------------------------------
 * MOUSE_ReleaseGrab  – release grab when '4' disables injection.
 * ----------------------------------------------------------------------- */
void MOUSE_ReleaseGrab(void)
{
    if (on_wayland && devices_grabbed) {
        ManyMouse_LinuxEvdevGrab(0);
        devices_grabbed = 0;
    }
}

/* -----------------------------------------------------------------------
 * MOUSE_Update  – poll ManyMouse, relay buttons/wheel, warp cursor (X11).
 * Called every game tick while injection is active.
 * ----------------------------------------------------------------------- */
void MOUSE_Update(const uint16_t tickrate)
{
#ifdef HAVE_X11
    /* X11 cursor warp (no-op on Wayland – evdev grab handles that) */
    if (!on_wayland && x11display) {
        if (tickrate > 8) {
            XWarpPointer(x11display, None, x11root, 0,0,0,0, lock_x, lock_y);
        } else {
            static uint8_t counter = 0;
            if (counter++ % 25 == 0)
                XWarpPointer(x11display, None, x11root, 0,0,0,0, lock_x, lock_y);
        }
        XFlush(x11display);
    }
#else
    (void)tickrate;
#endif

    xmouse = ymouse = 0;
    while (ManyMouse_PollEvent(&mm_event))
    {
        if (mm_event.type == MANYMOUSE_EVENT_RELMOTION) {
            if (mm_event.item == 0) xmouse += mm_event.value;
            else                    ymouse += mm_event.value;
        }
        /* Only relay while the real device is exclusively ours — on X11
         * (or if the Wayland grab failed) clicks already reach Dolphin
         * natively, and relaying too would double every click/scroll. */
        else if (devices_grabbed && mm_event.type == MANYMOUSE_EVENT_BUTTON) {
            uinput_relay_button((int)mm_event.item, mm_event.value);
        }
        else if (devices_grabbed && mm_event.type == MANYMOUSE_EVENT_SCROLL) {
            uinput_relay_scroll((int)mm_event.item, mm_event.value);
        }
    }
}
