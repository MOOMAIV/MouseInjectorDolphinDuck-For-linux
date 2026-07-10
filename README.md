# MouseInjector Linux Port

A Linux port of [MouseInjectorDolphinDuck](https://github.com/Altureus/MouseInjectorDolphinDuck) — an external app that injects raw mouse input into emulator game memory, giving you proper mouse look in classic console FPS titles.

## How it works

- **Process discovery** — scans `/proc` for supported emulators by executable name
- **Memory access** — uses `process_vm_readv`/`process_vm_writev` (Linux 3.2+, no ptrace needed)
- **RAM scanning** — parses `/proc/PID/maps` to find the emulator's console RAM region
- **Core detection** — for RetroArch, inspects `/proc/PID/maps` for loaded `.so` names instead of window titles
- **Mouse input** — ManyMouse with the Linux evdev backend (`/dev/input/event*`)
- **Keyboard hotkeys** — `XQueryKeymap` (X11, global) or termios raw stdin (fallback)
- **Cursor locking** — `XWarpPointer` (X11) or skipped gracefully on Wayland

## Supported Emulators on Linux

| Emulator       | Process name(s)                      | Notes                         |
|----------------|--------------------------------------|-------------------------------|
| Dolphin        | `dolphin-emu`, `dolphin-emu-qt`      | GC/Wii                        |
| DuckStation    | `duckstation-qt`, `duckstation-nogui`| PS1                           |
| PCSX2          | `pcsx2`, `pcsx2-qt`, `PCSX2`         | PS2                           |
| RetroArch      | `retroarch`                          | Detects core from maps        |
| PPSSPP         | `PPSSPP`, `PPSSPPQt`                 | PSP                           |
| simple64       | `simple64-gui`                       | N64                           |
| RMG            | `RMG`                                | N64                           |
| RPCS3          | `rpcs3`                              | PS3                           |
| Flycast        | `flycast`                            | Dreamcast                     |

> All supported game titles are the same as the upstream project. See the upstream README for the full list.

## Building

### Dependencies

```bash
# Debian/Ubuntu
sudo apt install gcc make libx11-dev libxi-dev

# Fedora/RHEL
sudo dnf install gcc make libX11-devel libXi-devel

# Arch
sudo pacman -S gcc make libx11 libxi
```

### Compile

```bash
# Recommended (X11 – global hotkeys + cursor warp)
make HAVE_X11=1

# Without X11 (Wayland / headless – terminal must be focused for hotkeys)
make HAVE_X11=0
```

## Running

### Mouse device access

The evdev backend reads raw events from `/dev/input/event*`. Most modern distros allow this for logged-in users via the `input` group:

```bash
# Add yourself to the input group (log out and back in after)
sudo usermod -aG input $USER
```

Or test immediately with:
```bash
sudo ./mouseinjector
```

### Mouse buttons and scroll wheel on Wayland (optional)

On Wayland, locking the cursor requires exclusively grabbing the whole mouse device — which also blocks left/right/middle click and the scroll wheel from reaching anything else, including Dolphin's own controller bindings. To fix this, the injector creates a small virtual input device ("Mouse Injector Button Relay") and forwards clicks/wheel ticks through it while injection is active, so Dolphin can bind to that instead.

This needs write access to `/dev/uinput`, which is usually root-only by default. A one-time udev rule fixes it:

```bash
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' | sudo tee /etc/udev/rules.d/99-mouseinjector-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

(Log out and back in if you just added yourself to the `input` group above.) If `/dev/uinput` still isn't accessible, the injector prints a warning and keeps running normally — only the button/wheel relay is skipped, mouse-look injection is unaffected.

Once running, open Dolphin's **Controller Settings** and bind whichever GameCube buttons you want to mouse clicks/wheel, selecting **"Mouse Injector Button Relay"** as the input device (instead of binding to your physical mouse directly).

### Usage

1. **Start the emulator first** and load your game
2. Run `./mouseinjector` in a terminal
3. Press **1** to confirm the welcome message
4. Press **4** to toggle mouse injection on/off
5. Use **+/-** to adjust sensitivity; **5/6/7** to select the option to change
6. Press **0** to lock/unlock settings

> **X11 users:** hotkeys work globally — you can switch focus to the emulator and the injector will still respond to keypresses.
>
> **Wayland users:** see [Wayland: getting your mouse and keyboard back](#wayland-getting-your-mouse-and-keyboard-back) below — once injection is on, the mouse is fully grabbed (clicks and scroll included) and there's no built-in way to refocus a window by clicking.

## Wayland: getting your mouse and keyboard back

Once mouse injection is active, the injector exclusively grabs the mouse device via `EVIOCGRAB` so the desktop cursor stays locked. This grab claims the **entire device** — clicks and scroll wheel stop reaching the compositor too, not just movement. Combined with a non-X11 build (where keyboard hotkeys only work while the terminal has keyboard focus), this can leave you with no way to click back into the terminal or use a keyboard shortcut to turn injection off.

The fix: the injector listens for `SIGUSR1` and toggles mouse injection exactly like pressing `4` — and unlike mouse clicks, **keyboard shortcuts bound by your desktop environment are handled by the compositor above the per-window focus layer**, so they keep working even while the mouse is fully grabbed (we only grab mouse devices, never the keyboard).

On startup, the injector prints its PID and writes it to a small file:
```
[mi] PID 12345 — to toggle mouse injection from anywhere (e.g. a KDE
     global shortcut), run:  kill -USR1 $(cat /run/user/1000/mouseinjector.pid)
```

### Setting this up in KDE

1. Open **System Settings → Shortcuts → Custom Shortcuts**
2. **Edit → New → Global Shortcut → Command/URL**
3. Set the **Trigger** to any key combo you like (e.g. `Meta+M`)
4. Set the **Action** command to:
   ```
   kill -USR1 $(cat /run/user/1000/mouseinjector.pid 2>/dev/null || cat /tmp/mouseinjector.pid)
   ```
   (replace `1000` with your UID if different — check with `id -u`, or just use the exact path the injector printed at startup)
5. Apply. Your chosen key combo now toggles mouse injection on/off from anywhere — no focus or mouse access required.

This isn't KDE-specific under the hood — any desktop environment that can bind a global shortcut to a shell command works the same way (GNOME via `gsettings`/Custom Shortcuts, Sway via `bindsym` in its config, etc.), since it's just sending a standard Unix signal to a PID.

## How hotkeys differ from Windows

| Action            | Windows     | Linux       |
|-------------------|-------------|-------------|
| Confirm welcome   | Ctrl+1      | **1**       |
| Toggle mouse      | 4           | **4**       |
| Lock settings     | Ctrl+0      | **0**       |
| List games        | Insert      | **Insert** or `i` |
| Sensitivity +/-   | Numpad +/-  | **+/-** or **=/-** |

## Known limitations

- **Wayland**: cursor locking works via an exclusive `EVIOCGRAB` on the mouse device rather than cursor warping — see [Wayland: getting your mouse and keyboard back](#wayland-getting-your-mouse-and-keyboard-back) for how to toggle injection without mouse or terminal-keyboard access once it's grabbed.
- **Windows-only emulators** (BizHawk, Project64, NO$PSX) are not supported natively. They may work under Wine, but this hasn't been tested.
- **RPCS3** memory scanning has not been verified on Linux — the region sizes may differ from Windows.

## Project structure

| File | Description |
|------|-------------|
| `main.c` | Main loop, TUI, keyboard input (POSIX + X11) |
| `main.h` | Portability macros replacing `windows.h` |
| `memory.c` | Process discovery + memory R/W via `/proc` |
| `mouse.c` | ManyMouse evdev + X11 cursor warp |
| `manymouse/linux_evdev.c` | ManyMouse Linux backend |
| `manymouse/x11_xinput2.c` | ManyMouse XInput2 backend |
| `games/` | Game-specific injection logic (unchanged) |
| `Makefile` | Linux build system |

## License

GPL-2.0 — same as the upstream project.
