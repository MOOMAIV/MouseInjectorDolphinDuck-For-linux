/*
 * ManyMouse main header. Include this from your app.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#ifndef _INCLUDE_MANYMOUSE_H_
#define _INCLUDE_MANYMOUSE_H_

#ifdef __cplusplus
extern "C" {
#endif

#define MANYMOUSE_VERSION "0.0.3"

typedef enum
{
    MANYMOUSE_EVENT_ABSMOTION = 0,
    MANYMOUSE_EVENT_RELMOTION,
    MANYMOUSE_EVENT_BUTTON,
    MANYMOUSE_EVENT_SCROLL,
    MANYMOUSE_EVENT_DISCONNECT,
    MANYMOUSE_EVENT_MAX
} ManyMouseEventType;

typedef struct
{
    ManyMouseEventType type;
    unsigned int device;
    unsigned int item;
    int value;
    int minval;
    int maxval;
} ManyMouseEvent;


/* internal use only. */
typedef struct
{
    const char *driver_name;
    int (*init)(void);
    void (*quit)(void);
    const char *(*name)(unsigned int index);
    int (*poll)(ManyMouseEvent *event);
} ManyMouseDriver;


int ManyMouse_Init(void);
const char *ManyMouse_DriverName(void);
void ManyMouse_Quit(void);
const char *ManyMouse_DeviceName(unsigned int index);
int ManyMouse_PollEvent(ManyMouseEvent *event);

/*
 * Linux-only: exclusively grab (grab=1) or release (grab=0) every mouse
 * fd that ManyMouse's own evdev backend is reading from. This must be
 * used instead of opening separate fds to the same devices — EVIOCGRAB
 * is per file descriptor, and grabbing via a different fd silences
 * ManyMouse's own reads. Returns the number of devices successfully
 * grabbed/released. Always returns 0 if the evdev driver isn't active
 * or on non-Linux platforms.
 */
int ManyMouse_LinuxEvdevGrab(int grab);

#ifdef __cplusplus
}
#endif

#endif  /* !defined _INCLUDE_MANYMOUSE_H_ */

/* end of manymouse.h ... */

