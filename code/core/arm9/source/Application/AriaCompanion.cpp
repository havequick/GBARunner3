#include "common.h"
#include <libtwl/mem/memVram.h>
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include <libtwl/gfx/gfxStatus.h>
#include "Emulator/IoRegisters.h"
#include "GbaHeader.h"
#include "GbaIoRegOffsets.h"
#include "SdCache/SdCache.h"
#include "SystemIpc.h"
#include "VirtualMachine/VMDtcm.h"
#include "AriaCompanion.h"

// VRAM-A is already tightly packed by the emulator core. Prefer compact code
// for this game-specific companion so its richer map still fits the image.
#pragma GCC optimize ("Os")

extern GbaHeader gRomHeader;

extern "C"
{
volatile u32 aria_companion_hblank_mask_active = 0;
volatile u16 aria_companion_saved_master_bright = 0;
}

// Castlevania: Aria of Sorrow (USA) second-screen live map prototype.
// Target ROM: A2CE / SHA-1 abd71fe01ebb201bcc133074db1dd8c5253776c7
namespace
{
constexpr u32 kAriaUs = GAMECODE("A2CE");
constexpr uintptr_t kEwramLo = 0x02000000;
constexpr uintptr_t kEwramHi = 0x02040000;
constexpr uintptr_t kMap = 0x020000B4;
constexpr uintptr_t kMapOrigin = 0x0200008C;
constexpr uintptr_t kMapItems = 0x02013294;
constexpr uintptr_t kBg1X = 0x0200A09A;
constexpr uintptr_t kBg1Y = 0x0200A09E;
constexpr uintptr_t kPlayerPtr = 0x02013110;
constexpr uintptr_t kPlayerX = 0x42;
constexpr uintptr_t kPlayerY = 0x46;

// Aria's native map metadata. It is copied once from the user's ROM into spare
// sub BG VRAM so the live renderer never performs an SD-cache load in VBlank.
constexpr u32 kMapInfoRom = 0x08116650;
constexpr u32 kMapWallsRom = 0x08117DD0;

constexpr int kW = 64, kH = 40;
constexpr int kTW = 32, kTH = 20;
constexpr int kMapOffsetX = 4, kMapOffsetY = 2;
constexpr int kTopTile = 1;
constexpr uintptr_t kChars = 0x06200000;
constexpr uintptr_t kMapVram = 0x06208000;
constexpr uintptr_t kSignatures = 0x06208800;
constexpr uintptr_t kMapInfo = 0x06209400;
constexpr uintptr_t kMapWalls = 0x0620A800;
constexpr uintptr_t kPal = 0x05000400;

constexpr u32 kWindowBits = (1 << 13) | (1 << 14) | (1 << 15);
constexpr uintptr_t kRegWin0H = 0x04000040;
constexpr uintptr_t kRegWin0V = 0x04000044;
constexpr uintptr_t kRegWinIn = 0x04000048;
constexpr uintptr_t kRegWinOut = 0x0400004A;

enum CellState
{
    CELL_HIDDEN = 0,
    CELL_REVEALED,
    CELL_VISITED,
    CELL_SAVE,
    CELL_WARP
};

enum MapColor
{
    COLOR_CLEAR = 0,
    COLOR_EDGE = 1,
    COLOR_SAVE = 2,
    COLOR_WARP = 3,
    COLOR_REVEALED = 4,
    COLOR_REVEALED_EDGE = 5,
    COLOR_PLAYER = 6,
    COLOR_VISITED = 9,
    COLOR_SECRET_EDGE = 11
};

bool sActive = false;
bool sViewportWindowActive = false;
u32 sFrame = 0;

constexpr u16 rgb15(int r, int g, int b) { return (u16)(r | (g << 5) | (b << 10)); }
inline volatile u8* tilePtr(int n) { return (volatile u8*)(kChars + n * 32); }
inline volatile u16* tileMap() { return (volatile u16*)kMapVram; }
inline volatile u32* signatures() { return (volatile u32*)kSignatures; }
inline volatile const u16* mapInfo() { return (volatile const u16*)kMapInfo; }
inline volatile const u8* mapWalls() { return (volatile const u8*)kMapWalls; }

void copyRomData(u32 romAddress, volatile u8* dst, u32 byteCount)
{
    while (byteCount)
    {
        const u8* block = (const u8*)sdc_getRomBlock(romAddress);
        const u32 offset = romAddress & SDC_BLOCK_MASK;
        u32 count = SDC_BLOCK_SIZE - offset;
        if (count > byteCount) count = byteCount;
        for (u32 i = 0; i < count; ++i) dst[i] = block[offset + i];
        dst += count;
        romAddress += count;
        byteCount -= count;
    }
}

u16 infoAt(int x, int y)
{
    if ((unsigned)x >= kW || (unsigned)y >= kH) return 0xFFFF;
    return mapInfo()[y * kW + x];
}

u8 wallAt(int x, int y)
{
    if ((unsigned)x >= kW || (unsigned)y >= kH) return 0;
    const u8 packed = mapWalls()[y * (kW / 2) + (x >> 1)];
    return (x & 1) ? (packed & 0xF) : (packed >> 4);
}

bool excludedFromAreaMap(int x, int y)
{
    return (x == 4 && y == 13) || (x == 12 && y == 13) ||
           (x == 13 && y == 13) || (x == 15 && y == 22) ||
           (x == 47 && y == 20);
}

CellState mapState(int screenX, int screenY)
{
    const int x = screenX - kMapOffsetX;
    const int y = screenY - kMapOffsetY;
    const u16 info = infoAt(x, y);
    if (info == 0xFFFF) return CELL_HIDDEN;

    const int half = x >> 5, bit = x & 31;
    const uintptr_t p = kMap + half * (40 * 8) + y * 8;
    const u32 mask = 1u << bit;
    const bool visited = (*(volatile const u32*)(p + 0) & mask) != 0;
    const bool revealed = (*(volatile const u32*)(p + 4) & mask) != 0;

    if (visited)
    {
        if (info & 0x8000) return CELL_SAVE;
        if (info & 0x4000) return CELL_WARP;
        return CELL_VISITED;
    }
    if (revealed) return CELL_REVEALED;

    const int area = (info >> 6) & 0xF;
    const volatile u8* items = (volatile const u8*)kMapItems;
    const bool areaMap = ((area == 0 || area == 1) && items[0x1D]) ||
                         (area == 3 && items[0x1E]) ||
                         (area == 5 && items[0x1F]);
    return areaMap && !excludedFromAreaMap(x, y) ? CELL_REVEALED : CELL_HIDDEN;
}

u8 fillColor(CellState state)
{
    switch (state)
    {
        case CELL_REVEALED: return COLOR_REVEALED;
        case CELL_VISITED: return COLOR_VISITED;
        case CELL_SAVE: return COLOR_SAVE;
        case CELL_WARP: return COLOR_WARP;
        default: return COLOR_CLEAR;
    }
}

u8 edgeColor(CellState state, int kind)
{
    if (kind == 1 || kind == 2) return COLOR_SECRET_EDGE;
    return state == CELL_REVEALED ? COLOR_REVEALED_EDGE : COLOR_EDGE;
}

inline void px(volatile u8* t, int x, int y, u8 c)
{
    volatile u8& b = t[y * 4 + (x >> 1)];
    b = (x & 1) ? (u8)((b & 0x0F) | (c << 4)) : (u8)((b & 0xF0) | c);
}

void horizontal(volatile u8* t, int x0, int y, u8 color)
{
    for (int x = x0; x < x0 + 4; ++x) px(t, x, y, color);
}

void vertical(volatile u8* t, int x, int y0, u8 color)
{
    for (int y = y0; y < y0 + 4; ++y) px(t, x, y, color);
}

void drawCell(volatile u8* t, int cx, int cy, int screenX, int screenY)
{
    const CellState state = mapState(screenX, screenY);
    if (state == CELL_HIDDEN) return;

    const int x0 = cx * 4, y0 = cy * 4;
    const u8 fill = fillColor(state);
    for (int y = y0; y < y0 + 4; ++y)
        for (int x = x0; x < x0 + 4; ++x)
            px(t, x, y, fill);

    const int mapX = screenX - kMapOffsetX;
    const int mapY = screenY - kMapOffsetY;
    const u8 shape = wallAt(mapX, mapY);
    const int top = shape >> 2;
    const int left = shape & 3;
    if (top || infoAt(mapX, mapY - 1) == 0xFFFF)
        horizontal(t, x0, y0, edgeColor(state, top));
    if (left || infoAt(mapX - 1, mapY) == 0xFFFF)
        vertical(t, x0, y0, edgeColor(state, left));

    // Shared boundaries are normally drawn by the cell below/right. If that
    // neighbor is still hidden, keep the visible room crisply outlined here.
    if (mapState(screenX, screenY + 1) == CELL_HIDDEN)
    {
        const int bottom = wallAt(mapX, mapY + 1) >> 2;
        horizontal(t, x0, y0 + 3, edgeColor(state, bottom));
    }
    if (mapState(screenX + 1, screenY) == CELL_HIDDEN)
    {
        const int right = wallAt(mapX + 1, mapY) & 3;
        vertical(t, x0 + 3, y0, edgeColor(state, right));
    }
}

bool playerMapPos(int& x, int& y)
{
    const u32 ent = *(volatile const u32*)kPlayerPtr;
    if (ent < kEwramLo || ent + kPlayerY + 2 > kEwramHi) return false;
    const s16 camX = *(volatile const s16*)kBg1X;
    const s16 camY = *(volatile const s16*)kBg1Y;
    const s16 entX = *(volatile const s16*)(ent + kPlayerX);
    const s16 entY = *(volatile const s16*)(ent + kPlayerY);
    const u16 origin = *(volatile const u16*)kMapOrigin;
    x = (origin & 0x7F) + (((s32)camX + entX) >> 8) + kMapOffsetX;
    y = ((origin >> 7) & 0x7F) + (((s32)camY + entY) >> 8) + kMapOffsetY;
    return (unsigned)x < kW && (unsigned)y < kH;
}

u32 signature(int tx, int ty, int pxm, int pym, bool showPlayer)
{
    u32 hash = 2166136261u;
    const int x0 = tx * 2, y0 = ty * 2;
    for (int y = y0 - 1; y <= y0 + 2; ++y)
        for (int x = x0 - 1; x <= x0 + 2; ++x)
        {
            hash ^= (u32)mapState(x, y);
            hash *= 16777619u;
        }
    if (showPlayer && (pxm >> 1) == tx && (pym >> 1) == ty)
        hash ^= (u32)(1 + (pxm - x0) + (pym - y0) * 4);
    return hash;
}

void drawTile(int tx, int ty, int pxm, int pym, bool showPlayer)
{
    volatile u8* t = tilePtr(1 + ty * kTW + tx);
    for (int i = 0; i < 32; ++i) t[i] = 0;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx)
            drawCell(t, cx, cy, tx * 2 + cx, ty * 2 + cy);

    if (showPlayer && (pxm >> 1) == tx && (pym >> 1) == ty)
    {
        const int x = (pxm & 1) * 4 + 1;
        const int y = (pym & 1) * 4 + 1;
        px(t, x, y, COLOR_PLAYER); px(t, x + 1, y, COLOR_PLAYER);
        px(t, x, y + 1, COLOR_PLAYER); px(t, x + 1, y + 1, COLOR_PLAYER);
    }
}

void redraw()
{
    int x = -1, y = -1;
    const bool havePlayer = playerMapPos(x, y);
    const bool showPlayer = havePlayer && (((sFrame >> 3) & 1) == 0);
    for (int ty = 0; ty < kTH; ++ty)
        for (int tx = 0; tx < kTW; ++tx)
        {
            const int i = ty * kTW + tx;
            const u32 value = signature(tx, ty, x, y, showPlayer);
            if (signatures()[i] != value)
            {
                signatures()[i] = value;
                drawTile(tx, ty, x, y, showPlayer);
            }
        }
}

void keepTopScreenMaskIrqEnabled()
{
    // We use HBlank only to black the DS-only scanlines 160-191. This keeps
    // direct GBA rendering (and its latency) intact instead of frame-capturing.
    vm_forcedIrqMask |= 1 << 1;
    gfx_setHBlankIrqEnabled(true);
}

void restoreGbaWindowRegisters()
{
    *(volatile u16*)kRegWin0H = *(u16*)&emu_ioRegisters[GBA_REG_OFFS_WIN0H];
    *(volatile u16*)kRegWin0V = *(u16*)&emu_ioRegisters[GBA_REG_OFFS_WIN0V];
    *(volatile u16*)kRegWinIn = *(u16*)&emu_ioRegisters[GBA_REG_OFFS_WININ];
    *(volatile u16*)kRegWinOut = *(u16*)&emu_ioRegisters[GBA_REG_OFFS_WINOUT];
}

void initSubScreen()
{
    // H and I hold the relocated GBA BIOS and the ROM cache lookup table.
    // VRAM C is otherwise only needed by the center-and-mask capture path,
    // which is deliberately disabled while the companion is active.
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);

    volatile u16* pal = (volatile u16*)kPal;
    for (int i = 0; i < 16; ++i) pal[i] = 0;
    pal[COLOR_EDGE] = rgb15(23, 27, 31);
    pal[COLOR_SAVE] = rgb15(31, 4, 5);
    pal[COLOR_WARP] = rgb15(31, 22, 3);
    pal[COLOR_REVEALED] = rgb15(3, 6, 11);
    pal[COLOR_REVEALED_EDGE] = rgb15(10, 15, 21);
    pal[COLOR_PLAYER] = rgb15(31, 31, 9);
    pal[COLOR_VISITED] = rgb15(7, 14, 25);
    pal[COLOR_SECRET_EDGE] = rgb15(2, 4, 7);

    volatile u32* chars = (volatile u32*)kChars;
    for (int i = 0; i < (32 * 1024) / 4; ++i) chars[i] = 0;
    volatile u16* m = tileMap();
    for (int i = 0; i < 32 * 32; ++i) m[i] = 0;
    for (int ty = 0; ty < kTH; ++ty)
        for (int tx = 0; tx < kTW; ++tx)
            m[(ty + kTopTile) * 32 + tx] = (u16)(1 + ty * kTW + tx);

    copyRomData(kMapInfoRom, (volatile u8*)kMapInfo, kW * kH * sizeof(u16));
    copyRomData(kMapWallsRom, (volatile u8*)kMapWalls, kW * kH / 2);

    REG_BG0CNT_SUB = 0x1000;
    REG_BG0HOFS_SUB = 0;
    REG_BG0VOFS_SUB = 0;
    REG_DISPCNT_SUB = 0x00010100;
    REG_MASTER_BRIGHT_SUB = 0;
    sysipc_setBottomBacklight(true);
}
}

extern "C" u32 aria_companion_adjust_ds_dispcnt(u32 dsDispCnt, u16 gbaDispCnt)
{
    if (!sActive) return dsDispCnt;

    if (!(gbaDispCnt & kWindowBits))
    {
        // Borrow WIN0 only when Aria is not using any GBA window. Inside the
        // 240-pixel GBA viewport all layers/effects pass through; outside it,
        // layers are hidden so the unstable extra DS columns cannot appear.
        *(volatile u16*)kRegWin0H = 0x00F0; // x = 0..239
        *(volatile u16*)kRegWin0V = 0x00C0; // all 192 DS scanlines
        *(volatile u16*)kRegWinIn = (*(volatile u16*)kRegWinIn & 0xFF00) | 0x003F;
        *(volatile u16*)kRegWinOut &= 0xFF00;
        sViewportWindowActive = true;
        return (dsDispCnt & ~kWindowBits) | (1 << 13);
    }

    if (sViewportWindowActive)
    {
        restoreGbaWindowRegisters();
        sViewportWindowActive = false;
    }
    return (dsDispCnt & ~kWindowBits) | (gbaDispCnt & kWindowBits);
}

void ariaCompanionInitialize(const DisplaySettings& displaySettings)
{
    sActive = false;
    sViewportWindowActive = false;
    aria_companion_hblank_mask_active = 0;
    if (gRomHeader.gameCode != kAriaUs) return;
    if (displaySettings.gbaScreen != GbaScreen::Top || displaySettings.enableCenterAndMask) return;

    // Save the user's configured top-screen brightness so HBlank can restore it
    // before scanline 0 of the next frame.
    aria_companion_saved_master_bright = REG_MASTER_BRIGHT;
    keepTopScreenMaskIrqEnabled();
    aria_companion_hblank_mask_active = 1;

    sFrame = 0;
    initSubScreen();
    for (int i = 0; i < kTW * kTH; ++i) signatures()[i] = 0xFFFFFFFF;
    sActive = true;
    redraw();
}

extern "C" int aria_companion_vblank()
{
    if (!sActive) return 0;

    // Some GBA DMA transitions can alter the emulator's forced HBlank state.
    // Reassert once per frame; the DMA stop path also preserves it for us.
    keepTopScreenMaskIrqEnabled();
    const u16 gbaDispCnt = *(u16*)&emu_ioRegisters[GBA_REG_OFFS_DISPCNT];
    REG_DISPCNT = aria_companion_adjust_ds_dispcnt(REG_DISPCNT, gbaDispCnt);

    ++sFrame;
    if ((sFrame & 7) == 0) redraw();
    return 1;
}
