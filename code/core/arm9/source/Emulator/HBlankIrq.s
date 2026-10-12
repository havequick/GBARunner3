.section ".itcm", "ax"
.altmacro

#include "AsmMacros.inc"

.extern dma_state
.extern aria_companion_hblank_mask_active
.extern aria_companion_saved_master_bright

/// @brief Performs hblank irq tasks for the emulator.
///        - Masks DS-only scanlines for the Aria companion without display capture.
///        - Emulates HDMA.
/// @param r0-r12 Preserved
/// @param r13 Trashed.
/// @param lr Return address.
arm_func emu_hblankIrq
    // Aria companion: a GBA frame is 160 lines high while the DS LCD is 192.
    // Black scanlines 160-191 using master brightness, then restore the user's
    // configured brightness immediately before scanline 0. This avoids the
    // one-frame latency of the normal center-and-mask capture path.
    ldr r13,= aria_companion_hblank_mask_active
    ldr r13, [r13]
    cmp r13, #0
    beq 2f

    mov r13, #0x04000000
    ldrh lr, [r13, #6] // REG_VCOUNT
    cmp lr, #159
    bne 1f
    mov lr, #0x8000
    orr lr, lr, #16
    strh lr, [r13, #0x6C] // REG_MASTER_BRIGHT: black
    b 2f
1:
    cmp lr, #191
    bne 2f
    ldr r13,= aria_companion_saved_master_bright
    ldrh lr, [r13]
    mov r13, #0x04000000
    strh lr, [r13, #0x6C] // restore before next visible frame
2:

    // Don't trigger hblank irq on scanlines beyond the gba screen
    mov r13, #0x04000000
    ldrh lr, [r13, #6]
    cmp lr, #260
    sublo r13, lr, #160
    rsblos r13, r13, #31
    bichs r4, r4, #2 // HBLANK IRQ

    cmp lr, #160
    // This is replaced by a b instruction when no hblank dma is in use
.global emu_hblankDmaSkipInstruction
emu_hblankDmaSkipInstruction:
    bge emu_hblankIrqReturn

    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12}
#ifndef GBAR3_TEST
    // These are replaced by nops when not active
.global emu_hblankDmaJumpInstructions
emu_hblankDmaJumpInstructions:
    bl dma_dma0Transfer
    bl dma_dma1Transfer
    bl dma_dma2Transfer
    bl dma_dma3Transfer
#endif
    pop {r0-r3,r12}
    b emu_hblankIrqReturn
