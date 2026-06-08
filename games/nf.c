//==========================================================================
// Mouse Injector for Dolphin
//==========================================================================
// Copyright (C) 2019-2020 Carnivorous
// All rights reserved.
//
// Mouse Injector is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, visit http://www.gnu.org/licenses/gpl-2.0.html
//==========================================================================
#include <math.h>
#include <stdint.h>
#include "../main.h"
#include "../memory.h"
#include "../mouse.h"
#include "game.h"

#define PI 3.14159265f // 0x40490FDB
#define TAU 6.2831853f // 0x40C90FDB
#define CROSSHAIRX 0.296f // 0x3E978D50
#define CROSSHAIRY 0.39999999f // 0xBECCCCCC
#define EQUINOXAIMSCALE 300.f
#define SENTRYMINY -0.1616991162f // 0xBE259474
#define SENTRYMAXY 0.1349733025f // 0x3E0A3671
#define SENTRYFOVBASE 41.25f // 0x42250000
// NF ADDRESSES - OFFSET ADDRESSES BELOW (REQUIRES PLAYERBASE TO USE)
#define NF_camx 0x80B96E2C - 0x80B96DEC
#define NF_camy 0x80B976F0 - 0x80B96DEC
#define NF_fov 0x80B97718 - 0x80B96DEC
#define NF_crosshairx 0x80B96F98 - 0x80B96DEC
#define NF_crosshairy 0x80B96F9C - 0x80B96DEC
#define NF_health 0x80B976DC - 0x80B96DEC
#define NF_lookspring 0x80B96FA8 - 0x80B96DEC
#define NF_sentryx 0x81112E94 - 0x81112CE0
#define NF_sentryy 0x81112E90 - 0x81112CE0
// Equinox final mission camera basis offsets from playerbase
#define NF_equinox_rightx 0x70
#define NF_equinox_righty 0x74
#define NF_equinox_rightz 0x78
#define NF_equinox_upx 0x7C
#define NF_equinox_upy 0x80
#define NF_equinox_upz 0x84
#define NF_equinox_forwardx 0x88
#define NF_equinox_forwardy 0x8C
#define NF_equinox_forwardz 0x90
// STATIC ADDRESSES BELOW
#define NF_playerbase 0x802BE87C // playable character pointer
#define NF_sentrybase 0x803780DC // sentry interface pointer (heli/jet ski)
#define NF_sentryfov 0x802E441C // sentry fov
#define NF_pauseflag 0x8024C2B4

typedef struct
{
	float x;
	float y;
	float z;
} VEC3;

static VEC3 NF_ReadVec3(const uint32_t base, const uint32_t xoffset);
static void NF_WriteVec3(const uint32_t base, const uint32_t xoffset, const VEC3 value);
static float NF_DotVec3(const VEC3 a, const VEC3 b);
static VEC3 NF_CrossVec3(const VEC3 a, const VEC3 b);
static VEC3 NF_ScaleVec3(const VEC3 value, const float scale);
static VEC3 NF_AddVec3(const VEC3 a, const VEC3 b);
static VEC3 NF_NormalizeVec3(const VEC3 value);
static VEC3 NF_RotateVec3(const VEC3 value, const VEC3 axis, const float angle);
static uint8_t NF_IsValidVec3(const VEC3 value);
static uint8_t NF_IsEquinoxCamera(const uint32_t playerbase, const float camy, const float fov);
static uint8_t NF_Status(void);
static void NF_InjectEquinoxAim(const uint32_t playerbase, const float looksensitivity);
static void NF_Inject(void);

static const GAMEDRIVER GAMEDRIVER_INTERFACE =
{
	"007: NightFire",
	NF_Status,
	NF_Inject,
	1, // 1000 Hz tickrate
	1, // crosshair sway supported for driver
	0,
	0
};

const GAMEDRIVER *GAME_NF = &GAMEDRIVER_INTERFACE;

//==========================================================================
// Purpose: read a 3-float vector from adjacent Equinox camera basis offsets
//==========================================================================
static VEC3 NF_ReadVec3(const uint32_t base, const uint32_t xoffset)
{
	VEC3 value;
	value.x = MEM_ReadFloat(base + xoffset);
	value.y = MEM_ReadFloat(base + xoffset + 4);
	value.z = MEM_ReadFloat(base + xoffset + 8);
	return value;
}
//==========================================================================
// Purpose: write a 3-float vector to adjacent Equinox camera basis offsets
//==========================================================================
static void NF_WriteVec3(const uint32_t base, const uint32_t xoffset, const VEC3 value)
{
	MEM_WriteFloat(base + xoffset, value.x);
	MEM_WriteFloat(base + xoffset + 4, value.y);
	MEM_WriteFloat(base + xoffset + 8, value.z);
}
//==========================================================================
// Purpose: vector math helpers for Equinox's full camera basis
//==========================================================================
static float NF_DotVec3(const VEC3 a, const VEC3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static VEC3 NF_CrossVec3(const VEC3 a, const VEC3 b)
{
	VEC3 value;
	value.x = a.y * b.z - a.z * b.y;
	value.y = a.z * b.x - a.x * b.z;
	value.z = a.x * b.y - a.y * b.x;
	return value;
}

static VEC3 NF_ScaleVec3(const VEC3 value, const float scale)
{
	VEC3 scaled;
	scaled.x = value.x * scale;
	scaled.y = value.y * scale;
	scaled.z = value.z * scale;
	return scaled;
}

static VEC3 NF_AddVec3(const VEC3 a, const VEC3 b)
{
	VEC3 value;
	value.x = a.x + b.x;
	value.y = a.y + b.y;
	value.z = a.z + b.z;
	return value;
}

static VEC3 NF_NormalizeVec3(const VEC3 value)
{
	const float length = sqrtf(NF_DotVec3(value, value));
	if(length < 0.5f || length > 1.5f || !isfinite(length))
		return value;
	return NF_ScaleVec3(value, 1.f / length);
}

static VEC3 NF_RotateVec3(const VEC3 value, const VEC3 axis, const float angle)
{
	const float c = cosf(angle);
	const float s = sinf(angle);
	const float d = NF_DotVec3(axis, value);
	VEC3 rotated = NF_AddVec3(NF_ScaleVec3(value, c), NF_ScaleVec3(NF_CrossVec3(axis, value), s));
	rotated = NF_AddVec3(rotated, NF_ScaleVec3(axis, d * (1.f - c)));
	return rotated;
}

static uint8_t NF_IsValidVec3(const VEC3 value)
{
	return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}
//==========================================================================
// Purpose: detect Equinox's final-level camera basis without using [8]
//==========================================================================
static uint8_t NF_IsEquinoxCamera(const uint32_t playerbase, const float camy, const float fov)
{
	if(fabsf(camy) > 0.0001f || fov < 0.99f || fov > 1.01f)
		return 0;

	VEC3 right = NF_ReadVec3(playerbase, NF_equinox_rightx);
	VEC3 up = NF_ReadVec3(playerbase, NF_equinox_upx);
	VEC3 forward = NF_ReadVec3(playerbase, NF_equinox_forwardx);

	if(!NF_IsValidVec3(right) || !NF_IsValidVec3(up) || !NF_IsValidVec3(forward))
		return 0;

	const float rightlength = NF_DotVec3(right, right);
	const float uplength = NF_DotVec3(up, up);
	const float forwardlength = NF_DotVec3(forward, forward);
	if(rightlength < 0.75f || rightlength > 1.25f || uplength < 0.75f || uplength > 1.25f || forwardlength < 0.75f || forwardlength > 1.25f)
		return 0;

	right = NF_NormalizeVec3(right);
	up = NF_NormalizeVec3(up);
	forward = NF_NormalizeVec3(forward);

	if(fabsf(NF_DotVec3(right, up)) > 0.35f || fabsf(NF_DotVec3(right, forward)) > 0.35f || fabsf(NF_DotVec3(up, forward)) > 0.35f)
		return 0;

	return NF_DotVec3(NF_NormalizeVec3(NF_CrossVec3(forward, right)), up) > 0.65f;
}
//==========================================================================
// Purpose: return 1 if game is detected
//==========================================================================
static uint8_t NF_Status(void)
{
	return (MEM_ReadUInt(0x80000000) == 0x474F3745U && MEM_ReadUInt(0x80000004) == 0x36390000U); // check game header to see if it matches NF
}
//==========================================================================
// Purpose: inject mouse movement into Equinox's crosshair-style aiming mode
//==========================================================================
static void NF_InjectEquinoxAim(const uint32_t playerbase, const float looksensitivity)
{
	VEC3 right = NF_ReadVec3(playerbase, NF_equinox_rightx);
	VEC3 up = NF_ReadVec3(playerbase, NF_equinox_upx);
	VEC3 forward = NF_ReadVec3(playerbase, NF_equinox_forwardx);

	if(!NF_IsValidVec3(right) || !NF_IsValidVec3(up) || !NF_IsValidVec3(forward))
		return;

	right = NF_NormalizeVec3(right);
	up = NF_NormalizeVec3(up);
	forward = NF_NormalizeVec3(forward);

	const float yaw = -(float)xmouse / 10.f * looksensitivity / (360.f / TAU);
	const float pitch = (float)(!invertpitch ? ymouse : -ymouse) / 10.f * looksensitivity / (360.f / TAU);

	if(xmouse)
	{
		right = NF_RotateVec3(right, up, yaw);
		forward = NF_RotateVec3(forward, up, yaw);
	}

	if(ymouse)
	{
		up = NF_RotateVec3(up, right, pitch);
		forward = NF_RotateVec3(forward, right, pitch);
	}

	right = NF_NormalizeVec3(right);
	forward = NF_NormalizeVec3(forward);
	up = NF_NormalizeVec3(NF_CrossVec3(forward, right));
	forward = NF_NormalizeVec3(NF_CrossVec3(right, up));

	NF_WriteVec3(playerbase, NF_equinox_rightx, right);
	NF_WriteVec3(playerbase, NF_equinox_upx, up);
	NF_WriteVec3(playerbase, NF_equinox_forwardx, forward);
	MEM_WriteFloat(playerbase + NF_camx, atan2f(forward.x, forward.z));
}
//==========================================================================
// Purpose: calculate mouse look and inject into current game
//==========================================================================
static void NF_Inject(void)
{
	if(xmouse == 0 && ymouse == 0) // if mouse is idle
		return;
	const uint32_t playerbase = MEM_ReadUInt(NF_playerbase);
	const float looksensitivity = (float)sensitivity / 40.f;
	const float crosshairsensitivity = ((float)crosshair / 100.f) * looksensitivity;
	if(WITHINMEMRANGE(playerbase)) // if playerbase is valid
	{
		if(MEM_ReadInt(playerbase + NF_lookspring) == 0x03010002) // disable lookspring when spawned
			MEM_WriteInt(playerbase + NF_lookspring, 0x01010002);
		float camx = MEM_ReadFloat(playerbase + NF_camx);
		float camy = MEM_ReadFloat(playerbase + NF_camy);
		const float fov = MEM_ReadFloat(playerbase + NF_fov);
		const float hp = MEM_ReadFloat(playerbase + NF_health);
		const uint32_t pauseflag = MEM_ReadUInt(NF_pauseflag);
		if(pauseflag)
			return;
		if(NF_IsEquinoxCamera(playerbase, camy, fov)) // Equinox final mission uses a full camera basis
		{
			NF_InjectEquinoxAim(playerbase, looksensitivity);
			return;
		}
		if(camx >= -PI && camx <= PI && camy >= -1.f && camy <= 1.f && fov >= 1.f && hp > 0)
		{
			camx -= (float)xmouse / 10.f * looksensitivity / (360.f / TAU) / (fov / 1.f); // normal calculation method for X
			camy += (float)(!invertpitch ? -ymouse : ymouse) / 10.f * looksensitivity / 90.f / (fov / 1.f); // normal calculation method for Y
			while(camx <= -PI)
				camx += TAU;
			while(camx >= PI)
				camx -= TAU;
			camy = ClampFloat(camy, -1.f, 1.f);
			MEM_WriteFloat(playerbase + NF_camx, camx);
			MEM_WriteFloat(playerbase + NF_camy, camy);
			if(crosshair) // if crosshair sway is enabled
			{
				float crosshairx = MEM_ReadFloat(playerbase + NF_crosshairx); // after camera x and y have been calculated and injected, calculate the crosshair/gun sway
				float crosshairy = MEM_ReadFloat(playerbase + NF_crosshairy);
				crosshairx += (float)xmouse / 80.f * crosshairsensitivity / (fov / 1.f);
				crosshairy += (float)(!invertpitch ? -ymouse : ymouse) / 80.f * crosshairsensitivity / (fov / 1.f);
				MEM_WriteFloat(playerbase + NF_crosshairx, ClampFloat(crosshairx, -CROSSHAIRX, CROSSHAIRX));
				MEM_WriteFloat(playerbase + NF_crosshairy, ClampFloat(crosshairy, -CROSSHAIRY, CROSSHAIRY));
			}
		}
	}
	else // if playerbase is invalid, check for sentry mode
	{
		const uint32_t sentrybase = MEM_ReadUInt(NF_sentrybase);
		if(NOTWITHINMEMRANGE(sentrybase)) // if sentrybase is invalid
			return;
		float sentryx = MEM_ReadFloat(sentrybase + NF_sentryx);
		float sentryy = MEM_ReadFloat(sentrybase + NF_sentryy);
		const float fov = MEM_ReadFloat(NF_sentryfov);
		if(sentryx >= -1.f && sentryx <= 1.f)
		{
			sentryx += (float)xmouse / 10.f * looksensitivity / 360.f / (SENTRYFOVBASE / fov);
			sentryy += (float)(!invertpitch ? ymouse : -ymouse) / 10.f * looksensitivity / (90.f / (SENTRYMAXY - SENTRYMINY)) / (SENTRYFOVBASE / fov);
			while(sentryx <= -1.f)
				sentryx += 1.f;
			while(sentryx >= 1.f)
				sentryx -= 1.f;
			sentryy = ClampFloat(sentryy, SENTRYMINY, SENTRYMAXY);
			MEM_WriteFloat(sentrybase + NF_sentryx, sentryx);
			MEM_WriteFloat(sentrybase + NF_sentryy, sentryy);
		}
	}
}
