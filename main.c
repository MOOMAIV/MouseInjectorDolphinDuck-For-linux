//==========================================================================
// Mouse Injector for Dolphin - Linux Port
//==========================================================================
// Copyright (C) 2019-2020 Carnivorous
// Linux port (C) 2024
//==========================================================================
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>

#ifdef HAVE_X11
#  include <X11/Xlib.h>
#  include <X11/keysym.h>
#  ifdef Status
#    undef Status  /* X11 pollutes this name; game.h uses it as a struct member */
#  endif
static Display *kbd_display = NULL;
#else
/* Stub so non-HAVE_X11 code paths compile */
static void *kbd_display = NULL;
#endif

#include "main.h"
#include "memory.h"
#include "mouse.h"
#include "./games/game.h"

/* ------------------------------------------------------------------
 * External toggle via signal — for Wayland users.
 *
 * Once mouse injection grabs the evdev device, EVIOCGRAB claims the
 * WHOLE device: clicks and scroll stop reaching the compositor too,
 * not just movement. That means there is no mouse-based way to
 * refocus another window, and on a build without X11 there is no
 * keyboard-based global hotkey either — termios only sees keys while
 * the terminal itself has focus.
 *
 * SIGUSR1 toggles mouse injection exactly like pressing '4', but can
 * be delivered from anywhere — including a desktop-environment
 * global keyboard shortcut, since DE shortcuts (e.g. KDE's
 * System Settings -> Shortcuts -> Custom Shortcuts) are handled by
 * the compositor above the per-window focus layer and are NOT
 * affected by our mouse-only device grab.
 *
 * The PID is written to a small file at startup so a shortcut command
 * can target this exact process, e.g.:
 *   kill -USR1 $(cat /tmp/mouseinjector.pid)
 * ------------------------------------------------------------------ */
static volatile sig_atomic_t g_toggle_requested = 0;
#define WELCOME_TIMEOUT_SECS 20
static char pidfile_path[256];

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_toggle_requested = 1; /* handled in the main loop, not here */
}

static void write_pidfile(void)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0])
        snprintf(pidfile_path, sizeof(pidfile_path), "%s/mouseinjector.pid", runtime_dir);
    else
        snprintf(pidfile_path, sizeof(pidfile_path), "/tmp/mouseinjector.pid");

    FILE *fp = fopen(pidfile_path, "w");
    if (fp) {
        fprintf(fp, "%d", (int)getpid());
        fclose(fp);
    } else {
        pidfile_path[0] = '\0'; /* don't try to remove it on exit */
    }
}

static void remove_pidfile(void)
{
    if (pidfile_path[0])
        unlink(pidfile_path);
}

/* ------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------ */
enum EDITINGCURRENT { EDITINGSENSITIVITY = 0, EDITINGCROSSHAIR };

static uint8_t mousetoggle    = 0;
static uint8_t selectedoption = EDITINGSENSITIVITY;
static uint8_t locksettings   = 0;
static uint8_t welcomed       = 0;

uint8_t  sensitivity   = 20;
uint8_t  crosshair     = 3;
uint8_t  invertpitch   = 0;
int      isHooked      = 0;
uint8_t  uncapTickrate = 0;
uint8_t  optionToggle  = 0;

float    out = 0, out2 = 0, out3 = 0;
float    preSinOut = 0, preCosOut = 0, totalAngleOut = 0;
uint32_t uIntOut1 = 0, uIntOut2 = 0;
char     titleOut[256];
uint64_t emuoffsetOut = 0;

/* ------------------------------------------------------------------
 * Platform: Sleep, console title
 * ------------------------------------------------------------------ */
void Sleep(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

void SetConsoleTitle(const char *title)
{
    printf("\033]0;%s\007", title);
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * Keyboard input
 *
 * Two backends selected at runtime:
 *   X11  — XQueryKeymap (global, works when emulator window is focused)
 *   termios — raw stdin (requires terminal focus)
 * ------------------------------------------------------------------ */

/* ----- termios raw stdin ----- */
static struct termios orig_termios;
static int            stdin_orig_flags;
static int            termios_active = 0;

/* Small circular byte buffer for raw stdin */
#define KEYBUF  64
static char keybuf[KEYBUF];
static int  keybuf_len = 0;

static void termios_init(void)
{
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    stdin_orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, stdin_orig_flags | O_NONBLOCK);
    termios_active = 1;
}

static void termios_restore(void)
{
    if (termios_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        fcntl(STDIN_FILENO, F_SETFL, stdin_orig_flags);
        termios_active = 0;
    }
}

static void stdin_drain(void)
{
    char tmp[32];
    int n = (int)read(STDIN_FILENO, tmp, sizeof(tmp));
    if (n <= 0) return;
    for (int i = 0; i < n && keybuf_len < KEYBUF; i++) {
        /* Translate ESC[2~ (Insert) → KEY_INSERT sentinel byte */
        if (tmp[i] == '\033' && i+3 < n &&
            tmp[i+1] == '[' && tmp[i+2] == '2' && tmp[i+3] == '~')
        {
            keybuf[keybuf_len++] = '\x7F'; /* sentinel for Insert */
            i += 3;
            continue;
        }
        keybuf[keybuf_len++] = tmp[i];
    }
}

/* Consume key from stdin buffer */
static int stdin_consume(int k)
{
    char target = (char)(k == KEY_INSERT ? '\x7F' : (char)k);
    for (int i = 0; i < keybuf_len; i++) {
        if (keybuf[i] == target) {
            memmove(keybuf + i, keybuf + i + 1, (size_t)(keybuf_len - i - 1));
            keybuf_len--;
            return 1;
        }
    }
    return 0;
}

/* ----- X11 backend ----- */
#ifdef HAVE_X11
#  define MAX_WATCHED 32
static struct { int key; int was_down; } edge[MAX_WATCHED];
static int edge_cnt = 0;

static int x11_rawdown(KeySym sym)
{
    if (!kbd_display || sym == NoSymbol) return 0;
    char keys[32];
    XQueryKeymap(kbd_display, keys);
    KeyCode code = XKeysymToKeycode(kbd_display, sym);
    if (!code) return 0;
    return (keys[code / 8] >> (code % 8)) & 1;
}

static KeySym keysym_for(int k)
{
    if (k >= '0' && k <= '9') return (KeySym)(XK_0 + k - '0');
    if (k == '+')   return XK_KP_Add;
    if (k == '=')   return XK_equal;
    if (k == '-')   return XK_minus;
    if (k == KEY_INSERT) return XK_Insert;
    return NoSymbol;
}

static int x11_isdown(int k)
{
    /* Special: '+' check multiple syms */
    if (k == '+') {
        return x11_rawdown(XK_plus) || x11_rawdown(XK_equal) ||
               x11_rawdown(XK_KP_Add);
    }
    return x11_rawdown(keysym_for(k));
}

/* Edge detection per key */
static int x11_pressed(int k)
{
    int down = x11_isdown(k);
    for (int i = 0; i < edge_cnt; i++) {
        if (edge[i].key == k) {
            int was = edge[i].was_down;
            edge[i].was_down = down;
            return (down && !was) ? 1 : 0;
        }
    }
    if (edge_cnt < MAX_WATCHED) {
        edge[edge_cnt].key      = k;
        edge[edge_cnt].was_down = down;
        edge_cnt++;
    }
    return down;
}
#endif /* HAVE_X11 */

/* ----- Public KEY API ----- */

void KEY_Update(void)
{
    if (termios_active) {
        keybuf_len = 0;
        stdin_drain();
    }
}

int KEY_Down(int k)
{
#ifdef HAVE_X11
    if (kbd_display) return x11_isdown(k);
#endif
    /* termios: non-destructive scan */
    char target = (char)(k == KEY_INSERT ? '\x7F' : (char)k);
    for (int i = 0; i < keybuf_len; i++)
        if (keybuf[i] == target) return 1;
    return 0;
}

int KEY_Pressed(int k)
{
#ifdef HAVE_X11
    if (kbd_display) return x11_pressed(k);
#endif
    return stdin_consume(k);
}

void KEY_Clear(void)
{
    keybuf_len = 0;
}

/* ------------------------------------------------------------------
 * Console helpers
 * ------------------------------------------------------------------ */
static void GUI_Clear(void)
{
    printf("\033[H\033[2J");
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------ */
static void quit(void);
static void ToggleMouseInjection(void);
static void GUI_TitleShowHookStatus(int hooked);
static void GUI_Init(void);
static void GUI_Welcome(void);
static void GUI_Interact(void);
static void GUI_Update(void);
static void GUI_ListGames(void);
static void INI_Load(void);
static void INI_Save(uint8_t showerror);

/* ------------------------------------------------------------------
 * Signal handler (restore terminal on Ctrl-C)
 * ------------------------------------------------------------------ */
static void sig_handler(int sig)
{
    (void)sig;
    quit();
    _Exit(0);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int32_t main(void)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sigusr1_handler);
    atexit(quit); /* register up front so every exit path below cleans up */

#ifdef HAVE_X11
    kbd_display = XOpenDisplay(NULL);
    if (!kbd_display)
        fprintf(stderr,
            "Note: X11 unavailable – hotkeys only work when terminal is focused.\n");
#endif
    if (!kbd_display)
        termios_init();

    GUI_Init();

    if (!MEM_Init()) {
        printf("\n Mouse Injector for %s %s\n%s\n\n"
               "   Supported emulator not detected. Closing...\n",
               DOLPHINVERSION, BUILDINFO, LINE);
        Sleep(3000);
        return 0;
    }

    if (!MOUSE_Init()) {
        printf("\n Mouse Injector for %s %s\n%s\n\n"
               "   Mouse not detected.\n"
               "   Make sure you are in the 'input' group:\n"
               "     sudo usermod -aG input $USER   (then log out/in)\n"
               "   Or run once with sudo to test.\n",
               DOLPHINVERSION, BUILDINFO, LINE);
        MEM_Quit();
        Sleep(5000);
        return 0;
    }

    /* Only write the PID file once there's an actual running instance
     * worth toggling — atexit(quit) above guarantees it's removed on
     * every exit path, including these early-return cases above it. */
    write_pidfile();

    INI_Load();
    if (!welcomed)
        GUI_Welcome();

    if (pidfile_path[0])
        fprintf(stderr,
            "[mi] PID %d — to toggle mouse injection from anywhere (e.g. a KDE\n"
            "     global shortcut), run:  kill -USR1 $(cat %s)\n",
            (int)getpid(), pidfile_path);

    GUI_Update();

    int initialHookOccurred = 0;
    while (1) {
        KEY_Update();
        GUI_Interact();

        if (g_toggle_requested) {
            g_toggle_requested = 0;
            ToggleMouseInjection();
            GUI_Update();
        }

        int hooked = 0;
        if (mousetoggle) {
            if (GAME_Status()) {
                MOUSE_Update(GAME_Tickrate());
                GAME_Inject();
                if (!initialHookOccurred) {
                    GUI_Update();
                    initialHookOccurred = 1;
                }
                hooked = 1;
            } else {
                hooked = 0;
                initialHookOccurred = 0;
                MEM_FindRamOffset();
                Sleep(100);
            }
            Sleep(GAME_Tickrate());
        } else {
            Sleep(100);
        }

        GUI_TitleShowHookStatus(hooked);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * quit
 * ------------------------------------------------------------------ */
static void quit(void)
{
    INI_Save(0);
    MOUSE_Quit();
    MEM_Quit();
    termios_restore();
    remove_pidfile();
#ifdef HAVE_X11
    if (kbd_display) {
        XCloseDisplay(kbd_display);
        kbd_display = NULL;
    }
#endif
    printf("\n");
}

static void GUI_TitleShowHookStatus(int hooked)
{
    if (hooked == isHooked) return;
    SetConsoleTitle(hooked ? "Mouse Injector | Hooked"
                           : "Mouse Injector | Unhooked");
    isHooked = hooked;
}

static void GUI_Init(void)
{
    SetConsoleTitle("Mouse Injector");
    printf("\033[8;27;80t"); /* suggest terminal size */
    fflush(stdout);
}

static void GUI_Welcome(void)
{
    GUI_Clear();
    printf("\n    Mouse Injector for %s %s\n%s\n\n"
           "   Addendum - Please Read before Use\n\n\n",
           DOLPHINVERSION, BUILDINFO, LINE);
    printf("    1)  Alpha Linux port - expect issues and crashes\n\n");
    printf("    2)  Sub-systems unsupported - use arrow keys for sentries/cameras\n\n");
    printf("    3)  NetPlay is unsupported\n\n");
    printf("    4)  Press '1' in main menu to list supported games (NTSC only)\n\n");
#ifdef HAVE_X11
    printf("    5)  X11 detected - hotkeys work globally\n\n\n");
#else
    printf("    5)  No X11 - keep terminal focused when using hotkeys\n\n\n");
#endif
    printf("   Press 1 to confirm you've read this...\n"
           "   (auto-continuing in %d seconds if the emulator has focus)\n%s\n",
           WELCOME_TIMEOUT_SECS, LINE);
    fflush(stdout);

    /* Two independent ways out, since terminal keyboard focus can't be
     * relied on the moment an emulator's own window grabs it (PCSX2's
     * BIOS/boot sequence often does this right as this screen appears):
     *   1) press '1' — works whenever the terminal does have focus
     *   2) SIGUSR1   — the same signal already wired up for toggling
     *                  mouse injection from a desktop global shortcut;
     *                  reaches us regardless of window focus
     * A timeout is the final backstop so this can never hang forever. */
    int elapsed_ms = 0;
    const int timeout_ms = WELCOME_TIMEOUT_SECS * 1000;
    while (!welcomed) {
        KEY_Update();
        if (K_CTRL1) welcomed = 1;
        if (g_toggle_requested) {
            g_toggle_requested = 0;
            welcomed = 1;
        }
        if (elapsed_ms >= timeout_ms)
            welcomed = 1;
        Sleep(250);
        elapsed_ms += 250;
    }
}

static void ToggleMouseInjection(void)
{
    if (!mousetoggle) {
        MOUSE_Lock();
        MOUSE_Update(GAME_Tickrate());
    } else {
        MOUSE_ReleaseGrab();
    }
    mousetoggle = !mousetoggle;
}

static void GUI_Interact(void)
{
    uint8_t upd = 0, quick = 0;

    if (K_4) {
        ToggleMouseInjection();
        upd = 1;
        INI_Save(1);
    }
    if (K_5 && !locksettings && !upd) {
        selectedoption = EDITINGSENSITIVITY;
        upd = 1;
        INI_Save(1);
    }
    if (K_6 && !locksettings && !upd && GAME_CrosshairSwaySupported()) {
        selectedoption = EDITINGCROSSHAIR;
        upd = 1;
        INI_Save(1);
    }
    if (K_7 && !locksettings && !upd) {
        invertpitch = !invertpitch;
        upd = 1;
        INI_Save(1);
    }
    if (K_8 && !locksettings && !upd && GAME_OptionSupported()) {
        optionToggle = !optionToggle;
        upd = 1;
        INI_Save(1);
    }
    if (K_PLUS && !locksettings && !upd) {
        if (selectedoption == EDITINGSENSITIVITY && sensitivity < 200)
            sensitivity++, upd = 1; INI_Save(1);
        if (selectedoption == EDITINGCROSSHAIR && crosshair < 18 &&
            GAME_CrosshairSwaySupported())
            crosshair++, upd = 1; INI_Save(1);
        quick = 1;
    }
    if (K_MINUS && !locksettings && !upd) {
        if (selectedoption == EDITINGSENSITIVITY && sensitivity > 1)
            sensitivity--, upd = 1; INI_Save(1);
        if (selectedoption == EDITINGCROSSHAIR && crosshair > 0 &&
            GAME_CrosshairSwaySupported())
            crosshair--, upd = 1; INI_Save(1);
        quick = 1;
    }
    if (K_INSERT && !locksettings && !upd && !mousetoggle) {
        GUI_ListGames();
        Sleep(10 * 1000);
        KEY_Clear();
        upd = 1; quick = 1; INI_Save(1);
    }
    if (K_CTRL0 && !upd) {
        locksettings = !locksettings;
        upd = 1; INI_Save(1);
    }

     if (upd) {
        GUI_Update();
        Sleep(quick ? 100 : 200);
    }
}

static void GUI_Update(void)
{
    GUI_Clear();
    GAME_Status();
    printf("\n Mouse Injector for %s - %s\n", hookedEmulatorName, GAME_Name());
    printf("%s\n\n   Main Menu - Press [#] to Use\n\n\n", LINE);
    printf(mousetoggle ? "   [4] - [ON]  Mouse Injection\n\n"
                       : "   [4] - [OFF] Mouse Injection\n\n");
    if (!locksettings) {
        printf("   [5] - Mouse Sensitivity: %d%%", sensitivity * 5);
        printf(selectedoption == EDITINGSENSITIVITY ? " [+/-]\n\n" : "\n\n");
        printf("   [6] - Crosshair Sway: ");
        if (GAME_CrosshairSwaySupported())
            printf(crosshair ? "%d%%" : "Locked", crosshair * 100 / 6);
        else
            printf("Not Available For Game");
        printf(selectedoption == EDITINGCROSSHAIR ? " [+/-]\n\n" : "\n\n");
        printf(invertpitch ? "   [7] - [ON]  Invert Pitch\n\n"
                           : "   [7] - [OFF] Invert Pitch\n\n");
        if (GAME_OptionSupported())
            printf("   [8] - %s\n\n", GAME_OptionMessage());
        printf("\n\n\n\n\n");
        printf("   [0] - %s Settings\n\n", locksettings ? "Unlock" : "Lock");
    } else {
        printf("\n\n\n\n\n\n\n\n\n\n\n");
        printf("   [0] - Unlock Settings\n\n");
    }
    if (mousetoggle || locksettings)
        printf(" Note: [+/-] to Change Values\n%s\n", LINE);
    else
        printf(" Note: [+/-] to Change Values  |  [Ins] List Supported Games\n%s\n", LINE);
      fflush(stdout);
}

static void GUI_ListGames(void)
{
    GUI_Clear();
    printf("\n Supported Games (NTSC Only)\t\t\tGame ID   Mouse Support\n%s\n\n", LINE);
    printf("    007: Agent Under Fire\t\t\t GW7E69\t  Good\n\n");
    printf("    TimeSplitters 2\t\t\t\t GTSE4F\t  Fair\n\n");
    printf("    TimeSplitters: Future Perfect\t\t G3FE69\t  Poor\n\n");
    printf("    007: NightFire\t\t\t\t GO7E69\t  Poor\n\n");
    printf("    Medal of Honor: Frontline\t\t\t GMFE69\t  Fair\n\n");
    printf("    Medal of Honor: European Assault\t\t GONE69\t  Good\n\n");
    printf("    Medal of Honor: Rising Sun\t\t\t GR8E69\t  Poor\n\n");
    printf("    Call of Duty 2: Big Red One\t\t\t GQCE52\t  Good\n\n");
    printf("    Die Hard: Vendetta\t\t\t\t GDIE7D\t  Fair\n\n");
    printf("    Serious Sam: Next Encounter\t\t\t G3BE9G\t  Fair\n\n");
    printf("    Trigger Man\t\t\t\t\t GG2E4Z\t  Good\n\n");
    printf("   (See README for full PS1/PS2/N64/SNES lists)\n");
    printf("   Returning to Main Menu in 10 seconds...\n%s\n", LINE);
    fflush(stdout);
}
static void INI_Load(void)
{
FILE *fileptr; // create a file pointer and open mouseinjector.ini from same dir as our program
if((fileptr = fopen("mouseinjector.ini", "r")) != NULL) // if the INI exists
{
    char line[128][128]; // char array used for file to write to
    char lines[128]; // maximum lines read size
    uint8_t counter = 0; // used to assign each line to a array
    while(fgets(lines, sizeof(lines), fileptr) != NULL && counter < 11) // read the first 10 lines
    {
        strcpy(line[counter], lines); // read file lines and assign value to line array
        counter++; // add 1 to counter, so the next line can be read
    }
    fclose(fileptr); // close the file stream
    if(counter == 5) // check if mouseinjector.ini length is valid
    {
        sensitivity = ClampInt(atoi(line[0]), 1, 200);
        crosshair = ClampInt(atoi(line[1]), 0, 18);
        invertpitch = !(!atoi(line[2]));
        locksettings = !(!atoi(line[3]));
        welcomed = !(!atoi(line[4]));
    }
    else
    {

    INI_Save(1); // overwrite mouseinjector.ini with valid settings
    }
}
else // if loading file failed
{

INI_Save(1); // create mouseinjector.ini
}
}
static void INI_Save(const uint8_t showerror)
{
   FILE *fileptr;
    if((fileptr = fopen("mouseinjector.ini", "w")) != NULL)
{
        fprintf(fileptr, "%u\n%u\n%u\n%u\n%u", sensitivity, crosshair, invertpitch, locksettings, welcomed);
        fclose(fileptr);

}
    else if (showerror)
        fprintf(stderr, "Warning: could not save mouseinjector.ini\n");


}

/* ------------------------------------------------------------------
 * AccumulateAddRemainder (unchanged from original)
 * ------------------------------------------------------------------ */
void AccumulateAddRemainder(float *value, float *accumulator, float dir, float dx)
{
    if (dir == 0) return;
    if (dir < 0)  *value += ceilf(dx);
    else           *value += floorf(dx);
    float r = fmodf(dx, 1.0f);
    if (fabsf(r + *accumulator) >= 1.0f) {
        if (dir > 0) *value += 1;
        else          *value -= 1;
    }
    *accumulator = fmodf(r + *accumulator, 1.0f);
}
