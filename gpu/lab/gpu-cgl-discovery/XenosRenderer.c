#include "TigerAPI.h"

/* Tiger GLEngine consumes one 56-byte GLD renderer record per driver. */
typedef struct {
    UInt32 next;
    UInt32 rendererID;
    UInt32 flags;
    UInt32 bufferModes;
    UInt32 colorModes;
    unsigned char reserved[36];
} TigerRendererInfo;
typedef char TigerRendererInfoSize[sizeof(TigerRendererInfo) == 56 ? 1 : -1];

void gldInitializeLibrary(void)
{
}

int gldGetVersion(int *major, int *minor, int *revision, int *renderer)
{
    *major = 2;
    *minor = 4;
    *revision = 11;
    *renderer = kXenosGLDRendererID;
    return 1;
}

int gldGetRendererInfo(TigerRendererInfo *info, UInt32 mask)
{
    XenosDisplay display;
    if (!XenosFindDisplay(&display) || mask != display.mask)
        return kCGLBadMatch; /* GLEngine skips this plug-in for an unsupported mask. */

    *info = (TigerRendererInfo){.rendererID = kXenosGLDRendererID};
    return 0;
}

int gldChoosePixelFormat(void **format, const int *attributes)
{
    (void)attributes;
    *format = 0;
    return 0;
}

/* GLEngine checks the complete export table when loading a plug-in.
 * For this discovery test, all operations beyond enumeration fail closed.
 */
#define GLD_UNSUPPORTED(name) int name(void) { return kCGLBadMatch; }
#include "UnsupportedGLD.def"
