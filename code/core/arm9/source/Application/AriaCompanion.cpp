#include "common.h"
#include <libtwl/mem/memVram.h>
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include "GbaHeader.h"
#include "SystemIpc.h"
#include "AriaCompanion.h"

extern GbaHeader gRomHeader;

// Castlevania: Aria of Sorrow (USA) second-screen live map prototype.
// Target ROM: A2CE / SHA-1 abd71fe01ebb201bcc133074db1dd8c5253776c7
namespace
{
constexpr u32 kAriaUs = GAMECODE("A2CE");
constexpr uintptr_t kEwramLo = 0x02000000;
constexpr uintptr_t kEwramHi = 0x02040000;
constexpr uintptr_t kMap = 0x020000B4;
constexpr uintptr_t kMapOrigin = 0x0200008C;
constexpr uintptr_t kBg1X = 0x0200A09A;
constexpr uintptr_t kBg1Y = 0x0200A09E;
constexpr uintptr_t kPlayerPtr = 0x02013110;
constexpr uintptr_t kPlayerX = 0x42;
constexpr uintptr_t kPlayerY = 0x46;

constexpr int kW = 64, kH = 40;
constexpr int kTW = 32, kTH = 20;
constexpr int kTopTile = 2;
constexpr uintptr_t kChars = 0x06200000;
constexpr uintptr_t kMapVram = 0x06208000;
constexpr uintptr_t kSignatures = kMapVram + 32 * 32 * sizeof(u16);
constexpr uintptr_t kPal = 0x05000400;

bool sActive = false;
u32 sFrame = 0;

constexpr u16 rgb15(int r, int g, int b) { return (u16)(r | (g << 5) | (b << 10)); }
inline volatile u8* tilePtr(int n) { return (volatile u8*)(kChars + n * 32); }
inline volatile u16* tileMap() { return (volatile u16*)kMapVram; }
inline volatile u16* signatures() { return (volatile u16*)kSignatures; }

inline void px(volatile u8* t, int x, int y, u8 c)
{
    volatile u8& b = t[y * 4 + (x >> 1)];
    b = (x & 1) ? (u8)((b & 0x0F) | (c << 4)) : (u8)((b & 0xF0) | c);
}

void cell(volatile u8* t, int cx, int cy, u8 c)
{
    const int x0 = cx * 4, y0 = cy * 4;
    for (int y = y0; y < y0 + 4; ++y)
        for (int x = x0; x < x0 + 4; ++x)
            px(t, x, y, c);
}

u8 mapState(int x, int y)
{
    if ((unsigned)x >= kW || (unsigned)y >= kH) return 0;
    const int half = x >> 5, bit = x & 31;
    const uintptr_t p = kMap + half * (40 * 8) + y * 8;
    const u32 m = 1u << bit;
    const u32 visited = *(volatile const u32*)(p + 0);
    const u32 revealed = *(volatile const u32*)(p + 4);
    if (visited & m) return 2;
    if (revealed & m) return 1;
    return 0;
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
    x = (origin & 0x7F) + (((s32)camX + entX) >> 8);
    y = ((origin >> 7) & 0x7F) + (((s32)camY + entY) >> 8);
    return (unsigned)x < kW && (unsigned)y < kH;
}

u16 signature(int tx, int ty, int pxm, int pym, bool showPlayer)
{
    u16 v = 0;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx)
            v |= (u16)(mapState(tx * 2 + cx, ty * 2 + cy) & 3) << ((cy * 2 + cx) * 2);
    if (showPlayer && (pxm >> 1) == tx && (pym >> 1) == ty)
        v |= (u16)((((pym & 1) << 1) | (pxm & 1)) + 1) << 8;
    return v;
}

void drawTile(int tx, int ty, int pxm, int pym, bool showPlayer)
{
    volatile u8* t = tilePtr(1 + ty * kTW + tx);
    for (int i = 0; i < 32; ++i) t[i] = 0;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx)
            cell(t, cx, cy, mapState(tx * 2 + cx, ty * 2 + cy));
    if (showPlayer && (pxm >> 1) == tx && (pym >> 1) == ty)
    {
        const int cx = pxm & 1, cy = pym & 1;
        const int x = cx * 4 + 1, y = cy * 4 + 1;
        px(t, x, y, 3); px(t, x + 1, y, 3); px(t, x, y + 1, 3); px(t, x + 1, y + 1, 3);
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
            const u16 v = signature(tx, ty, x, y, showPlayer);
            if (signatures()[i] != v)
            {
                signatures()[i] = v;
                drawTile(tx, ty, x, y, showPlayer);
            }
        }
}

void initSubScreen()
{
    // H and I hold the relocated GBA BIOS and the ROM cache lookup table.
    // VRAM C is otherwise only needed by the center-and-mask capture path,
    // which is deliberately disabled while the companion is active.
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);

    volatile u16* pal = (volatile u16*)kPal;
    pal[0] = rgb15(0, 0, 0);
    pal[1] = rgb15(7, 9, 12);
    pal[2] = rgb15(12, 20, 31);
    pal[3] = rgb15(31, 29, 7);

    volatile u32* chars = (volatile u32*)kChars;
    for (int i = 0; i < (32 * 1024) / 4; ++i) chars[i] = 0;
    volatile u16* m = tileMap();
    for (int i = 0; i < 32 * 32; ++i) m[i] = 0;
    for (int ty = 0; ty < kTH; ++ty)
        for (int tx = 0; tx < kTW; ++tx)
            m[(ty + kTopTile) * 32 + tx] = (u16)(1 + ty * kTW + tx);

    REG_BG0CNT_SUB = 0x1000;
    REG_BG0HOFS_SUB = 0;
    REG_BG0VOFS_SUB = 0;
    REG_DISPCNT_SUB = 0x00010100;
    REG_MASTER_BRIGHT_SUB = 0;
    sysipc_setBottomBacklight(true);
}
}

void ariaCompanionInitialize(const DisplaySettings& displaySettings)
{
    sActive = false;
    if (gRomHeader.gameCode != kAriaUs) return;
    if (displaySettings.gbaScreen != GbaScreen::Top || displaySettings.enableCenterAndMask) return;
    sFrame = 0;
    initSubScreen();
    for (int i = 0; i < kTW * kTH; ++i) signatures()[i] = 0xFFFF;
    sActive = true;
    redraw();
}

extern "C" int aria_companion_vblank()
{
    if (!sActive) return 0;
    ++sFrame;
    if ((sFrame & 3) == 0) redraw();
    return 1;
}
