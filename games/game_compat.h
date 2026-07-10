/*
 * game_compat.h  –  force-included for all game drivers on Linux.
 *
 * Many game drivers use sin/cos/atan/abs/fmod/ceil without explicitly
 * including <math.h> or <stdlib.h>. Under GCC 14+, implicit function
 * declarations are errors, not warnings. This header fixes that without
 * touching every upstream game file.
 *
 * Included automatically via -include flag in the Makefile.
 */
#ifndef GAME_COMPAT_H
#define GAME_COMPAT_H

#include <math.h>    /* sin, cos, atan, atan2, fmod, ceil, floorf, fabsf ... */
#include <stdlib.h>  /* abs */
#include <stdint.h>

#endif /* GAME_COMPAT_H */
