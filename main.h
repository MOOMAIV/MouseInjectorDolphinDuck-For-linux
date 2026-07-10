//==========================================================================
// Mouse Injector for Dolphin - Linux Port
//==========================================================================
// Copyright (C) 2019-2020 Carnivorous
// Linux port (C) 2024
//==========================================================================
#ifndef MAIN_H
#define MAIN_H

#include <string.h>
#include <stdint.h>

#define DOLPHINVERSION "Dolphin"
#define BUILDINFO      "(v0.31-linux - " __DATE__ ")"
#define LINE           "__________________________________________________________________"

/* ------------------------------------------------------------------
 * Keyboard polling using XQueryKeymap (global, no focus requirement).
 * Falls back to a simple stdin buffer when X11 is unavailable.
 * ------------------------------------------------------------------ */
void KEY_Update(void);   /* call once per frame to refresh state  */
int  KEY_Down(int keysym_or_ascii); /* returns 1 if key is currently held  */
int  KEY_Pressed(int keysym_or_ascii); /* edge: true once per keypress */
void KEY_Clear(void);    /* consume all pending key events */

/* Key identifiers (use ASCII for printable keys, extras below) */
#define KEY_INSERT  0x100
#define KEY_CTRL    0x101

/* Macros mirroring the original GetAsyncKeyState macros */
#define K_1      KEY_Pressed('1')
#define K_2      KEY_Pressed('2')
#define K_3      KEY_Pressed('3')
#define K_4      KEY_Pressed('4')
#define K_5      KEY_Pressed('5')
#define K_6      KEY_Pressed('6')
#define K_7      KEY_Pressed('7')
#define K_8      KEY_Pressed('8')
/* CTRL+0 / CTRL+1 → just '0' / '1' on Linux (terminals don't send Ctrl+digit) */
#define K_CTRL0  KEY_Pressed('0')
#define K_CTRL1  KEY_Pressed('1')
#define K_PLUS   (KEY_Pressed('+') || KEY_Pressed('='))
#define K_MINUS  KEY_Pressed('-')
/* INSERT → 'i' on keyboard (terminal Insert sends \033[2~ – handled in KEY_Update) */
#define K_INSERT KEY_Pressed(KEY_INSERT)

/* Millisecond sleep */
void Sleep(unsigned int ms);

/* Console helpers */
void SetConsoleTitle(const char *title);

/* Utility */
static inline float ClampFloat(const float value, const float min, const float max)
{
    const float test = value < min ? min : value;
    return test > max ? max : test;
}
static inline int32_t ClampInt(const int32_t value, const int32_t min, const int32_t max)
{
    const int32_t test = value < min ? min : value;
    return test > max ? max : test;
}
static inline uint16_t ClampHalfword(const uint16_t value, const uint16_t min, const uint16_t max)
{
    const int16_t test = (int16_t)(value < min ? min : value);
    return (uint16_t)(test > (int16_t)max ? (int16_t)max : test);
}
static inline uint8_t FloatsEqual(const float f1, const float f2)
{
    const float epsilon = 0.0001f;
    return (f1 - f2) < epsilon;
}

extern void AccumulateAddRemainder(float *value, float *accumulator, float dir, float dx);

extern uint8_t sensitivity;
extern uint8_t crosshair;
extern uint8_t invertpitch;
extern uint8_t optionToggle;
extern float   out;
extern float   out2;
extern float   out3;
extern float   preSinOut;
extern float   preCosOut;
extern float   totalAngleOut;
extern uint32_t uIntOut1;
extern uint32_t uIntOut2;
extern char    titleOut[256];
extern uint64_t emuoffsetOut;
extern int     isHooked;

#endif /* MAIN_H */
