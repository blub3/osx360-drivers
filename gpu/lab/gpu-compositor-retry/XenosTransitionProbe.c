#include "TigerAPI.h"
typedef void *Window;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct {
    int reserved, type, options, window;
    const float *backColor;
} Transition;

int main(void)
{
    void *hit = dlopen("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/"
        "HIToolbox.framework/Versions/A/HIToolbox", kDlopenNow);
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/"
        "Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", kDlopenNow);
    if (!hit || !cg)
        return 1;
    int (*create)(unsigned, unsigned, const Rect *, Window *) = dlsym(hit, "CreateNewWindow");
    void (*show)(Window) = dlsym(hit, "ShowWindow");
    void (*dispose)(Window) = dlsym(hit, "DisposeWindow");
    void (*events)(double) = dlsym(hit, "RunCurrentEventLoop");
    int (*windowList)(int, int, int, int *, int *) = dlsym(cg, "CGSGetWindowList");
    int (*connection)(void) = dlsym(cg, "_CGSDefaultConnection");
    int (*newTransition)(int, const Transition *, int *) = dlsym(cg, "CGSNewTransition");
    int (*releaseTransition)(int, int) = dlsym(cg, "CGSReleaseTransition");
    printf("Transition APIs create=%p windowList=%p connection=%p transition=%p release=%p\n",
           create, windowList, connection, newTransition, releaseTransition);
    if (!create || !show || !dispose || !events || !windowList || !connection ||
        !newTransition || !releaseTransition)
        return 1;
    int cid = connection(), count = 0, ids[8];
    if (cid <= 0 || windowList(cid, cid, 8, ids, &count) || count)
        return 1;
    Window window = 0;
    const Rect rect = {180, 240, 252, 352};
    if (create(13, 1U << 21, &rect, &window) || !window)
        return 1;
    show(window);
    events(0.1);
    int wid = 0, handle = 0;
    if (!windowList(cid, cid, 8, ids, &count) && count == 1)
        wid = ids[0];
    const Transition transition = {0, 2, 4, wid, 0};
    int status = wid > 0 ? newTransition(cid, &transition, &handle) : 10000;
    printf("Window transition cid=%d wid=%d create=%d handle=%d\n", cid, wid, status, handle);
    if (!status && handle)
        releaseTransition(cid, handle);
    dispose(window);
    /* The transition itself is optional. Tiger calls the format callback
       before rejecting a transition on a software display. */
    return wid <= 0 || (status != 0 && status != 1004);
}
