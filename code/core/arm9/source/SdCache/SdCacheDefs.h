#pragma once

// Reserve 32 KiB for the Aria companion's full 256x192 FIFO framebuffer.
// This trims the ROM cache by 3.125% while keeping its 4 KiB block geometry.
#define SDC_SIZE                (992 * 1024)
#define SDC_BLOCK_SHIFT         12
#define SDC_BLOCK_SIZE          (1 << SDC_BLOCK_SHIFT)
#define SDC_BLOCK_MASK          (SDC_BLOCK_SIZE - 1)
#define SDC_BLOCK_COUNT         (SDC_SIZE / SDC_BLOCK_SIZE)
#define SDC_BLOCK_INVALID       0xFFFF
#define SDC_ROM_BLOCK_INVALID   0xFFFF

#define SDC_ROM_BLOCK_COUNT     (32 * 1024 * 1024 / SDC_BLOCK_SIZE)
