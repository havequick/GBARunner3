#include "common.h"
#include <libtwl/dma/dmaNitro.h>
#include <libtwl/gfx/gfx.h>
#include "GbaHeader.h"
#include "SdCache/SdCache.h"
#include "SystemIpc.h"
#include "AriaCompanion.h"

// VRAM-A is already tightly packed by the emulator core. Prefer compact code
// for this game-specific companion so its richer map still fits the image.
#pragma GCC optimize ("Os")

#define ARIA_EWRAM [[gnu::section(".ewram"), gnu::noinline]]

extern GbaHeader gRomHeader;

// The emulator mirrors the GBA DISPCNT register to the DS main engine. While
// the companion is active, keep that engine in main-memory FIFO display mode:
// its hidden 2D output is still captured by the stock center-and-mask path,
// while the physical bottom screen receives the map framebuffer.
extern "C"
{
u32 gAriaCompanionDisplayModeOverride = 0;
}

// Castlevania: Aria of Sorrow (USA) second-screen live map prototype.
// Supported A2CE ROMs:
// - USA retail: abd71fe01ebb201bcc133074db1dd8c5253776c7
// - Symphony of the Colors v1.0: 02f66b8c85b324e49fb8c16348a8251b920e16cf
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

// Aria's native map metadata. It is copied once from the user's ROM into main
// RAM so the live renderer never performs an SD-cache load in VBlank.
constexpr u32 kMapInfoRom = 0x08116650;
constexpr u32 kMapWallsRom = 0x08117DD0;

constexpr int kW = 64, kH = 40;
constexpr int kTW = 32, kTH = 20;
constexpr int kMapOffsetX = 4, kMapOffsetY = 2;
constexpr int kScreenW = 256, kScreenH = 192;
constexpr int kMapTop = 16;
constexpr int kMapDmaChannel = 3;

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
u32 sFrame = 0;

[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
u16 sFrameBuffer[kScreenW * kScreenH];

[[gnu::section(".ewram.bss")]]
u32 sSignatures[kTW * kTH];

[[gnu::section(".ewram.bss")]]
u16 sMapInfo[kW * kH];

[[gnu::section(".ewram.bss")]]
u8 sMapWalls[kW * kH / 2];

constexpr u16 rgb15(int r, int g, int b) { return (u16)(r | (g << 5) | (b << 10)); }

constexpr u16 kColors[16] =
{
    rgb15(0, 0, 0),     // clear
    rgb15(23, 27, 31),  // edge
    rgb15(31, 4, 5),    // save
    rgb15(31, 22, 3),   // warp
    rgb15(3, 6, 11),    // revealed
    rgb15(10, 15, 21),  // revealed edge
    rgb15(31, 31, 9),   // player
    0,
    0,
    rgb15(7, 14, 25),   // visited
    0,
    rgb15(2, 4, 7),     // secret edge
    0, 0, 0, 0
};

inline volatile u16* tilePtr(int tx, int ty)
{
    return &sFrameBuffer[(kMapTop + ty * 8) * kScreenW + tx * 8];
}
inline volatile u32* signatures() { return sSignatures; }
inline volatile const u16* mapInfo() { return sMapInfo; }
inline volatile const u8* mapWalls() { return sMapWalls; }

ARIA_EWRAM void copyRomData(u32 romAddress, volatile u8* dst, u32 byteCount)
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

ARIA_EWRAM u16 infoAt(int x, int y)
{
    if ((unsigned)x >= kW || (unsigned)y >= kH) return 0xFFFF;
    return mapInfo()[y * kW + x];
}

ARIA_EWRAM u8 wallAt(int x, int y)
{
    if ((unsigned)x >= kW || (unsigned)y >= kH) return 0;
    const u8 packed = mapWalls()[y * (kW / 2) + (x >> 1)];
    return (x & 1) ? (packed & 0xF) : (packed >> 4);
}

ARIA_EWRAM bool excludedFromAreaMap(int x, int y)
{
    return (x == 4 && y == 13) || (x == 12 && y == 13) ||
           (x == 13 && y == 13) || (x == 15 && y == 22) ||
           (x == 47 && y == 20);
}

ARIA_EWRAM CellState mapState(int screenX, int screenY)
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

ARIA_EWRAM u8 fillColor(CellState state)
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

ARIA_EWRAM u8 edgeColor(CellState state, int kind)
{
    if (kind == 1 || kind == 2) return COLOR_SECRET_EDGE;
    return state == CELL_REVEALED ? COLOR_REVEALED_EDGE : COLOR_EDGE;
}

inline void px(volatile u16* t, int x, int y, u8 c)
{
    t[y * kScreenW + x] = kColors[c];
}

ARIA_EWRAM void horizontal(volatile u16* t, int x0, int y, u8 color)
{
    for (int x = x0; x < x0 + 4; ++x) px(t, x, y, color);
}

ARIA_EWRAM void vertical(volatile u16* t, int x, int y0, u8 color)
{
    for (int y = y0; y < y0 + 4; ++y) px(t, x, y, color);
}

ARIA_EWRAM void drawCell(volatile u16* t, int cx, int cy, int screenX, int screenY)
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

ARIA_EWRAM bool playerMapPos(int& x, int& y)
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

ARIA_EWRAM u32 signature(int tx, int ty, int pxm, int pym, bool showPlayer)
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

ARIA_EWRAM void drawTile(int tx, int ty, int pxm, int pym, bool showPlayer)
{
    volatile u16* t = tilePtr(tx, ty);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            t[y * kScreenW + x] = kColors[COLOR_CLEAR];
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

ARIA_EWRAM void redraw()
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

ARIA_EWRAM void initMapDisplay()
{
    for (int i = 0; i < kScreenW * kScreenH; ++i)
        sFrameBuffer[i] = kColors[COLOR_CLEAR];

    copyRomData(kMapInfoRom, (volatile u8*)sMapInfo, sizeof(sMapInfo));
    copyRomData(kMapWallsRom, (volatile u8*)sMapWalls, sizeof(sMapWalls));

    // Preserve every BG/OBJ bit produced by the emulator, changing only the
    // physical main-engine output selector. Display capture source A remains
    // the internal 2D composite, exactly as in stock center-and-mask mode.
    gAriaCompanionDisplayModeOverride = 3u << 16;
    REG_DISPCNT = (REG_DISPCNT & ~(3u << 16)) | gAriaCompanionDisplayModeOverride;
    REG_MASTER_BRIGHT = 0;
    sysipc_setBottomBacklight(true);
}

inline void startMapDma()
{
    dma_ntrSetParams(
        kMapDmaChannel,
        sFrameBuffer,
        &REG_MMEM_FIFO,
        DMACNT_ENABLE |
        DMACNT_MODE_MMEM_DISP_FIFO |
        DMACNT_REPEAT |
        DMACNT_32BIT |
        DMACNT_SRC_MODE_INCREMENT |
        DMACNT_DST_MODE_FIXED |
        DMACNT_COUNT(4));
}
}

extern "C" ARIA_EWRAM void aria_companion_set_dispcnt(u32 dsDispCnt)
{
    if (gAriaCompanionDisplayModeOverride)
    {
        dsDispCnt = (dsDispCnt & ~(3u << 16)) | gAriaCompanionDisplayModeOverride;
    }
    REG_DISPCNT = dsDispCnt;
}

void ariaCompanionInitialize(const DisplaySettings& displaySettings)
{
    sActive = false;
    gAriaCompanionDisplayModeOverride = 0;
    if (gRomHeader.gameCode != kAriaUs) return;
    if (displaySettings.gbaScreen != GbaScreen::Top || !displaySettings.enableCenterAndMask) return;

    sFrame = 0;
    initMapDisplay();
    for (int i = 0; i < kTW * kTH; ++i) signatures()[i] = 0xFFFFFFFF;
    sActive = true;
    redraw();
    dc_flushRange(sFrameBuffer, sizeof(sFrameBuffer));
}

extern "C" int aria_companion_vblank()
{
    if (!sActive) return 0;

    // FIFO display DMA must be restarted for each frame. Stop it before
    // touching the framebuffer so the next scanout sees a coherent image.
    dma_ntrStopSafe(kMapDmaChannel);
    ++sFrame;
    if ((sFrame & 7) == 0)
    {
        redraw();
        dc_flushRange(sFrameBuffer, sizeof(sFrameBuffer));
    }
    startMapDma();
    return 1;
}
