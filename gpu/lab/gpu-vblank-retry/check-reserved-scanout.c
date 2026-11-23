#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define XBOX360_RAM_SIZE            0x20000000
#define XBOX360_FB_SIZE             0x1000000 // Direct framebuffer + space for bounce buffer used in XNU (8MB x2, good for 1920x1080x32)
#define XBOX360_FB_BASE             (XBOX360_RAM_SIZE - XBOX360_FB_SIZE)
#define XBOX360_VBLANK_SIZE         0x00800000
#define XBOX360_VBLANK_BASE         (XBOX360_FB_BASE - XBOX360_VBLANK_SIZE)
#define XBOX360_VBLANK_PROPERTY     "xbox360-vblank-scanout"
#define XBOX360_RAM_SIZE_ACTUAL     XBOX360_VBLANK_BASE
#define XBOX360_RAM_TYPE            "GDDR3"
typedef struct { uint32_t base, size; } Bank;
typedef struct { Bank PhysicalDRAM[26]; } BootArgs;
int main(void) {
    BootArgs args, *xnu_boot_args = &args;
    memset(&args, 0xff, sizeof(args));
    memset(xnu_boot_args->PhysicalDRAM, 0, sizeof(xnu_boot_args->PhysicalDRAM));
    xnu_boot_args->PhysicalDRAM[0].base = 0x00000000; // 0MB start
    xnu_boot_args->PhysicalDRAM[0].size = 0x02000000; // 32MB size TODO: This reduces freezes, but they still may occur.
    xnu_boot_args->PhysicalDRAM[1].base = 0x10000000; // 256MB start
    xnu_boot_args->PhysicalDRAM[1].size = XBOX360_RAM_SIZE_ACTUAL - xnu_boot_args->PhysicalDRAM[1].base;


    assert(XBOX360_VBLANK_BASE == 0x1e800000U);
    assert(XBOX360_RAM_SIZE_ACTUAL == XBOX360_VBLANK_BASE);
    assert(XBOX360_VBLANK_BASE + XBOX360_VBLANK_SIZE == XBOX360_FB_BASE);
    assert(XBOX360_FB_BASE == 0x1f000000U);
    assert(XBOX360_FB_BASE + 2 * XBOX360_VBLANK_SIZE == XBOX360_RAM_SIZE);
    assert(args.PhysicalDRAM[1].base + args.PhysicalDRAM[1].size == XBOX360_VBLANK_BASE);
    assert(args.PhysicalDRAM[0].size + args.PhysicalDRAM[1].size == 264U * 1024 * 1024);
    for (int i = 2; i < 26; ++i)
        assert(args.PhysicalDRAM[i].size == 0);
    assert(1280 * 736 * 4 <= XBOX360_VBLANK_SIZE);
    assert(1920 * 1088 * 4 <= XBOX360_VBLANK_SIZE);
    puts("Reservation verified for 720p and 1080p, both desktop buffers disjoint, Tiger banks total 264 MiB.");
}
