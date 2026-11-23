#ifndef XENOS_TIGER_BLIT_ABI_H
#define XENOS_TIGER_BLIT_ABI_H

#include "TigerAPI.h"
typedef const void *CFRef;
typedef struct { unsigned char value[16]; } TigerUUID;

static const TigerUUID graphicsType =
    {{0xac, 0xcf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a, 0x27, 0x89, 0x90, 0x4e}};
static const TigerUUID graphicsInterface =
    {{0x67, 0x66, 0xe9, 0x4a, 0, 0, 0, 0, 0, 0, 0, 0x0a, 0x27, 0x89, 0x90, 0x4e}};
static const TigerUUID unknownInterface =
    {{0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

#ifndef XENOS_UNARMED_BLIT
#define XENOS_BLIT_FACTORY "FC82AC58-0AA6-454B-BBDC-938F8DD503DB"
#else
#define XENOS_BLIT_FACTORY "1219E9F2-0EA1-42B0-9CE7-BEC7864F4B2B"
#endif

enum {
    kBlitFill = 0, kBlitCopy = 1, kBlitCopyRegion = 4,
    kBlitFramebuffer = 0x1000, kBlitSolid = 0x6000, kBlitDestinationBit = 1,
    kBlitBadArgument = 0xe00002c2U, kBlitUnsupported = 0xe00002c7U,
    kBlitNoResources = 0xe00002beU
};

typedef struct { int x, y, width, height; } TigerBlitRect;
typedef struct { UInt32 operation[22], count; TigerBlitRect rectangle; } TigerFill;
typedef struct { int sourceX, sourceY, x, y, width, height; } TigerCopyRect;
typedef struct { UInt32 operation[22], count; TigerCopyRect rects[]; } TigerCopy;
typedef struct {
    UInt32 operation[22];
    int deltaX, deltaY;
    const void *region;
} TigerCopyRegion;
typedef int (*TigerBlitter)(void *, UInt32 options, UInt32 type, UInt32 sourceType,
                            const void *operation, const void *source);
typedef struct TigerBlitSurface TigerBlitSurface;

typedef struct {
    void *reserved;
    int (*query)(void *, TigerUUID, void **);
    UInt32 (*retain)(void *);
    UInt32 (*release)(void *);
    unsigned short version, revision;
    int (*probe)(void *, CFRef properties, io_service_t, int *score);
    int (*start)(void *, CFRef properties, io_service_t);
    int (*stop)(void *);
    int (*reset)(void *, UInt32);
    int (*capabilities)(void *, UInt32, CFRef *);
    void *getLegacyBlitProc;
    int (*flush)(void *, UInt32);
    void *waitLegacy;
    int (*synchronize)(void *, UInt32 options, UInt32 x, UInt32 y, UInt32 width, UInt32 height);
    int (*getBeamPosition)(void *, UInt32 options, int *position);
    int (*allocateSurface)(void *, UInt32 options, TigerBlitSurface *, void *cgsID);
    int (*freeSurface)(void *, UInt32 options, TigerBlitSurface *);
    int (*lockSurface)(void *, UInt32 options, TigerBlitSurface *, UInt32 *address);
    int (*unlockSurface)(void *, UInt32 options, TigerBlitSurface *, UInt32 *swapFlags);
    int (*swapSurface)(void *, UInt32 options, TigerBlitSurface *, UInt32 *swapFlags);
    int (*setDestination)(void *, UInt32, const void *);
    int (*getBlitter)(void *, UInt32, UInt32, UInt32, TigerBlitter *);
    int (*waitComplete)(void *, UInt32);
    void *extra[24];
} TigerBlitInterface;

/* Tiger IOPS reads these slots and the 88-byte operation prefix directly. */
#define ABI_OFFSET(type, field) __builtin_offsetof(type, field)
typedef char TigerProbeSlot[ABI_OFFSET(TigerBlitInterface, probe) == 0x14 ? 1 : -1];
typedef char TigerSyncSlot[ABI_OFFSET(TigerBlitInterface, synchronize) == 0x34 ? 1 : -1];
typedef char TigerBeamSlot[ABI_OFFSET(TigerBlitInterface, getBeamPosition) == 0x38 ? 1 : -1];
typedef char TigerSurfaceSlot[ABI_OFFSET(TigerBlitInterface, allocateSurface) == 0x3c ? 1 : -1];
typedef char TigerFreeSlot[ABI_OFFSET(TigerBlitInterface, freeSurface) == 0x40 ? 1 : -1];
typedef char TigerBlitDestSlot[ABI_OFFSET(TigerBlitInterface, setDestination) == 0x50 ? 1 : -1];
typedef char TigerGetBlitterSlot[ABI_OFFSET(TigerBlitInterface, getBlitter) == 0x54 ? 1 : -1];
typedef char TigerWaitSlot[ABI_OFFSET(TigerBlitInterface, waitComplete) == 0x58 ? 1 : -1];
typedef char TigerFillPrefix[ABI_OFFSET(TigerFill, count) == 0x58 ? 1 : -1];
typedef char TigerCopyPrefix[ABI_OFFSET(TigerCopy, count) == 0x58 ? 1 : -1];

#endif
