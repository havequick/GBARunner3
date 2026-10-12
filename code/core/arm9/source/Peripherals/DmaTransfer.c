#include "common.h"
#include <string.h>
#include <libtwl/gfx/gfxStatus.h>
#include "Emulator/IoRegisters.h"
#include "VirtualMachine/VMDtcm.h"
#include "MemCopy.h"
#include "GbaIoRegOffsets.h"
#include "SdCache/SdCache.h"
#include "MemoryEmulator/HiCodeCacheMapping.h"
#include "Peripherals/Sound/GbaSound9.h"
#include "VirtualMachine/VMNestedIrq.h"
#include "GbaDma.h"
#include "MemoryEmulator/RomDefs.h"
#include "DmaTransfer.h"

extern volatile u32 aria_companion_hblank_mask_active;

DTCM_DATA dma_state_t dma_state;

void dma_immTransfer16(u32 src, u32 dst, u32 byteCount, int srcStep, int dstStep);
void dma_immTransfer32(u32 src, u32 dst, u32 byteCount, int srcStep, int dstStep);

extern void dma_immTransferSafe16(u32 src, u32 dst, u32 byteCount, int srcStep, int dstStep);
extern void dma_immTransferSafe16BadSrc(u32 dst, u32 byteCount, int dstStep);
extern void dma_immTransferSafe32(u32 src, u32 dst, u32 byteCount, int srcStep, int dstStep);
extern void dma_immTransferSafe32BadSrc(u32 dst, u32 byteCount, int dstStep);

extern u32 dma_transferRegister;
extern s8 dma_stepTable[4];

extern u32 emu_hblankDmaSkipInstruction;
extern u32 emu_hblankDmaJumpInstructions[4];

static u32 hblankDmaSkipInstruction;
static u32 hblankDmaSkipDuringVblankInstruction;
static u32 hblankDmaJumpInstructions[4];

extern u32 emu_vblankDmaJumpInstruction;
extern u32 emu_vblankDmaJumpInstructions[4];

static u32 vblankDmaJumpInstruction;
static u32 vblankDmaJumpInstructions[4];

extern u32 emu_soundDma1JumpInstruction;
extern u32 emu_soundDma2JumpInstruction;

static u32 soundDma1JumpInstruction;
static u32 soundDma2JumpInstruction;

static inline void updateHBlankIrqForChannelStop(void)
{
    if (!(dma_state.dmaFlags & DMA_FLAG_HBLANK_MASK))
    {
        emu_hblankDmaSkipInstruction = hblankDmaSkipInstruction;
        if (!aria_companion_hblank_mask_active)
            vm_forcedIrqMask &= ~(1 << 1);
        else
            vm_forcedIrqMask |= 1 << 1;
        u32 gbaDispStat = *(u16*)&emu_ioRegisters[4];
        if (!(gbaDispStat & (1 << 4)) && !aria_companion_hblank_mask_active)
        {
            gfx_setHBlankIrqEnabled(false);
        }
    }
}

static inline void updateForVBlankChannelStop(void)
{
    if (!(dma_state.dmaFlags & DMA_FLAG_VBLANK_MASK))
    {
        emu_vblankDmaJumpInstruction = 0;
    }
}

static inline void updateArm7IrqForChannelStop(void)
{
    if (!(dma_state.dmaFlags & DMA_FLAG_SOUND_MASK))
    {
        vm_forcedIrqMask &= ~(1 << 16); // arm7 irq
    }
}

static inline void triggerDmaIrqIfEnabled(int channel, u32 control)
{
    if (control & GBA_DMA_CONTROL_IRQ)
    {
        vm_emulatedIfImeIe |= 1 << (8 + channel);
    }
}

static inline int getSrcStep(u32 control)
{
    return dma_stepTable[(control >> GBA_DMA_CONTROL_SRC_STEP_SHIFT) & GBA_DMA_CONTROL_SRC_STEP_MASK];
}

static inline int getDstStep(u32 control)
{
    return dma_stepTable[(control >> GBA_DMA_CONTROL_DST_STEP_SHIFT) & GBA_DMA_CONTROL_DST_STEP_MASK];
}

ITCM_CODE void dma_dmaTransfer(int channel)
{
    GbaDmaChannel* dmaIoBase = (GbaDmaChannel*)&emu_ioRegisters[GBA_REG_OFFS_DMA0SAD + channel * 0xC];
    u32 control = dmaIoBase->control;
    u32 count = dmaIoBase->count;
    if (count == 0)
        count = 0x10000;
    u32 byteCount;
    if (control & GBA_DMA_CONTROL_32BIT)
        byteCount = count << 2;
    else
        byteCount = count << 1;
    int srcStep = getSrcStep(control);
    u32 src = dma_state.channels[channel].curSrc;
    dma_state.channels[channel].curSrc += srcStep * byteCount;
    int dstStep = getDstStep(control);
    u32 dst;
    if (((control >> GBA_DMA_CONTROL_DST_STEP_SHIFT) & GBA_DMA_CONTROL_DST_STEP_MASK) != GBA_DMA_CONTROL_DST_STEP_RELOAD)
    {
        dst = dma_state.channels[channel].curDst;
        dma_state.channels[channel].curDst += dstStep * byteCount;
    }
    else
    {
        // reload
        dst = dmaIoBase->dst;
        dma_state.channels[channel].curDst = dst;
    }
    if (src >= 0x08000000 && src < 0x0E000000)
    {
        // rom always forces increment
        srcStep = 1;
    }
    if (control & GBA_DMA_CONTROL_32BIT)
        dma_immTransfer32(src, dst, byteCount, srcStep, dstStep);
    else
        dma_immTransfer16(src, dst, byteCount, srcStep, dstStep);
    triggerDmaIrqIfEnabled(channel, control);
    if (!(control & GBA_DMA_CONTROL_REPEAT))
    {
        dmaIoBase->control = control & ~GBA_DMA_CONTROL_ENABLED;
        dma_state.dmaFlags &= ~DMA_FLAG_HBLANK(channel);
        dma_state.dmaFlags &= ~DMA_FLAG_VBLANK(channel);
        updateHBlankIrqForChannelStop();
        updateForVBlankChannelStop();
        emu_hblankDmaJumpInstructions[channel] = 0;
        emu_vblankDmaJumpInstructions[channel] = 0;
    }
}

void dma_init(void)
{
    memset(&dma_state, 0, sizeof(dma_state));

    hblankDmaSkipDuringVblankInstruction = emu_hblankDmaSkipInstruction;
    hblankDmaSkipInstruction = (hblankDmaSkipDuringVblankInstruction & ~0xF0000000) | 0xE0000000; // always condition
    hblankDmaJumpInstructions[0] = emu_hblankDmaJumpInstructions[0];
    hblankDmaJumpInstructions[1] = emu_hblankDmaJumpInstructions[1];
    hblankDmaJumpInstructions[2] = emu_hblankDmaJumpInstructions[2];
    hblankDmaJumpInstructions[3] = emu_hblankDmaJumpInstructions[3];

    // Initially no hblank dma is enabled. Replace the instructions with nops.
    emu_hblankDmaSkipInstruction = hblankDmaSkipInstruction;
    emu_hblankDmaJumpInstructions[0] = 0;
    emu_hblankDmaJumpInstructions[1] = 0;
    emu_hblankDmaJumpInstructions[2] = 0;
    emu_hblankDmaJumpInstructions[3] = 0;

    vblankDmaJumpInstruction = emu_vblankDmaJumpInstruction;
    vblankDmaJumpInstructions[0] = emu_vblankDmaJumpInstructions[0];
    vblankDmaJumpInstructions[1] = emu_vblankDmaJumpInstructions[1];
    vblankDmaJumpInstructions[2] = emu_vblankDmaJumpInstructions[2];
    vblankDmaJumpInstructions[3] = emu_vblankDmaJumpInstructions[3];

    // Initially no vblank dma is enabled. Replace the instructions with nops.
    emu_vblankDmaJumpInstruction = 0;
    emu_vblankDmaJumpInstructions[0] = 0;
    emu_vblankDmaJumpInstructions[1] = 0;
    emu_vblankDmaJumpInstructions[2] = 0;
    emu_vblankDmaJumpInstructions[3] = 0;

    soundDma1JumpInstruction = emu_soundDma1JumpInstruction;
    soundDma2JumpInstruction = emu_soundDma2JumpInstruction;

    // Initially no sound dma is enabled. Replace the instructions with nops.
    emu_soundDma1JumpInstruction = 0;
    emu_soundDma2JumpInstruction = 0;
}

ITCM_CODE static u32 translateAddress(u32 address)
{
    switch (address >> 24)
    {
        case 2:
            address &= ~0x00FC0000;
            break;
        case 3:
            address &= ~0x00FF8000;
            break;
        case 5:
        case 7:
            address &= ~0x00FFFC00;
            break;
        case 6:
        {
            address &= ~0x00FE0000;
            if (!(address & 0x10000))
            {
                break;
            }

            if (address & 0x4000)
            {
                address += 0x003F0000;
            }
            else
            {
                u32 dispCnt = emu_ioRegisters[GBA_REG_OFFS_DISPCNT];
                if ((dispCnt & 7) < 3 || address & 0x8000)
                {
                    address += 0x003F0000;
                }
            }
            break;
        }
    }
    return address;
}

ITCM_CODE static u32 dmaIoBaseToChannel(const GbaDmaChannel* dmaIoBase)
{
    u32 regOffset = (u32)dmaIoBase - (u32)emu_ioRegisters;
    return (regOffset - GBA_REG_OFFS_DMA0SAD) / (GBA_REG_OFFS_DMA1SAD - GBA_REG_OFFS_DMA0SAD);
}

static inline void dma_immTransferRomSrc(u32 src, u32 dst, u32 byteCount, bool dma32)
{
    u8* dstPtr = (u8*)dst;
    do
    {
        const void* cacheBlock = sdc_getRomBlock(src);
        u32 offset = src & SDC_BLOCK_MASK;
        u32 remainingInBlock = SDC_BLOCK_SIZE - offset;
        if (remainingInBlock > byteCount)
            remainingInBlock = byteCount;
        if (dma32)
            mem_copy32((const u8*)cacheBlock + offset, dstPtr, remainingInBlock);
        else
            mem_copy16((const u8*)cacheBlock + offset, dstPtr, remainingInBlock);
        src += remainingInBlock;
        dstPtr += remainingInBlock;
        byteCount -= remainingInBlock;
    } while (byteCount > 0);
}

static inline bool fastDmaSourceAllowed(u32 srcRegion)
{
    return 0b0011111111001100 & (1 << srcRegion);
}

static inline bool fastDmaDestinationAllowed(u32 dstRegion)
{
    u32 mask = 0b0000000011001100;
    if ((emu_ioRegisters[GBA_REG_OFFS_DISPCNT] & 7) >= 3)
    {
        mask |= 1 << 6;
    }
    return mask & (1 << dstRegion);
}

// remainder unchanged
