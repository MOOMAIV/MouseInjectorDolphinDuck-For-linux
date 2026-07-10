//==========================================================================
// Mouse Injector for Dolphin - Linux Port
//==========================================================================
// Copyright (C) 2019-2020 Carnivorous
// Linux port (C) 2024 - ported from Windows using /proc + process_vm_readv
// All rights reserved.
//
// Mouse Injector is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//==========================================================================
/* _GNU_SOURCE set by Makefile -D flag */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/uio.h>   /* process_vm_readv / process_vm_writev */
#include "main.h"
#include "memory.h"

/* -----------------------------------------------------------------------
 * Internal state – Linux equivalent of HANDLE emuhandle
 * -------------------------------------------------------------------- */
static uint64_t emuoffset   = 0;
static uint32_t aramoffset  = 0x02000000;
static pid_t    emupid      = -1;

static int isPS1handle       = 0;
static int isN64handle       = 0;
static int isDolphinHandle   = 0;
static int isMupenhandle     = 0;
static int isBSNEShandle     = 0;
static int isPcsx2handle     = 0;
static int isRetroArchHandle = 0;
static int isKronosHandle    = 0;
static int isFlycastHandle   = 0;
static int isBSNESMercuryHandle = 0;
static int isRPCS3Handle     = 0;
static int isPPSSPPHandle    = 0;
static int isProject64Handle = 0;   /* via Wine only */

char hookedEmulatorName[80];

/* -----------------------------------------------------------------------
 * Low-level helpers
 * -------------------------------------------------------------------- */
static void MEM_ByteSwap32(uint32_t *input)
{
    const uint8_t *a = (const uint8_t *)input;
    *input = (uint32_t)((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]);
}

/* Generic process memory read via process_vm_readv */
static ssize_t mem_read(uintptr_t remote_addr, void *buf, size_t len)
{
    struct iovec local_iov  = { buf,               len };
    struct iovec remote_iov = { (void *)remote_addr, len };
    return process_vm_readv(emupid, &local_iov, 1, &remote_iov, 1, 0);
}

/* Generic process memory write via process_vm_writev */
static ssize_t mem_write(uintptr_t remote_addr, const void *buf, size_t len)
{
    struct iovec local_iov  = { (void *)buf,        len };
    struct iovec remote_iov = { (void *)remote_addr, len };
    return process_vm_writev(emupid, &local_iov, 1, &remote_iov, 1, 0);
}

/* -----------------------------------------------------------------------
 * Process discovery – scan /proc for known emulator names
 * Returns the pid, or -1 if not found.
 * -------------------------------------------------------------------- */

/* Read the short command name from /proc/<pid>/comm (max 15 chars + \n) */
static int proc_comm(pid_t pid, char *out, size_t outlen)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)outlen, f)) { fclose(f); return 0; }
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return 1;
}

/* Read the full executable path symlink /proc/<pid>/exe -> basename */
static int proc_exe_basename(pid_t pid, char *out, size_t outlen)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    char buf[256];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    /* basename */
    const char *b = strrchr(buf, '/');
    snprintf(out, outlen, "%s", b ? b + 1 : buf);
    out[outlen - 1] = '\0';
    return 1;
}

/* Check if the process's maps contain a specific substring (for RetroArch
 * core detection from loaded .so names). */
static int proc_maps_contains(pid_t pid, const char *needle)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, needle)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* -----------------------------------------------------------------------
 * MEM_Init  – find a supported emulator process and open it
 * -------------------------------------------------------------------- */
uint8_t MEM_Init(void)
{
    emupid = -1;
    memset(hookedEmulatorName, 0, sizeof(hookedEmulatorName));

    /* Reset all handle flags */
    isPS1handle = isN64handle = isMupenhandle = isBSNEShandle = 0;
    isDolphinHandle = 0;
    isPcsx2handle = isRetroArchHandle = isKronosHandle = 0;
    isFlycastHandle = isBSNESMercuryHandle = isRPCS3Handle = 0;
    isPPSSPPHandle = isProject64Handle = 0;

    DIR *proc = opendir("/proc");
    if (!proc) return 0;

    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL)
    {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);

        char comm[64] = {0};
        char exe[256] = {0};
        proc_comm(pid, comm, sizeof(comm));
        proc_exe_basename(pid, exe, sizeof(exe));

        /* Helper: match either comm or exe basename */
#define MATCH(s) (strcmp(comm,(s))==0 || strcmp(exe,(s))==0)

        /* "dolphin" alone intentionally excluded — that is the KDE file manager */
        if (MATCH("dolphin-emu") || MATCH("dolphin-emu-qt") ||
            strstr(exe, "dolphin-emu") != NULL)
        {
            strcpy(hookedEmulatorName, "Dolphin");
            isDolphinHandle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("duckstation-qt") || MATCH("duckstation-nogui") ||
            /* Wine */ MATCH("duckstation-qt-"))
        {
            strcpy(hookedEmulatorName, "DuckStation");
            isPS1handle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("pcsx2") || MATCH("PCSX2") ||
            MATCH("pcsx2-qt") || MATCH("pcsx2-qtx64") || MATCH("pcsx2-avx2"))
        {
            strcpy(hookedEmulatorName, "PCSX2");
            isPcsx2handle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("retroarch"))
        {
            strcpy(hookedEmulatorName, "RetroArch - No core loaded");
            isRetroArchHandle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("PPSSPP") || MATCH("PPSSPPQt"))
        {
            strcpy(hookedEmulatorName, "PPSSPP");
            isPPSSPPHandle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("simple64-gui") || MATCH("simple64"))
        {
            strcpy(hookedEmulatorName, "simple64");
            isN64handle = 1;
            isMupenhandle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("RMG"))
        {
            strcpy(hookedEmulatorName, "Rosalie's Mupen GUI");
            isN64handle = 1;
            isMupenhandle = 1;
            emupid = pid;
            break;
        }
        if (MATCH("rpcs3"))
        {
            strcpy(hookedEmulatorName, "RPCS3");
            isRPCS3Handle = 1;
            emupid = pid;
            break;
        }
        /* flycast standalone */
        if (MATCH("flycast"))
        {
            strcpy(hookedEmulatorName, "Flycast");
            isFlycastHandle = 1;
            emupid = pid;
            break;
        }
#undef MATCH
    }
    closedir(proc);
    return (emupid != -1) ? 1 : 0;
}

void MEM_Quit(void)
{
    /* Nothing to close on Linux – no HANDLE to release */
    emupid = -1;
}

/* -----------------------------------------------------------------------
 * MEM_FindRamOffset
 * Parse /proc/<pid>/maps to find the emulator's console RAM region.
 * Equivalent to the Windows VirtualQueryEx + QueryWorkingSetEx loop.
 * -------------------------------------------------------------------- */

/* A parsed line from /proc/<pid>/maps */
typedef struct {
    uintptr_t   start;
    uintptr_t   end;
    char        perms[8];   /* "rwxp" style */
    int         is_private; /* perms[3]=='p' */
    int         is_anon;    /* no device/inode (00:00 0) */
    char        name[256];  /* pathname or empty */
} MapsRegion;

static int parse_maps_line(const char *line, MapsRegion *r)
{
    unsigned long long start, end;
    unsigned int dev_maj, dev_min;
    unsigned long inode;
    char perms[8] = {0}, name[256] = {0};
    int n = sscanf(line, "%llx-%llx %7s %*x %x:%x %lu %255[^\n]",
                   &start, &end, perms, &dev_maj, &dev_min, &inode, name);
    if (n < 6) return 0;
    r->start      = (uintptr_t)start;
    r->end        = (uintptr_t)end;
    snprintf(r->perms, sizeof(r->perms), "%s", perms);
    r->is_private = (perms[3] == 'p');
    r->is_anon    = (dev_maj == 0 && dev_min == 0 && inode == 0);
    if (n >= 7)
        snprintf(r->name, sizeof(r->name), "%s", name);
    else
        r->name[0] = '\0';
    return 1;
}

/* Verify a candidate region is readable */
static int region_is_valid(uintptr_t addr)
{
    uint32_t test = 0;
    return (mem_read(addr, &test, sizeof(test)) == (ssize_t)sizeof(test));
}

/* -----------------------------------------------------------------------
 * Dolphin (GC/Wii) disc-header verification.
 *
 * On Linux, Dolphin's MemArena maps the 24 MB MEM1 region into a
 * 32 MB (0x2000000) virtual view — the same size used by the "FakeVMem"
 * placeholder region.  Both can appear as readable rw-shared mappings of
 * identical size, so a size-only match can land on the wrong one (which
 * just contains zeros/garbage), causing every game driver's Status()
 * check to read nonsense and never match.
 *
 * Every booted GC or Wii disc has a fixed 4-byte magic word at offset
 * 0x1C in its disc header (mapped at GC address 0x8000001C) – this is
 * the same value Dolphin itself checks when verifying a valid boot
 * image. We use it to confirm a size-matched candidate is *actually*
 * backing the console's RAM before accepting it.
 * -------------------------------------------------------------------- */
#define GC_DISC_MAGIC  0xC2339F3DU
#define WII_DISC_MAGIC 0x5D1C9EA3U

static int dolphin_region_looks_valid(uintptr_t candidate)
{
    uint32_t magic = 0;
    /* offset 0x1C from the start of MEM1 == GC address 0x8000001C */
    if (mem_read(candidate + 0x1C, &magic, sizeof(magic)) != (ssize_t)sizeof(magic))
        return 0;
    MEM_ByteSwap32(&magic); /* GC/Wii memory is big-endian */
    return (magic == GC_DISC_MAGIC || magic == WII_DISC_MAGIC);
}

/* -----------------------------------------------------------------------
 * Detect RetroArch loaded core from maps (replaces window title check)
 * -------------------------------------------------------------------- */
static void retroarch_detect_core(void)
{
    /* Ordered by priority – first match wins */
    if (proc_maps_contains(emupid, "mupen64plus") ||
        proc_maps_contains(emupid, "mupen64plus_next"))
    {
        strcpy(hookedEmulatorName, "RetroArch Mupen64Plus-Next");
        isN64handle   = 1;
        isMupenhandle = 1;
        return;
    }
    if (proc_maps_contains(emupid, "kronos_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch Kronos");
        isKronosHandle = 1;
        return;
    }
    if (proc_maps_contains(emupid, "mednafen_psx_hw_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch Beetle PSX HW");
        return;
    }
    if (proc_maps_contains(emupid, "mednafen_psx_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch Beetle PSX");
        return;
    }
    if (proc_maps_contains(emupid, "pcsx_rearmed_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch PCSX-ReARMed");
        return;
    }
    if (proc_maps_contains(emupid, "duckstation_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch DuckStation");
        return;
    }
    if (proc_maps_contains(emupid, "swanstation_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch SwanStation");
        return;
    }
    if (proc_maps_contains(emupid, "bsnes_mercury"))
    {
        strcpy(hookedEmulatorName, "RetroArch bsnes-mercury");
        isBSNESMercuryHandle = 1;
        return;
    }
    if (proc_maps_contains(emupid, "flycast_libretro"))
    {
        strcpy(hookedEmulatorName, "RetroArch Flycast");
        isFlycastHandle = 1;
        return;
    }
}

uint8_t MEM_FindRamOffset(void)
{
    emuoffset = 0;

    if (emupid == -1) return 0;

    /* For RetroArch, detect which core is loaded */
    if (isRetroArchHandle)
        retroarch_detect_core();

    /* ------------------------------------------------------------
     * Target region sizes per emulator.
     *
     * Dolphin on Linux uses memfd_create("dolphin-emu",0)+MAP_SHARED,
     * so the region appears as "rw-s" with a real inode, NOT anonymous.
     * We therefore do NOT filter on is_anon; size + rw + a test-read
     * is the reliable discriminator on Linux.
     *
     * PCSX2: Windows looked for a 0x1000 region using surrounding-region
     * ordering that doesn't translate to Linux.  On Linux the EE RAM is a
     * straightforward 0x2000000 anonymous mapping – use that instead.
     * ------------------------------------------------------------ */
    uint64_t emuRegionSize = 0x2000000; /* Dolphin GC/Wii MEM1+MEM2 */

    if (isPS1handle)
        emuRegionSize = 0x800000;
    else if (isN64handle)
        emuRegionSize = isMupenhandle ? 0x20011000 : 0x22D0000;
    else if (isBSNEShandle)
        emuRegionSize = 0x34000;
    else if (isPcsx2handle)
        emuRegionSize = 0x2000000;   /* Linux: 32 MB EE RAM (not 0x1000) */
    else if (isFlycastHandle)
        emuRegionSize = 0x10000;
    else if (isRPCS3Handle)
        emuRegionSize = 0xCC00000;
    else if (isPPSSPPHandle)
        emuRegionSize = 0x1F00000;
    else if (isProject64Handle)
        emuRegionSize = 0x800000;

    /* Open /proc/<pid>/maps */
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)emupid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        fprintf(stderr, "[mi] cannot open %s: check permissions\n", maps_path);
        return 0;
    }

    char line[512];
    MapsRegion r;
    uint64_t lastRegionSize = 0;
    static int debug_call_counter = 0;
    const int debug = (getenv("MOUSEINJECTOR_DEBUG") != NULL) &&
                       (debug_call_counter++ % 10 == 0); /* ~once/sec at 100ms poll */

    if (debug)
        fprintf(stderr, "[mi-debug] scanning for %s region size 0x%lX\n",
                hookedEmulatorName, (unsigned long)emuRegionSize);

    while (fgets(line, sizeof(line), fp))
    {
        if (!parse_maps_line(line, &r)) continue;

        uint64_t regionSize = r.end - r.start;

        /* Must have read+write permission.
         * 's' (shared) is allowed – that is how Dolphin maps its RAM. */
        if (r.perms[0] != 'r' || r.perms[1] != 'w') {
            lastRegionSize = regionSize;
            continue;
        }

        if (regionSize != emuRegionSize) {
            lastRegionSize = regionSize;
            continue;
        }

        if (debug)
            fprintf(stderr, "[mi-debug] size match at 0x%lx perms=%s name=%s\n",
                    (unsigned long)r.start, r.perms, r.name);

        /* ---- size matches: apply emulator-specific rules ---- */
        uintptr_t candidate = r.start;

        if (isBSNESMercuryHandle) {
            /* BSNES-mercury: specific preceding-region size (same on Linux) */
            if (lastRegionSize != 0xB7000) goto next;
            candidate += 0x7F1C;

        } else if (isBSNEShandle) {
            candidate += 0x2D7C;

        } else if (isN64handle && isMupenhandle) {
            /* simple64 / RMG: first 0x1000 is metadata; scan for RDRAM */
            candidate += 0x1000;
            emuoffset   = candidate;
            int steps   = 256;
            while (steps-- > 0) {
                uint32_t probe = 0;
                if (mem_read(emuoffset, &probe, 4) == 4 && probe != 0) break;
                emuoffset += 0x1000;
            }
            if (region_is_valid(emuoffset)) { fclose(fp); return 1; }
            emuoffset = 0;
            goto next;

        } else if (isFlycastHandle) {
            /* Flycast: region must be preceded by another 0x2000000 block */
            if (lastRegionSize != 0x2000000) goto next;

        } else if (isRPCS3Handle) {
            if (lastRegionSize != 0xFF70000) goto next;

        } else if (isPcsx2handle) {
            /* On Linux we just take the first 32 MB rw region; the
             * Windows ordering heuristic (lastRegionSize == 0x80000) does
             * not apply here. */
            (void)lastRegionSize;
        }
        /* DuckStation, PPSSPP, RetroArch cores, etc.:
         * no extra ordering constraint – validate with a test read. */

        if (!region_is_valid(candidate)) {
            if (debug) fprintf(stderr, "[mi-debug]   -> not readable, skip\n");
            goto next;
        }

        if (isDolphinHandle) {
            /* Multiple 0x2000000 regions exist in Dolphin's Linux address
             * space (the real MEM1 view AND the same-sized FakeVMem
             * placeholder). Confirm this candidate is actually backing
             * RAM before accepting it — otherwise keep scanning, since
             * the real MEM1 view may appear later in the maps file. */
            if (!dolphin_region_looks_valid(candidate)) {
                if (debug) fprintf(stderr, "[mi-debug]   -> magic check failed, skip\n");
                goto next;
            }
        }

        if (debug) fprintf(stderr, "[mi-debug]   -> ACCEPTED as emuoffset\n");
        emuoffset = candidate;
        fclose(fp);
        return 1;

    next:
        lastRegionSize = regionSize;
    }

    fclose(fp);
    if (debug)
        fprintf(stderr, "[mi-debug] no matching region found this pass\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Main GC / Wii RAM  (MEM1, with byte-swap)
 * -------------------------------------------------------------------- */
int32_t MEM_ReadInt(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return 0;
    int32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    MEM_ByteSwap32((uint32_t *)&output);
    return output;
}
uint32_t MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    MEM_ByteSwap32(&output);
    return output;
}
uint16_t MEM_ReadUInt16(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return 0;
    uint16_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    return output;
}
uint8_t MEM_ReadUInt8(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return 0;
    uint8_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    return output;
}
float MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    MEM_ByteSwap32((uint32_t *)&output);
    return output;
}
void MEM_WriteInt(const uint32_t addr, int32_t value)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return;
    MEM_ByteSwap32((uint32_t *)&value);
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}
void MEM_WriteUInt(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return;
    MEM_ByteSwap32(&value);
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}
void MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || NOTWITHINMEMRANGE(addr)) return;
    MEM_ByteSwap32((uint32_t *)&value);
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * ARAM (GameCube Audio RAM)
 * -------------------------------------------------------------------- */
int32_t ARAM_ReadInt(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINARAMRANGE(addr)) return 0;
    int32_t output = 0;
    mem_read(emuoffset + aramoffset + (addr - 0x7E000000), &output, sizeof(output));
    MEM_ByteSwap32((uint32_t *)&output);
    return output;
}
uint32_t ARAM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINARAMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + aramoffset + (addr - 0x7E000000), &output, sizeof(output));
    MEM_ByteSwap32(&output);
    return output;
}
float ARAM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || NOTWITHINARAMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + aramoffset + (addr - 0x7E000000), &output, sizeof(output));
    MEM_ByteSwap32((uint32_t *)&output);
    return output;
}
void ARAM_WriteUInt(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || NOTWITHINARAMRANGE(addr)) return;
    MEM_ByteSwap32(&value);
    mem_write(emuoffset + aramoffset + (addr - 0x7E000000), &value, sizeof(value));
}
void ARAM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || NOTWITHINARAMRANGE(addr)) return;
    MEM_ByteSwap32((uint32_t *)&value);
    mem_write(emuoffset + aramoffset + (addr - 0x7E000000), &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * PS1 memory (little-endian, no byte-swap)
 * -------------------------------------------------------------------- */
uint32_t PS1_MEM_ReadPointer(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return (output - 0x80000000);
}
uint32_t PS1_MEM_ReadWord(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    MEM_ByteSwap32(&output);
    return output;
}
uint32_t PS1_MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
int32_t PS1_MEM_ReadInt(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    int32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
int16_t PS1_MEM_ReadInt16(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    int16_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint16_t PS1_MEM_ReadHalfword(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    uint16_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint8_t PS1_MEM_ReadByte(const uint32_t addr)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return 0;
    uint8_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
void PS1_MEM_WriteInt(const uint32_t addr, int32_t value)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void PS1_MEM_WriteInt16(const uint32_t addr, int16_t value)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void PS1_MEM_WriteWord(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void PS1_MEM_WriteHalfword(const uint32_t addr, uint16_t value)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void PS1_MEM_WriteByte(const uint32_t addr, uint8_t value)
{
    if (!emuoffset || PS1NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * N64 memory (little-endian in mupen64plus, no byte-swap)
 * -------------------------------------------------------------------- */
uint32_t N64_MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    return output;
}
int16_t N64_MEM_ReadInt16(const uint32_t addr)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return 0;
    int16_t output = 0;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    return output;
}
float N64_MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + (addr - 0x80000000), &output, sizeof(output));
    return output;
}
void N64_MEM_WriteUInt(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}
void N64_MEM_WriteInt16(const uint32_t addr, int16_t value)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}
void N64_MEM_WriteByte(const uint32_t addr, uint8_t value)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}
void N64_MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || N64NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000000), &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * SNES memory
 * -------------------------------------------------------------------- */
uint8_t SNES_MEM_ReadByte(const uint32_t addr)
{
    if (!emuoffset || SNESNOTWITHINMEMRANGE(addr)) return 0;
    uint8_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint16_t SNES_MEM_ReadWord(const uint32_t addr)
{
    if (!emuoffset || SNESNOTWITHINMEMRANGE(addr)) return 0;
    uint16_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
void SNES_MEM_WriteByte(const uint32_t addr, uint8_t value)
{
    if (!emuoffset || SNESNOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void SNES_MEM_WriteWord(const uint32_t addr, uint16_t value)
{
    if (!emuoffset || SNESNOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * PS2 memory
 * -------------------------------------------------------------------- */
uint32_t PS2_MEM_ReadPointer(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
uint32_t PS2_MEM_ReadWord(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
uint32_t PS2_MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
uint32_t PS2_MEM_ReadUInt16(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    uint16_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return (uint32_t)output;
}
int16_t PS2_MEM_ReadInt16(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    int16_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
uint8_t PS2_MEM_ReadUInt8(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    uint8_t output = 0;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
float PS2_MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + (addr - 0x80000), &output, sizeof(output));
    return output;
}
void PS2_MEM_WriteWord(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000), &value, sizeof(value));
}
void PS2_MEM_WriteUInt(const uint32_t addr, uint32_t value)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000), &value, sizeof(value));
}
void PS2_MEM_WriteUInt16(const uint32_t addr, uint16_t value)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000), &value, sizeof(value));
}
void PS2_MEM_WriteInt16(const uint32_t addr, int16_t value)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000), &value, sizeof(value));
}
void PS2_MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || PS2NOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + (addr - 0x80000), &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * Sega Dreamcast (SD) memory
 * -------------------------------------------------------------------- */
uint32_t SD_MEM_ReadWord(const uint32_t addr)
{
    if (!emuoffset) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
float SD_MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset) return 0;
    float output = 0.0f;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
void SD_MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * PS3 memory (via RPCS3)
 * -------------------------------------------------------------------- */
uint32_t PS3_MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || PS3NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    MEM_ByteSwap32(&output);
    return output;
}
float PS3_MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || PS3NOTWITHINMEMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + addr, &output, sizeof(output));
    MEM_ByteSwap32((uint32_t *)&output);
    return output;
}
uint32_t PS3_MEM_ReadPointer(const uint32_t addr)
{
    if (!emuoffset || PS3NOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    MEM_ByteSwap32(&output);
    return output;
}
void PS3_MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || PS3NOTWITHINMEMRANGE(addr)) return;
    MEM_ByteSwap32((uint32_t *)&value);
    mem_write(emuoffset + addr, &value, sizeof(value));
}

/* -----------------------------------------------------------------------
 * PSP memory (via PPSSPP)
 * -------------------------------------------------------------------- */
uint32_t PSP_MEM_ReadWord(const uint32_t addr)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint32_t PSP_MEM_ReadPointer(const uint32_t addr)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint32_t PSP_MEM_ReadUInt(const uint32_t addr)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return 0;
    uint32_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
uint16_t PSP_MEM_ReadUInt16(const uint32_t addr)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return 0;
    uint16_t output = 0;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
float PSP_MEM_ReadFloat(const uint32_t addr)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return 0;
    float output = 0.0f;
    mem_read(emuoffset + addr, &output, sizeof(output));
    return output;
}
void PSP_MEM_WriteUInt16(const uint32_t addr, uint16_t value)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}
void PSP_MEM_WriteFloat(const uint32_t addr, float value)
{
    if (!emuoffset || PSPNOTWITHINMEMRANGE(addr)) return;
    mem_write(emuoffset + addr, &value, sizeof(value));
}

/* printdebug stub (used in commented-out debug lines in original) */
void printdebug(uint32_t val) { (void)val; }
