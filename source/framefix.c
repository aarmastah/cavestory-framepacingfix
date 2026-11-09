/*
 * Cave Story (Doukutsu.exe, 2004/2005 PC release) frame pacing +
 * frame interpolation fix
 * ------------------------------------------------------------------
 * Drop-in proxy for dinput.dll. Place next to the game's exe.
 *
 * 1. Frame pacing
 *    Flip_SystemTask() (0x40B340) paces the game with
 *        while (GetTickCount() < last + 20) Sleep(1);
 *    On NT-based Windows GetTickCount() only advances every ~15.6 ms, so
 *    frames alternate between ~16 and ~31 ms. The wait loop is replaced
 *    with a QueryPerformanceCounter-based scheduler. The game's own frame
 *    interval (the "+20") is read from the exe every tick so mods that
 *    change the frame rate keep their speed, or framefix.ini can force one.
 *
 * 2. Frame interpolation
 *    The game logic still runs at its fixed rate (50 fps in vanilla), but
 *    frames are presented once per monitor refresh, in sync with the
 *    display. Each presented frame is drawn by the game's own drawing code
 *    with every entity and the camera placed between their positions in
 *    the last two logic ticks, so motion is smooth at 60/120/144/240 Hz
 *    while the game speed is unchanged.
 *
 *    How it works:
 *    - The 12 "world" drawing functions (PutBack, PutStage_Back, PutNpChar,
 *      PutBossChar, PutBullet, PutMyChar, PutStar, PutMapDataVector,
 *      PutStage_Front, PutFront, PutCaret, PutValueView) are hooked at
 *      their entry. All take the camera position (fx, fy).
 *    - The back buffer's DirectDraw Blt/BltFast are hooked, so everything
 *      else drawn to the back buffer (HUD, text box, fades, menus, and any
 *      custom drawing a mod does) is captured as raw screen-space blits.
 *    - Each logic tick is thus recorded as a list of "world draw (fx, fy)"
 *      and "raw blit" operations, in order.
 *    - For every display refresh, entity/camera positions are temporarily
 *      set to interpolated values, the tick's list is replayed into a
 *      private surface (world functions re-run by the game itself, raw
 *      blits repeated as-is), the result is presented, and every value
 *      that was changed is restored. Game state is never altered.
 *
 *    The scrolling cloud backgrounds (types 6/7, bkMoon/bkFog) use a whole-
 *    pixel counter that wraps at 640; it is interpolated the short way
 *    round, and the layers are redrawn with a fractional scroll position.
 *
 *    Interpolation is skipped (the real frame is shown) for objects that
 *    jump far in one tick (teleports, room changes), for ticks with no
 *    world drawing (title, menus) and for ticks that draw to the back
 *    buffer in a way that can't be replayed (GetDC/Lock).
 *
 * framefix.ini (optional, next to the exe):
 *     [FrameFix]
 *     FPS=0            ; 0 = game's own rate, or force e.g. 60 / 59.94
 *     Interpolation=1  ; 0 = pacing fix only, present at the game's rate
 *     ClearType=0      ; 0 = crisp, non-antialiased text (default)
 *                      ; 1 = leave text to Windows' font smoothing setting
 *
 * 3. Text rendering
 *    The game creates its font with DEFAULT_QUALITY, so when ClearType is on
 *    (the Windows default) the text box gets subpixel colour fringes. The
 *    game's font creation imports are redirected so fonts are created with
 *    NONANTIALIASED_QUALITY instead.
 */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* =================================================================== */
/*  dinput.dll forwarding                                              */
/* =================================================================== */

static HMODULE g_real;

static FARPROC RealProc(const char *name)
{
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 12) return NULL;
        lstrcatA(path, "\\dinput.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return NULL;
    }
    return GetProcAddress(g_real, name);
}

typedef HRESULT (WINAPI *PFN4)(HINSTANCE, DWORD, void *, void *);
typedef HRESULT (WINAPI *PFN5)(HINSTANCE, DWORD, const void *, void *, void *);
typedef HRESULT (WINAPI *PFN0)(void);
typedef HRESULT (WINAPI *PFN3)(const void *, const void *, void **);

HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE h, DWORD v, void *o, void *u)
{ PFN4 f = (PFN4)RealProc("DirectInputCreateA"); return f ? f(h, v, o, u) : E_FAIL; }
HRESULT WINAPI Proxy_DirectInputCreateW(HINSTANCE h, DWORD v, void *o, void *u)
{ PFN4 f = (PFN4)RealProc("DirectInputCreateW"); return f ? f(h, v, o, u) : E_FAIL; }
HRESULT WINAPI Proxy_DirectInputCreateEx(HINSTANCE h, DWORD v, const void *r, void *o, void *u)
{ PFN5 f = (PFN5)RealProc("DirectInputCreateEx"); return f ? f(h, v, r, o, u) : E_FAIL; }
HRESULT WINAPI Proxy_DllCanUnloadNow(void)
{ PFN0 f = (PFN0)RealProc("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI Proxy_DllGetClassObject(const void *c, const void *i, void **o)
{ PFN3 f = (PFN3)RealProc("DllGetClassObject"); return f ? f(c, i, o) : E_FAIL; }
HRESULT WINAPI Proxy_DllRegisterServer(void)
{ PFN0 f = (PFN0)RealProc("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI Proxy_DllUnregisterServer(void)
{ PFN0 f = (PFN0)RealProc("DllUnregisterServer"); return f ? f() : E_FAIL; }

/* =================================================================== */
/*  Game addresses (vanilla Doukutsu.exe 1.0.0.6 and mods built on it)  */
/* =================================================================== */

/* Flip_SystemTask */
#define PATCH_ADDR      0x0040B343
#define SYSTASK_CALL    0x0040B34C   /* E8 rel32 -> SystemTask */
#define QUIT_ADDR       0x0040B355   /* xor eax,eax; jmp epilogue */
#define CONTINUE_ADDR   0x0040B3B0   /* compute window rect, Blt to screen */
#define AFTERBLT_ADDR   0x0040B42D   /* lost-surface check after the Blt */
#define INTERVAL_INSN   0x0040B36D   /* add ecx, <frame ms>   (83 C1 ib / 81 C1 id) */
#define RESET_INSN      0x0040B38A   /* add edx, <resync ms>  (83 C2 ib / 81 C2 id) */

/* DirectDraw globals (Draw.cpp) */
#define G_LPDD          ((void **)0x0049D37C)
#define G_FRONTBUFFER   ((void **)0x0049D380)
#define G_BACKBUFFER    ((void **)0x0049D384)
#define G_SRC_RECT      ((RECT *)0x0049CDB0)
#define G_CLIENT_OFS_X  ((int *)0x0049CDAC)
#define G_CLIENT_OFS_Y  ((int *)0x0049CDA8)
#define G_SCREEN_W      ((int *)0x0049D374)
#define G_SCREEN_H      ((int *)0x0049D378)

/* World drawing functions: void __cdecl F(int fx, int fy) */
static const DWORD g_worldAddr[] = {
    0x004023D0, /* PutBack          */
    0x00413AF0, /* PutStage_Back    */
    0x00472770, /* PutBossChar      */
    0x0046F810, /* PutNpChar        */
    0x00403DC0, /* PutBullet        */
    0x00415250, /* PutMyChar        */
    0x004213B0, /* PutStar          */
    0x00413E40, /* PutMapDataVector */
    0x00413C60, /* PutStage_Front   */
    0x00402830, /* PutFront         */
    0x0040ABC0, /* PutCaret         */
    0x00426430, /* PutValueView     */
};
#define NUM_WORLD (int)(sizeof g_worldAddr / sizeof g_worldAddr[0])

/* Per-draw animation counters advanced inside world drawing functions;
   saved and restored around replays so extra draws don't speed them up. */
static const DWORD g_drawCounters[] = {
    0x0049E6CB, /* gMC bubble counter (PutMyChar)       */
    0x0049E58C, /* wind/current tile anim (PutMapDataVector) */
};

/* Scratch statics of the sprite blit helpers (PutBitmap3/PutBitmap4/
   PutBitmap3-with-surface). Overwritten on every call, but restored anyway
   so replaying leaves memory byte-for-byte as it was. */
#define SCRATCH_ADDR 0x0049D4B4
#define SCRATCH_SIZE (0x0049D514 - 0x0049D4B4)

/* Interpolated entity tables (positions are in 1/512 pixel units). */
typedef struct {
    DWORD base, stride;
    int   count;
    int   condOff;      /* byte; bit 0x80 = alive, -1 = always alive */
    int   codeOff;      /* identity field (int), -1 = none */
    int   xOff, yOff;
    int   maxDelta;     /* larger moves per tick are not interpolated */
} EntityTable;

static const EntityTable g_ent[] = {
    { 0x004A6220, 0xAC, 512, 0x00, 0x28, 0x08, 0x0C, 0x4000 }, /* gNPC  */
    { 0x004BBA58, 0xAC,  20, 0x00, 0x28, 0x08, 0x0C, 0x4000 }, /* gBoss */
    { 0x00499C98, 0x80,  64, 0x0C, 0x04, 0x10, 0x14, 0x4000 }, /* gBul  */
    { 0x0049BCA8, 0x44,  64, 0x00, 0x04, 0x0C, 0x10, 0x4000 }, /* gCrt  */
    { 0x0049E638, 0x00,   1, 0x00,   -1, 0x1C, 0x20, 0x4000 }, /* gMC   */
    { 0x004A5800, 0x44,   3,   -1,   -1, 0x0C, 0x10, 0x4000 }, /* gStar */
    { 0x0049E1C8, 0x00,   1,   -1,   -1, 0x00, 0x04, 0x8000 }, /* gFrame (camera) */
};
#define NUM_TABLES (int)(sizeof g_ent / sizeof g_ent[0])

/* Single scalar values. */
typedef struct { DWORD addr; int maxDelta; } Scalar;
static const Scalar g_scalars[] = {
    { 0x00499C90, 0x2000 },  /* gWaterY  (rising water level)         */
};

/* Background (gBack). Its scroll counter needs special handling, below. */
#define BACK_TYPE   ((int *)0x00499C88)
#define BACK_FX     ((int *)0x00499C8C)

/* PutBack's code for background types 6 and 7 (scrolling cloud layers,
   used with bkMoon / bkFog). gBack.fx counts whole pixels 0..639 and wraps,
   and the four layers scroll at fx/2, fx, fx*2 and fx*4, so the game can't
   draw them between whole-pixel steps. When this code is unmodified it is
   re-implemented below with a fractional fx, giving each layer smooth motion. */
#define CLOUD_CODE      0x00402606
#define CLOUD_CODE_LEN  0x1FF
#define CLOUD_CODE_FNV  0x15C931EBu
#define PUTBACK_JTAB    0x00402809       /* switch jump table, 8 entries */
#define PUTBITMAP4      0x0040C5B0       /* (RECT *view, int x, int y, RECT *src, int surf) */
#define GRC_GAME        ((RECT *)0x0048F91C)
#define SURF_BACKGROUND 0x1C
#define NUM_SCALARS (int)(sizeof g_scalars / sizeof g_scalars[0])

/* =================================================================== */
/*  Config & timing                                                     */
/* =================================================================== */

#define DEFAULT_FRAME_MS   20
#define DEFAULT_RESET_MS  100
#define SPIN_US          1200

typedef BOOL (__cdecl *SystemTask_t)(void);
static SystemTask_t SystemTask = (SystemTask_t)0x413570;

static LONGLONG g_freq;
static LONGLONG g_override_us;
static BOOL     g_interpEnabled = TRUE;
static BOOL     g_allowClearType = FALSE;
static HANDLE   g_timer;

static LONGLONG Now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static LONGLONG UsToTicks(LONGLONG us) { return us * g_freq / 1000000; }

static BOOL Readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

static LONG ReadAddImm(DWORD addr, BYTE modrm, LONG fallback, LONG lo, LONG hi)
{
    const BYTE *p = (const BYTE *)addr;
    LONG v;
    if (!Readable(p, 6) || p[1] != modrm) return fallback;
    if (p[0] == 0x83)      v = (signed char)p[2];
    else if (p[0] == 0x81) v = *(const LONG *)(p + 2);
    else return fallback;
    return (v >= lo && v <= hi) ? v : fallback;
}

static LONGLONG FrameUs(void)
{
    if (g_override_us) return g_override_us;
    return (LONGLONG)ReadAddImm(INTERVAL_INSN, 0xC1, DEFAULT_FRAME_MS, 1, 1000) * 1000;
}

static LONGLONG ResetUs(void)
{
    return (LONGLONG)ReadAddImm(RESET_INSN, 0xC2, DEFAULT_RESET_MS, 1, 10000) * 1000;
}

static void CoarseSleep(LONGLONG ticks)
{
    LONGLONG us = ticks * 1000000 / g_freq;
    if (g_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(us * 10);
        if (SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep(us >= 2000 ? (DWORD)(us / 1000) - 1 : 1);
}

static void SleepUntil(LONGLONG target)
{
    LONGLONG spin = UsToTicks(SPIN_US);
    for (;;) {
        LONGLONG remain = target - Now();
        if (remain <= 0) return;
        if (remain > spin) CoarseSleep(remain - spin);
        else { while (Now() < target) YieldProcessor(); return; }
    }
}

#ifdef FRAMEFIX_LOG
static HANDLE g_log;
static void Log(const char *fmt, ...)
{
    char buf[256]; DWORD w; va_list ap;
    if (!g_log) g_log = CreateFileA("framefix_log.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    WriteFile(g_log, buf, lstrlenA(buf), &w, NULL);
}
#else
#define Log(...) ((void)0)
#endif

/* =================================================================== */
/*  DirectDraw surface hooks: capture what is drawn to the back buffer  */
/* =================================================================== */

typedef HRESULT (WINAPI *Blt_t)(void *, LPRECT, void *, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (WINAPI *BltFast_t)(void *, DWORD, DWORD, void *, LPRECT, DWORD);
typedef HRESULT (WINAPI *GetDC_t)(void *, HDC *);
typedef HRESULT (WINAPI *Lock_t)(void *, LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);

#define VT_BLT      5
#define VT_BLTFAST  7
#define VT_GETDC   17
#define VT_ISLOST  24
#define VT_LOCK    25
#define VT_RESTORE 27

typedef struct { void **vt; Blt_t Blt; BltFast_t BltFast; GetDC_t GetDC; Lock_t Lock; } VtHook;
static VtHook g_vt[8];
static int    g_nvt;

static VtHook *FindVt(void *obj)
{
    void **vt = *(void ***)obj; int i;
    for (i = 0; i < g_nvt; i++) if (g_vt[i].vt == vt) return &g_vt[i];
    /* A wrapper may have copied our hooked vtable into a new one: the
       originals are the same implementation, so use the first entry. */
    return g_nvt ? &g_vt[0] : NULL;
}

/* Recording of one logic tick */
enum { OP_WORLD, OP_BLT, OP_BLTFAST };

typedef struct {
    BYTE  kind, hasDst, hasSrc, hasFx;
    int   world, fx, fy;          /* OP_WORLD */
    RECT  dst, src;
    void *srcSurf;
    DWORD flags, x, y;
    DDBLTFX bfx;
} Op;

#define MAX_OPS 4096
typedef struct {
    Op   ops[MAX_OPS];
    int  n;
    BOOL dirty;                   /* contains something we can't replay */
    BOOL hasWorld;
} OpList;

static OpList  g_lists[3];
static OpList *g_rec = &g_lists[0], *g_cur = &g_lists[1], *g_prev = &g_lists[2];

static int  g_worldDepth;         /* >0 while inside a world draw function */
static int  g_hookDepth;          /* re-entrancy guard for COM hooks */
static BOOL g_replaying;

static BOOL Recording(void *surf)
{
    return !g_replaying && g_worldDepth == 0 && g_hookDepth == 1 && surf == *G_BACKBUFFER;
}

static Op *NewOp(void)
{
    if (g_rec->n >= MAX_OPS) { g_rec->dirty = TRUE; return NULL; }
    return &g_rec->ops[g_rec->n++];
}

static HRESULT WINAPI Hook_Blt(void *self, LPRECT dst, void *src, LPRECT srect, DWORD flags, LPDDBLTFX fx)
{
    VtHook *h = FindVt(self);
    HRESULT hr;
    g_hookDepth++;
    if (Recording(self)) {
        Op *op = NewOp();
        if (op) {
            op->kind = OP_BLT;
            op->hasDst = dst != NULL;   if (dst) op->dst = *dst;
            op->hasSrc = srect != NULL; if (srect) op->src = *srect;
            op->hasFx = fx != NULL;     if (fx) op->bfx = *fx;
            op->srcSurf = src;
            op->flags = flags;
            if (src && src == self) g_rec->dirty = TRUE;
        }
    }
    hr = h ? h->Blt(self, dst, src, srect, flags, fx) : E_FAIL;
    g_hookDepth--;
    return hr;
}

static HRESULT WINAPI Hook_BltFast(void *self, DWORD x, DWORD y, void *src, LPRECT srect, DWORD trans)
{
    VtHook *h = FindVt(self);
    HRESULT hr;
    g_hookDepth++;
    if (Recording(self)) {
        Op *op = NewOp();
        if (op) {
            op->kind = OP_BLTFAST;
            op->x = x; op->y = y;
            op->hasSrc = srect != NULL; if (srect) op->src = *srect;
            op->srcSurf = src;
            op->flags = trans;
            if (src && src == self) g_rec->dirty = TRUE;
        }
    }
    hr = h ? h->BltFast(self, x, y, src, srect, trans) : E_FAIL;
    g_hookDepth--;
    return hr;
}

static HRESULT WINAPI Hook_GetDC(void *self, HDC *dc)
{
    VtHook *h = FindVt(self);
    HRESULT hr;
    g_hookDepth++;
    if (Recording(self)) g_rec->dirty = TRUE;   /* GDI drawing: can't replay */
    hr = h ? h->GetDC(self, dc) : E_FAIL;
    g_hookDepth--;
    return hr;
}

static HRESULT WINAPI Hook_Lock(void *self, LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e)
{
    VtHook *h = FindVt(self);
    HRESULT hr;
    g_hookDepth++;
    if (Recording(self)) g_rec->dirty = TRUE;   /* direct pixel access */
    hr = h ? h->Lock(self, r, d, f, e) : E_FAIL;
    g_hookDepth--;
    return hr;
}

static BOOL HookSurfaceVtable(void *surf)
{
    void **vt;
    DWORD old;
    VtHook *h;
    if (!surf || !Readable(surf, 4)) return FALSE;
    vt = *(void ***)surf;
    if (!Readable(vt, (VT_RESTORE + 1) * sizeof(void *))) return FALSE;
    if (vt[VT_BLT] == (void *)Hook_Blt) return TRUE;   /* already ours */
    { int i; for (i = 0; i < g_nvt; i++) if (g_vt[i].vt == vt) return TRUE; }
    if (g_nvt >= (int)(sizeof g_vt / sizeof g_vt[0])) return FALSE;
    h = &g_vt[g_nvt];
    h->vt = vt;
    h->Blt = (Blt_t)vt[VT_BLT];
    h->BltFast = (BltFast_t)vt[VT_BLTFAST];
    h->GetDC = (GetDC_t)vt[VT_GETDC];
    h->Lock = (Lock_t)vt[VT_LOCK];
    if (!VirtualProtect(vt, (VT_RESTORE + 1) * sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    g_nvt++;
    vt[VT_BLT] = (void *)Hook_Blt;
    vt[VT_BLTFAST] = (void *)Hook_BltFast;
    vt[VT_GETDC] = (void *)Hook_GetDC;
    vt[VT_LOCK] = (void *)Hook_Lock;
    VirtualProtect(vt, (VT_RESTORE + 1) * sizeof(void *), old, &old);
    return TRUE;
}

/* =================================================================== */
/*  World drawing function hooks                                        */
/* =================================================================== */

typedef void (__cdecl *DrawFn)(int, int);
static DrawFn g_tramp[NUM_WORLD];
static BOOL   g_putBackRedirected;

static void WorldDraw(int i, int fx, int fy)
{
    if (!g_replaying && g_worldDepth == 0) {
        Op *op = NewOp();
        if (op) {
            op->kind = OP_WORLD; op->world = i; op->fx = fx; op->fy = fy;
            g_rec->hasWorld = TRUE;
        }
    }
    g_worldDepth++;
    g_tramp[i](fx, fy);
    g_worldDepth--;
}

#define WHOOK(i) static void __cdecl WHook##i(int fx, int fy) { WorldDraw(i, fx, fy); }
WHOOK(0) WHOOK(1) WHOOK(2) WHOOK(3) WHOOK(4) WHOOK(5)
WHOOK(6) WHOOK(7) WHOOK(8) WHOOK(9) WHOOK(10) WHOOK(11)
static const DrawFn g_whook[NUM_WORLD] = {
    WHook0, WHook1, WHook2, WHook3, WHook4, WHook5,
    WHook6, WHook7, WHook8, WHook9, WHook10, WHook11,
};

/* Length of a position-independent prologue instruction, 0 if unknown. */
static int InsnLen(const BYTE *p)
{
    if (p[0] >= 0x50 && p[0] <= 0x57) return 1;                     /* push r32 */
    if ((p[0] == 0x8B || p[0] == 0x89) && (p[1] & 0xC0) == 0xC0) return 2; /* mov r,r */
    if ((p[0] == 0x83) && p[1] == 0xEC) return 3;                   /* sub esp, ib */
    if ((p[0] == 0x81) && p[1] == 0xEC) return 6;                   /* sub esp, id */
    if (p[0] == 0xC7 && p[1] == 0x45) return 7;                     /* mov [ebp+ib], id */
    if (p[0] == 0xC6 && p[1] == 0x45) return 4;                     /* mov byte [ebp+ib], ib */
    if (p[0] == 0x0F && p[1] == 0xB6 && p[2] == 0x05) return 7;     /* movzx eax, byte [abs] */
    if (p[0] == 0xA1) return 5;                                     /* mov eax, [abs] */
    if (p[0] == 0x8B && (p[1] & 0xC7) == 0x05) return 6;            /* mov r, [abs] */
    return 0;
}

static BOOL HookWorldFunction(int i)
{
    BYTE *fn = (BYTE *)g_worldAddr[i];
    BYTE *tr;
    int len = 0, l;
    DWORD old;

    if (!Readable(fn, 16)) return FALSE;
    tr = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return FALSE;

    if (fn[0] == 0xE9) {
        /* A mod already redirected this function: chain to its target. */
        if (i == 0) g_putBackRedirected = TRUE;
        DWORD target = (DWORD)(UINT_PTR)fn + 5 + *(LONG *)(fn + 1);
        tr[0] = 0xE9;
        *(DWORD *)(tr + 1) = target - ((DWORD)(UINT_PTR)tr + 5);
        len = 5;
    } else {
        while (len < 5) {
            l = InsnLen(fn + len);
            if (!l) { VirtualFree(tr, 0, MEM_RELEASE); return FALSE; }
            len += l;
        }
        memcpy(tr, fn, len);
        tr[len] = 0xE9;
        *(DWORD *)(tr + len + 1) = ((DWORD)(UINT_PTR)fn + len) - ((DWORD)(UINT_PTR)tr + len + 5);
    }
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    g_tramp[i] = (DrawFn)tr;

    if (!VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    fn[0] = 0xE9;
    *(DWORD *)(fn + 1) = (DWORD)(UINT_PTR)g_whook[i] - ((DWORD)(UINT_PTR)fn + 5);
    if (len > 5) memset(fn + 5, 0x90, len - 5);
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    return TRUE;
}

/* =================================================================== */
/*  Entity snapshots and interpolation                                  */
/* =================================================================== */

typedef struct { int x, y, code; BYTE alive; } EntSnap;

#define MAX_ENTS 1024
typedef struct {
    EntSnap e[MAX_ENTS];
    int     scalar[NUM_SCALARS];
    int     backType, backFx;
} Snapshot;

static Snapshot g_snapA, g_snapB;
static Snapshot *g_snapCur = &g_snapA, *g_snapPrev = &g_snapB;

static void TakeSnapshot(Snapshot *s)
{
    int t, i, k = 0;
    for (t = 0; t < NUM_TABLES; t++) {
        const EntityTable *et = &g_ent[t];
        for (i = 0; i < et->count && k < MAX_ENTS; i++, k++) {
            BYTE *b = (BYTE *)(et->base + i * et->stride);
            EntSnap *e = &s->e[k];
            e->alive = et->condOff < 0 ? 1 : ((b[et->condOff] & 0x80) != 0);
            e->code  = et->codeOff < 0 ? 0 : *(int *)(b + et->codeOff);
            e->x = *(int *)(b + et->xOff);
            e->y = *(int *)(b + et->yOff);
        }
    }
    for (i = 0; i < NUM_SCALARS; i++) s->scalar[i] = *(int *)g_scalars[i].addr;
    s->backType = *BACK_TYPE;
    s->backFx = *BACK_FX;
}

/* Undo log for everything changed while drawing an interpolated frame. */
typedef struct { int *addr; int val; } Undo;
static Undo  g_undo[MAX_ENTS * 2 + 16];
static int   g_nundo;
static BYTE  g_counterSave[sizeof g_drawCounters / sizeof g_drawCounters[0]];
static BYTE  g_scratchSave[SCRATCH_SIZE];

static int Lerp(int a, int b, int alpha16)
{
    return a + (int)(((LONGLONG)(b - a) * alpha16) >> 16);
}

static void SetInterp(int *addr, int prev, int cur, int maxDelta, int alpha16)
{
    int d = cur - prev;
    if (d == 0 || d > maxDelta || d < -maxDelta) return;
    if (*addr != cur) return;                 /* changed since snapshot: leave it */
    g_undo[g_nundo].addr = addr;
    g_undo[g_nundo].val = cur;
    g_nundo++;
    *addr = Lerp(prev, cur, alpha16);
}

static BOOL     g_cloudReplica;        /* PutBack type 6/7 code is unmodified */
static BOOL     g_cloudActive;         /* draw clouds ourselves this frame */
static LONGLONG g_cloudFx16;           /* interpolated gBack.fx, 16.16 fixed point */

static void SetValue(int *addr, int expect, int val)
{
    if (*addr != expect || val == expect) return;
    g_undo[g_nundo].addr = addr;
    g_undo[g_nundo].val = expect;
    g_nundo++;
    *addr = val;
}

static void InterpolateBackground(int alpha16)
{
    int type = g_snapCur->backType, pfx = g_snapPrev->backFx, cfx = g_snapCur->backFx;

    g_cloudActive = FALSE;
    if (g_snapPrev->backType != type || *BACK_TYPE != type) return;   /* stage changed */

    if (type == 6 || type == 7) {
        /* fx wraps from 639 to 0: interpolate along the shortest way round. */
        int d = cfx - pfx;
        LONGLONG f, period = (LONGLONG)640 << 16;
        if (pfx < 0 || pfx >= 640 || cfx < 0 || cfx >= 640) return;
        if (d < -320) d += 640; else if (d > 320) d -= 640;
        if (d < -16 || d > 16) return;
        f = ((LONGLONG)pfx << 16) + (LONGLONG)d * alpha16;
        f %= period; if (f < 0) f += period;
        if (g_cloudReplica) {
            g_cloudFx16 = f;
            g_cloudActive = TRUE;
        } else {
            SetValue(BACK_FX, cfx, (int)(f >> 16));   /* game draws it, whole pixels */
        }
    } else {
        SetInterp(BACK_FX, pfx, cfx, 0x1000, alpha16);   /* e.g. type 5, 1/512 px units */
    }
}

/* Same drawing as PutBack for types 6/7, with a fractional scroll position. */
static void DrawCloudBackground(LONGLONG fx16)
{
    typedef void (__cdecl *PutBitmap4_t)(const RECT *, int, int, const RECT *, int);
    PutBitmap4_t put = (PutBitmap4_t)PUTBITMAP4;
    static const int top[4] = { 0x58, 0x7B, 0x92, 0xB0 };
    static const int bot[4] = { 0x7B, 0x92, 0xB0, 0xF0 };
    int ofs[4], k;
    RECT r;

    ofs[0] = (int)(fx16 >> 17);            /* fx / 2 */
    ofs[1] = (int)(fx16 >> 16) % 320;      /* fx     */
    ofs[2] = (int)(fx16 >> 15) % 320;      /* fx * 2 */
    ofs[3] = (int)(fx16 >> 14) % 320;      /* fx * 4 */

    r.left = 0; r.top = 0; r.right = 320; r.bottom = 0x58;          /* sky */
    put(GRC_GAME, 0, 0, &r, SURF_BACKGROUND);
    for (k = 0; k < 4; k++) {
        r.top = top[k]; r.bottom = bot[k]; r.right = 320;
        r.left = ofs[k];
        put(GRC_GAME, 0, top[k], &r, SURF_BACKGROUND);
        r.left = 0;
        put(GRC_GAME, 320 - ofs[k] % 320, top[k], &r, SURF_BACKGROUND);
    }
}

static void ApplyInterpolation(int alpha16)
{
    int t, i, k = 0;
    g_nundo = 0;
    for (i = 0; i < (int)(sizeof g_drawCounters / sizeof g_drawCounters[0]); i++)
        g_counterSave[i] = *(BYTE *)g_drawCounters[i];
    memcpy(g_scratchSave, (void *)SCRATCH_ADDR, SCRATCH_SIZE);

    for (t = 0; t < NUM_TABLES; t++) {
        const EntityTable *et = &g_ent[t];
        for (i = 0; i < et->count && k < MAX_ENTS; i++, k++) {
            const EntSnap *p = &g_snapPrev->e[k], *c = &g_snapCur->e[k];
            BYTE *b;
            if (!c->alive || !p->alive || c->code != p->code) continue;
            {
                int dx = c->x - p->x, dy = c->y - p->y;
                if (dx > et->maxDelta || dx < -et->maxDelta ||
                    dy > et->maxDelta || dy < -et->maxDelta) continue;   /* teleport */
            }
            b = (BYTE *)(et->base + i * et->stride);
            SetInterp((int *)(b + et->xOff), p->x, c->x, et->maxDelta, alpha16);
            SetInterp((int *)(b + et->yOff), p->y, c->y, et->maxDelta, alpha16);
        }
    }
    for (i = 0; i < NUM_SCALARS; i++)
        SetInterp((int *)g_scalars[i].addr, g_snapPrev->scalar[i], g_snapCur->scalar[i],
                  g_scalars[i].maxDelta, alpha16);
    InterpolateBackground(alpha16);
}

static void UndoInterpolation(void)
{
    int i;
    while (g_nundo > 0) { g_nundo--; *g_undo[g_nundo].addr = g_undo[g_nundo].val; }
    for (i = 0; i < (int)(sizeof g_drawCounters / sizeof g_drawCounters[0]); i++)
        *(BYTE *)g_drawCounters[i] = g_counterSave[i];
    memcpy((void *)SCRATCH_ADDR, g_scratchSave, SCRATCH_SIZE);
}

/* Camera arguments passed to the k-th call of world function i in the previous tick. */
static BOOL PrevWorldArgs(int world, int occurrence, int *fx, int *fy)
{
    int i, n = 0;
    for (i = 0; i < g_prev->n; i++) {
        const Op *op = &g_prev->ops[i];
        if (op->kind == OP_WORLD && op->world == world) {
            if (n == occurrence) { *fx = op->fx; *fy = op->fy; return TRUE; }
            n++;
        }
    }
    return FALSE;
}

/* =================================================================== */
/*  Rendering and presenting                                            */
/* =================================================================== */

static void  *g_interpSurf;
static void  *g_interpFor;        /* back buffer the surface was made for */
static int    g_interpW, g_interpH;

static void *GetInterpSurface(void)
{
    void *dd = *G_LPDD, *bb = *G_BACKBUFFER;
    int w = *G_SCREEN_W, h = *G_SCREEN_H;
    if (!dd || !bb || w <= 0 || h <= 0) return NULL;

    if (g_interpSurf && (g_interpFor != bb || g_interpW != w || g_interpH != h)) {
        ((ULONG (WINAPI *)(void *))(*(void ***)g_interpSurf)[2])(g_interpSurf);
        g_interpSurf = NULL;
    }
    if (!g_interpSurf) {
        DDSURFACEDESC d;
        void *s = NULL;
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        d.dwWidth = w;
        d.dwHeight = h;
        if (((HRESULT (WINAPI *)(void *, LPDDSURFACEDESC, void **, IUnknown *))
                 (*(void ***)dd)[6])(dd, &d, &s, NULL) != DD_OK || !s)
            return NULL;
        g_interpSurf = s; g_interpFor = bb; g_interpW = w; g_interpH = h;
        HookSurfaceVtable(s);
    }
    if (((HRESULT (WINAPI *)(void *))(*(void ***)g_interpSurf)[VT_ISLOST])(g_interpSurf) != DD_OK)
        ((HRESULT (WINAPI *)(void *))(*(void ***)g_interpSurf)[VT_RESTORE])(g_interpSurf);
    return g_interpSurf;
}

static Blt_t OrigBlt(void *surf)
{
    VtHook *h = FindVt(surf);
    return h ? h->Blt : (Blt_t)(*(void ***)surf)[VT_BLT];
}

/* Same as the presentation code in Flip_SystemTask. */
static BOOL Present(HWND hwnd, void *src)
{
    void *front = *G_FRONTBUFFER;
    RECT rc;
    if (!front || !src || !GetWindowRect(hwnd, &rc)) return FALSE;
    rc.left += *G_CLIENT_OFS_X;
    rc.top  += *G_CLIENT_OFS_Y;
    rc.right  = rc.left + *G_SCREEN_W;
    rc.bottom = rc.top + *G_SCREEN_H;
    return OrigBlt(front)(front, &rc, src, G_SRC_RECT, DDBLT_WAIT, NULL) == DD_OK;
}

/* Draw the current tick's frame with everything placed at alpha between
   the previous and current tick. Returns the surface, or NULL on failure. */
#ifdef FRAMEFIX_LOG
static int g_dbgX, g_dbgCamX, g_dbgCloud, g_dbgBackFx;
#endif
static void *RenderInterpolated(int alpha16)
{
    void *bb = *G_BACKBUFFER, *surf = GetInterpSurface();
    Blt_t blt;
    int i, occ[NUM_WORLD];

    if (!surf) return NULL;
#ifdef FRAMEFIX_VERIFY
    static BYTE before[0x2F048];
    memcpy(before, (void *)0x48F000, sizeof before);
#endif
    blt = OrigBlt(surf);
    if (blt(surf, NULL, bb, NULL, DDBLT_WAIT, NULL) != DD_OK) return NULL;

    memset(occ, 0, sizeof occ);
    ApplyInterpolation(alpha16);
#ifdef FRAMEFIX_LOG
    g_dbgX = *(int *)0x49E654; g_dbgCamX = *(int *)0x49E1C8;
    g_dbgCloud = g_cloudActive ? (int)(g_cloudFx16 >> 8) : -1;
    g_dbgBackFx = *BACK_FX;
#endif
    g_replaying = TRUE;
    *G_BACKBUFFER = surf;                     /* game's draw code now targets our surface */

    for (i = 0; i < g_cur->n; i++) {
        const Op *op = &g_cur->ops[i];
        if (op->kind == OP_WORLD) {
            int fx = op->fx, fy = op->fy, px, py;
            if (PrevWorldArgs(op->world, occ[op->world]++, &px, &py)) {
                int dx = fx - px, dy = fy - py;
                if (dx >= -0x8000 && dx <= 0x8000 && dy >= -0x8000 && dy <= 0x8000) {
                    fx = Lerp(px, fx, alpha16);
                    fy = Lerp(py, fy, alpha16);
                }
            }
            if (op->world == 0 && g_cloudActive && (*BACK_TYPE == 6 || *BACK_TYPE == 7))
                DrawCloudBackground(g_cloudFx16);          /* PutBack, cloud types */
            else
                g_tramp[op->world](fx, fy);
        } else if (op->kind == OP_BLT) {
            blt(surf, op->hasDst ? (LPRECT)&op->dst : NULL, op->srcSurf,
                op->hasSrc ? (LPRECT)&op->src : NULL, op->flags,
                op->hasFx ? (LPDDBLTFX)&op->bfx : NULL);
        } else {
            VtHook *h = FindVt(surf);
            if (h) h->BltFast(surf, op->x, op->y, op->srcSurf,
                              op->hasSrc ? (LPRECT)&op->src : NULL, op->flags);
        }
    }

    *G_BACKBUFFER = bb;
    g_replaying = FALSE;
    UndoInterpolation();
#ifdef FRAMEFIX_VERIFY
    {
        const BYTE *after = (const BYTE *)0x48F000;
        DWORD k, run = 0;
        for (k = 0; k < sizeof before; k++) {
            if (before[k] != after[k]) { if (!run) run = 0x48F000 + k; }
            else if (run) { Log("DIFF %x-%x\r\n", run, 0x48F000 + k - 1); run = 0; }
        }
        if (run) Log("DIFF %x-end\r\n", run);
    }
#endif
    return surf;
}

/* =================================================================== */
/*  Display refresh synchronisation                                     */
/* =================================================================== */

typedef HRESULT (WINAPI *DwmFlush_t)(void);
typedef HRESULT (WINAPI *DwmGetCompositionTimingInfo_t)(HWND, DWM_TIMING_INFO *);

enum { VS_DWM, VS_DDRAW, VS_TIMER };
static int       g_vsMode = VS_DWM;
static int       g_vsStrikes;
static LONGLONG  g_period;          /* refresh period in QPC ticks */
static LONGLONG  g_lastVB;          /* timestamp of a recent vblank */
static DwmFlush_t g_DwmFlush;
static DwmGetCompositionTimingInfo_t g_DwmTiming;

static BOOL DwmTiming(LONGLONG *vblank, LONGLONG *period)
{
    DWM_TIMING_INFO ti;
    if (!g_DwmTiming) return FALSE;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (g_DwmTiming(NULL, &ti) != S_OK) return FALSE;
    *period = (LONGLONG)ti.qpcRefreshPeriod;
    *vblank = (LONGLONG)ti.qpcVBlank;
    return *period > 0 && *vblank > 0;
}

static void InitVSync(HWND hwnd)
{
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    DEVMODEA dm;
    HMONITOR mon;
    MONITORINFOEXA mi;
    DWORD hz = 0;

    if (dwm) {
        g_DwmFlush = (DwmFlush_t)GetProcAddress(dwm, "DwmFlush");
        g_DwmTiming = (DwmGetCompositionTimingInfo_t)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
    }
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hz = dm.dmDisplayFrequency;
    if (hz < 24 || hz > 1000) hz = 60;
    g_period = g_freq / hz;
    g_lastVB = Now();
    {
        LONGLONG vb, p;
        if (!g_DwmFlush || !DwmTiming(&vb, &p)) g_vsMode = VS_DDRAW;
        else g_lastVB = vb;
    }
    Log("vsync init: %d Hz, mode %d\r\n", (int)hz, g_vsMode);
}

/* Most recent vblank at or before 'now', and the refresh period. */
static LONGLONG LastVBlank(LONGLONG now, LONGLONG *period)
{
    LONGLONG vb = g_lastVB, p = g_period;
    if (g_vsMode == VS_DWM) {
        LONGLONG dvb, dp;
        if (DwmTiming(&dvb, &dp)) { vb = dvb; p = dp; g_period = dp; }
    }
    if (p <= 0) p = g_freq / 60;
    *period = p;
    if (now >= vb) return vb + ((now - vb) / p) * p;
    return vb - ((vb - now + p - 1) / p) * p;
}

static void Strike(void)
{
    if (++g_vsStrikes >= 3) {
        g_vsStrikes = 0;
        if (g_vsMode < VS_TIMER) { g_vsMode++; Log("vsync fallback -> mode %d\r\n", g_vsMode); }
    }
}

/* Block until the predicted vblank 'vn'; returns its timestamp. */
static LONGLONG WaitVBlank(LONGLONG vn)
{
    LONGLONG after, tol = g_period / 4;

    if (g_vsMode == VS_DWM) {
        HRESULT hr = g_DwmFlush();
        after = Now();
        if (hr == S_OK && after >= vn - tol) {
            LONGLONG vb, p;
            g_vsStrikes = 0;
            if (DwmTiming(&vb, &p) && vb >= vn - p / 2 && vb <= after) { g_lastVB = vb; return vb; }
            g_lastVB = vn;
            return vn;
        }
        Strike();
    } else if (g_vsMode == VS_DDRAW) {
        void *dd = *G_LPDD;
        HRESULT hr = dd ? ((HRESULT (WINAPI *)(void *, DWORD, HANDLE))(*(void ***)dd)[22])
                              (dd, DDWAITVB_BLOCKBEGIN, NULL) : E_FAIL;
        after = Now();
        if (hr == DD_OK && after >= vn - tol) {
            g_vsStrikes = 0;
            /* Re-anchor the timeline to the real vblank, keep the period. */
            g_lastVB = after;
            return after;
        }
        Strike();
    }
    /* Timer fallback: sleep to the predicted refresh time. */
    SleepUntil(vn);
    g_lastVB = vn;
    return vn;
}

/* =================================================================== */
/*  Frame scheduler (replaces the wait loop in Flip_SystemTask)         */
/* =================================================================== */

static BOOL     g_init, g_hooksDone;
static LONGLONG g_next;           /* deadline (display time) of the current tick */
static LONGLONG g_prevDeadline;
static LONGLONG g_servedVB;       /* last refresh a frame was presented for */
static BOOL     g_prevValid;      /* previous tick is directly before this one */

static void InitOnce(HWND hwnd)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    timeBeginPeriod(1);
    g_next = Now();
    g_prevDeadline = g_next;
    InitVSync(hwnd);
    g_init = TRUE;
}

/* Use the cloud re-implementation only if PutBack's type 6/7 code is the
   original (a mod that changed it keeps the game's own drawing). */
static BOOL CheckCloudCode(void)
{
    const BYTE *p = (const BYTE *)CLOUD_CODE;
    const DWORD *jt = (const DWORD *)PUTBACK_JTAB;
    DWORD h = 0x811C9DC5u;
    int k;
    if (g_putBackRedirected || !g_tramp[0]) return FALSE;
    if (!Readable(p, CLOUD_CODE_LEN) || !Readable(jt, 32)) return FALSE;
    if (jt[6] != CLOUD_CODE || jt[7] != CLOUD_CODE) return FALSE;
    for (k = 0; k < CLOUD_CODE_LEN; k++) h = (h ^ p[k]) * 0x01000193u;
    return h == CLOUD_CODE_FNV;
}

static void InstallDrawHooks(void)
{
    int i, ok = 0;
    if (g_hooksDone || !*G_BACKBUFFER) return;
    g_hooksDone = TRUE;
    if (!HookSurfaceVtable(*G_BACKBUFFER)) { g_interpEnabled = FALSE; return; }
    if (*G_FRONTBUFFER) HookSurfaceVtable(*G_FRONTBUFFER);
    for (i = 0; i < NUM_WORLD; i++) ok += HookWorldFunction(i) ? 1 : 0;
    Log("hooked %d/%d world draw functions\r\n", ok, NUM_WORLD);
    g_cloudReplica = CheckCloudCode();
    Log("cloud background replica: %d\r\n", g_cloudReplica);
}

/* Returns 0 = quit, 1 = let the game present its frame, 2 = already presented. */
int __cdecl FrameWaitC(HWND hwnd)
{
    LONGLONG now, frame, D, Dprev;
    BOOL canInterp, presented = FALSE, lateAtEntry;

    if (!g_init) InitOnce(hwnd);
    if (g_interpEnabled) InstallDrawHooks();

    /* The game has just finished drawing a tick: finalize its recording. */
    {
        OpList *t = g_prev; g_prev = g_cur; g_cur = g_rec; g_rec = t;
        g_rec->n = 0; g_rec->dirty = FALSE; g_rec->hasWorld = FALSE;
    }
    {
        Snapshot *t = g_snapPrev; g_snapPrev = g_snapCur; g_snapCur = t;
        TakeSnapshot(g_snapCur);
    }

    frame = UsToTicks(FrameUs());
    D = g_next;
    Dprev = g_prevDeadline;
    canInterp = g_interpEnabled && g_prevValid && g_cur->hasWorld && !g_cur->dirty && D > Dprev;
    lateAtEntry = Now() >= D;

    for (;;) {
        LONGLONG vlast, vn, p;
        if (!SystemTask()) return 0;
        now = Now();
        if (now >= D) break;

        if (!g_interpEnabled) {           /* plain pacing */
            SleepUntil(D);
            continue;
        }

        /* A frame submitted any time before the next composition is shown
           at that composition, so a refresh that passed while the game was
           computing this tick can still be served now, on time. */
        vlast = LastVBlank(now, &p);
        if (vlast > g_servedVB && now - vlast < p * 3 / 4) {
            g_servedVB = vlast;
            if (canInterp) {
                LONGLONG a = ((vlast - Dprev) << 16) / (D - Dprev);
                void *s;
                if (a < 0) a = 0;
                if (a > 0x10000) a = 0x10000;
                s = RenderInterpolated((int)a);
                if (s && Present(hwnd, s)) presented = TRUE;
                else canInterp = FALSE;
#ifdef FRAMEFIX_LOG
                Log("P %d %d %d %d %d %d\r\n", (int)((vlast * 1000000) / g_freq % 1000000000),
                    (int)((a * 1000) >> 16), g_dbgX, (int)(((now - vlast) * 1000000) / g_freq), g_dbgCamX,
                    (int)(((Now() - now) * 1000000) / g_freq));
                Log("C %d %d %d\r\n", g_dbgCloud, g_dbgBackFx, g_snapCur->backFx);
#endif
            } else if (!presented) {
                if (Present(hwnd, *G_BACKBUFFER)) presented = TRUE;
#ifdef FRAMEFIX_LOG
                Log("R %d\r\n", (int)((vlast * 1000000) / g_freq % 1000000000));
#endif
            }
            continue;
        }

        vn = vlast + p;
        if (vn >= D) {                    /* next refresh belongs to the next tick */
            SleepUntil(D);
            continue;
        }
        WaitVBlank(vn);                   /* then serve it on the next iteration */
    }

    /* Advance the schedule. */
    g_prevDeadline = D;
    if (now - D >= UsToTicks(ResetUs())) {
        g_next = now + frame;             /* far behind (e.g. after a pause): resync */
        g_prevDeadline = now;
        g_prevValid = FALSE;
    } else {
        g_next = D + frame;
        g_prevValid = TRUE;
    }

#ifdef FRAMEFIX_LOG
    Log("T %d %d %d %d\r\n", (int)((now * 1000000) / g_freq % 1000000000), g_cur->n,
        g_cur->hasWorld, g_cur->dirty);
#endif

    if (presented) return 2;
    if (canInterp && !lateAtEntry) return 2;   /* no refresh fell in this tick */
    return 1;
}

/* Thunk called from Flip_SystemTask: passes its hwnd argument along. */
__asm__(
    ".text\n"
    ".globl _FrameWaitThunk\n"
    "_FrameWaitThunk:\n"
    "    pushl 8(%ebp)\n"
    "    call _FrameWaitC\n"
    "    addl $4, %esp\n"
    "    ret\n");
extern void FrameWaitThunk(void);

/* =================================================================== */
/*  Font quality: no ClearType                                          */
/* =================================================================== */

typedef HFONT (WINAPI *CreateFontA_t)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD,
                                      DWORD, DWORD, DWORD, DWORD, LPCSTR);
typedef HFONT (WINAPI *CreateFontW_t)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD,
                                      DWORD, DWORD, DWORD, DWORD, LPCWSTR);
typedef HFONT (WINAPI *CreateFontIndirectA_t)(const LOGFONTA *);
typedef HFONT (WINAPI *CreateFontIndirectW_t)(const LOGFONTW *);

static CreateFontA_t         g_CreateFontA;
static CreateFontW_t         g_CreateFontW;
static CreateFontIndirectA_t g_CreateFontIndirectA;
static CreateFontIndirectW_t g_CreateFontIndirectW;

static HFONT WINAPI My_CreateFontA(int h, int w, int esc, int ori, int wt, DWORD it, DWORD ul,
                                   DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, LPCSTR face)
{
    (void)q;
    Log("CreateFontA '%s' %d quality %d -> %d\r\n", face ? face : "", h, (int)q, NONANTIALIASED_QUALITY);
    return g_CreateFontA(h, w, esc, ori, wt, it, ul, so, cs, op, cp, NONANTIALIASED_QUALITY, pf, face);
}

static HFONT WINAPI My_CreateFontW(int h, int w, int esc, int ori, int wt, DWORD it, DWORD ul,
                                   DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, LPCWSTR face)
{
    (void)q;
    return g_CreateFontW(h, w, esc, ori, wt, it, ul, so, cs, op, cp, NONANTIALIASED_QUALITY, pf, face);
}

static HFONT WINAPI My_CreateFontIndirectA(const LOGFONTA *lf)
{
    LOGFONTA l;
    if (!lf) return g_CreateFontIndirectA(lf);
    l = *lf; l.lfQuality = NONANTIALIASED_QUALITY;
    return g_CreateFontIndirectA(&l);
}

static HFONT WINAPI My_CreateFontIndirectW(const LOGFONTW *lf)
{
    LOGFONTW l;
    if (!lf) return g_CreateFontIndirectW(lf);
    l = *lf; l.lfQuality = NONANTIALIASED_QUALITY;
    return g_CreateFontIndirectW(&l);
}

/* Point every import slot of the exe that resolves to 'real' at 'hook'. */
static int PatchImports(void *real, void *hook)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int n = 0;

    if (!real || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        void **slot = (void **)(base + imp->FirstThunk);
        for (; *slot; slot++) {
            if (*slot == real) {
                DWORD old;
                if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
                    *slot = hook;
                    VirtualProtect(slot, sizeof *slot, old, &old);
                    n++;
                }
            }
        }
    }
    return n;
}

static void DisableClearType(void)
{
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) gdi = LoadLibraryA("gdi32.dll");
    if (!gdi) return;
    g_CreateFontA = (CreateFontA_t)(void *)GetProcAddress(gdi, "CreateFontA");
    g_CreateFontW = (CreateFontW_t)(void *)GetProcAddress(gdi, "CreateFontW");
    g_CreateFontIndirectA = (CreateFontIndirectA_t)(void *)GetProcAddress(gdi, "CreateFontIndirectA");
    g_CreateFontIndirectW = (CreateFontIndirectW_t)(void *)GetProcAddress(gdi, "CreateFontIndirectW");
    PatchImports((void *)g_CreateFontA, (void *)My_CreateFontA);
    PatchImports((void *)g_CreateFontW, (void *)My_CreateFontW);
    PatchImports((void *)g_CreateFontIndirectA, (void *)My_CreateFontIndirectA);
    PatchImports((void *)g_CreateFontIndirectW, (void *)My_CreateFontIndirectW);
}

/* =================================================================== */
/*  Config, patching, entry point                                       */
/* =================================================================== */

static void LoadConfig(void)
{
    char path[MAX_PATH], val[32], *slash;
    double fps;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash - path) + 14 >= MAX_PATH) return;
    lstrcpyA(slash + 1, "framefix.ini");
    GetPrivateProfileStringA("FrameFix", "FPS", "0", val, sizeof val, path);
    fps = atof(val);
    if (fps >= 1.0 && fps <= 1000.0)
        g_override_us = (LONGLONG)(1000000.0 / fps + 0.5);
    g_interpEnabled = GetPrivateProfileIntA("FrameFix", "Interpolation", 1, path) != 0;
    g_allowClearType = GetPrivateProfileIntA("FrameFix", "ClearType", 0, path) != 0;
}

static void ApplyPatch(void)
{
    BYTE *site = (BYTE *)PATCH_ADDR;
    const BYTE *call = (const BYTE *)SYSTASK_CALL;
    BYTE code[QUIT_ADDR - PATCH_ADDR];       /* 18 bytes */
    DWORD old;

    if ((UINT_PTR)GetModuleHandleA(NULL) != 0x400000) return;
    if (!Readable(site, 0x70)) return;

    if (call[0] == 0xE8)
        SystemTask = (SystemTask_t)(SYSTASK_CALL + 5 + *(const LONG *)(call + 1));

    /* call FrameWaitThunk ; dec eax ; jz CONTINUE ; jg AFTERBLT ; (fall into QUIT) */
    code[0] = 0xE8;
    *(DWORD *)(code + 1) = (DWORD)(UINT_PTR)FrameWaitThunk - (PATCH_ADDR + 5);
    code[5] = 0x48;
    code[6] = 0x0F; code[7] = 0x84;
    *(DWORD *)(code + 8) = CONTINUE_ADDR - (PATCH_ADDR + 12);
    code[12] = 0x0F; code[13] = 0x8F;
    *(DWORD *)(code + 14) = AFTERBLT_ADDR - (PATCH_ADDR + 18);

    if (!VirtualProtect(site, sizeof code, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof code);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadConfig();
        ApplyPatch();
        if (!g_allowClearType) DisableClearType();
    } else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        if (g_init) timeEndPeriod(1);
        if (g_real) FreeLibrary(g_real);
    }
    return TRUE;
}
