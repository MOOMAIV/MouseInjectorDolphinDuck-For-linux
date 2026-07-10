# MouseInjector Linux Port

A Linux port i made with claude of [MouseInjectorDolphinDuck](https://github.com/Altureus/MouseInjectorDolphinDuck) 

## Disclaimer
This was made with ai and has had very very little work done on it by me, so if you find anything goofy or out of place thats why.

## How to Use
1. Start emulator first
2. Start MouseInjector, read initial information then press ctrl+1
3. Make sure game is running, move the terminal over the emulator and press '4' to hook into the process, then alt tab to focus the emulator.
    1. If game is supported then the mouse will be captured at the position it was at when hooked
        * You will be <b><u>unable</u></b> to use the mouse elsewhere while it is hooked, focus the terminal then press 4 to unhook
        * Some games depend on post startup values/addresses so hook may not happen immediately
            * DuckStation games usually will not hook until after the startup sequence
    2. Unsupported/broken games will not hook and mouse won't be captured
4. Adjust options with numbers 4-7 while in-game, ctrl+0 will lock the settings
 
## Emulators

| Emulator       | Process name(s)                      | Notes                         |
|----------------|--------------------------------------|-------------------------------|
| Dolphin        | `dolphin-emu`, `dolphin-emu-qt`      | GC/Wii                        |
| DuckStation    | `duckstation-qt`, `duckstation-nogui`| Does not work                 |
| PCSX2          | `pcsx2`, `pcsx2-qt`, `PCSX2`         | Cant find games               |
| RetroArch      | `retroarch`                          | Untested                      |
| PPSSPP         | `PPSSPP`, `PPSSPPQt`                 | Untested                      |
| simple64       | `simple64-gui`                       | Planned                       |
| Flycast        | `flycast`                            | Untested                      |



> All supported game titles are the same as the upstream project. See the upstream GAMES.md for the full list.

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
# (Wayland / headless – terminal must be focused for hotkeys)
make HAVE_X11=0

# Untested (X11 – global hotkeys + cursor warp)
make HAVE_X11=1

```

## Running

### Mouse device access - might not be needed

The evdev backend reads raw events from `/dev/input/event*`. Most modern distros allow this for logged-in users via the `input` group:

```bash
# Add yourself to the input group (log out and back in after)
sudo usermod -aG input $USER
```

Or test immediately with:
```bash
sudo ./mouseinjector
```

## Known limitations

- **Wayland**: cursor locking can only be done by having the window active
- **Windows-only emulators** (BizHawk, Project64, NO$PSX) are not supported

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

## How it works

- **Process discovery** — scans `/proc` for supported emulators by executable name
- **Memory access** — uses `process_vm_readv`/`process_vm_writev` (Linux 3.2+, no ptrace needed)
- **RAM scanning** — parses `/proc/PID/maps` to find the emulator's console RAM region
- **Core detection** — for RetroArch, inspects `/proc/PID/maps` for loaded `.so` names instead of window titles
- **Mouse input** — ManyMouse with the Linux evdev backend (`/dev/input/event*`)
- **Keyboard hotkeys** — `XQueryKeymap` (X11, global) or termios raw stdin (fallback)
- **Cursor locking** — `XWarpPointer` (X11) or skipped gracefully on Wayland



## License


- ManyMouse is Copyright (c) 2005-2012 Ryan C. Gordon and others. https://icculus.org/manymouse/
- GPL-2.0 — same as the upstream project.
