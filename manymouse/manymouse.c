/*
 * ManyMouse foundation code – Linux port version.
 * Only evdev (and optionally XInput2) drivers are compiled in.
 *
 * Original code Copyright (c) 2005-2012 Ryan C. Gordon.
 */
#include <stdlib.h>
#include "manymouse.h"

static const char *manymouse_copyright =
    "ManyMouse " MANYMOUSE_VERSION " copyright (c) 2005-2012 Ryan C. Gordon.";

/* Drivers available on Linux */
extern const ManyMouseDriver *ManyMouseDriver_evdev;   /* linux_evdev.c   */

#ifdef HAVE_X11
extern const ManyMouseDriver *ManyMouseDriver_xinput2; /* x11_xinput2.c   */
#endif

/*
 * Favoured order: XInput2 (if available) → evdev
 * XInput2 gives per-device motion on X11; evdev works everywhere.
 */
static const ManyMouseDriver **mice_drivers[] =
{
#ifdef HAVE_X11
    &ManyMouseDriver_xinput2,
#endif
    &ManyMouseDriver_evdev,
};

static const ManyMouseDriver *driver = NULL;

int ManyMouse_Init(void)
{
    const int upper = (int)(sizeof(mice_drivers) / sizeof(mice_drivers[0]));
    int i;
    int retval = -1;

    if (manymouse_copyright == NULL) return -1;  /* keep symbol linked */
    if (driver != NULL) return -1;

    for (i = 0; (i < upper) && (driver == NULL); i++)
    {
        const ManyMouseDriver *d = *(mice_drivers[i]);
        if (d != NULL)
        {
            const int mice = d->init();
            if (mice > retval) retval = mice;
            if (mice >= 0)     driver = d;
        }
    }
    return retval;
}

void ManyMouse_Quit(void)
{
    if (driver) { driver->quit(); driver = NULL; }
}

const char *ManyMouse_DriverName(void)
{
    return driver ? driver->driver_name : NULL;
}

const char *ManyMouse_DeviceName(unsigned int index)
{
    return driver ? driver->name(index) : NULL;
}

int ManyMouse_PollEvent(ManyMouseEvent *event)
{
    return driver ? driver->poll(event) : 0;
}
