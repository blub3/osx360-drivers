#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/OSAtomic.h>
#include "NativeHDMI.h"
#include "XenosRegs.hpp"
#include "XenosWindowServerFramebufferProbe.h"
#include <kern/task.h>
#include <IOKit/IOUserClient.h>
#include "XenosWindowAperture.h"

extern "C" int proc_selfpid(void);
extern "C" void proc_selfname(char *, int);

namespace {
const IODisplayModeID kBootMode = 100;
const IODisplayModeID k720pMode = 101;
const UInt32 kWidth = 1920;
const UInt32 kHeight = 1080;
const UInt32 k720pWidth = 1280;
const UInt32 k720pHeight = 720;
const UInt32 kRowBytes = kWidth * sizeof(UInt32);
// Keep the 1080p stride: XenonCoreGraphics copies the same surface in both modes.
const IOPhysicalAddress kLinearFramebuffer = 0x20000000 - 2 * kXenosFramebufferLength;
const char kPixelFormats[] = IO32BitDirectPixels "\0";

enum {
    kCrtcUpdate = 0x60e8,
    kCrtcApply = 0x60ec,
    kSurfacePitch = 0x6120,
    kSurfaceEndX = 0x6134,
    kSurfaceEndY = 0x6138,
    kSurfaceUpdate = 0x6144,
    kDesktopHeight = 0x652c,
    kViewportStart = 0x6580,
    kViewportSize = 0x6584,
    kScalerEnable = 0x6590,
    kScalerUpdate = 0x65cc,
    kViewportWidth = 0x65e8,
    kScalerUpdateLock = 0x10000,
    kApplyTimeoutMs = 100,
};

// Restore the stereo PCM setup used by the working OpenBIOS build.
const struct { uint8_t reg, value; } kAudioSetup[] = {
    {0x14, 0x11}, {0x02, 0x03}, {0x03, 0x00}, {0x04, 0x18}, {0x05, 0x00},
    {0x1d, 0x40}, {0x21, 0x02}, {0x22, 0x2b}, {0x2f, 0x01}, {0x3e, 0x3f}, {0xdf, 0x10},
};

bool transmit(IOService *smc, uint8_t (&message)[16])
{
    const IOReturn error = smc->callPlatformFunction(XENON_HDMI_I2C_FUNCTION, false,
                                                      message, NULL, NULL, NULL);
    if (error != kIOReturnSuccess) {
        IOLog("XenonFramebufferTest: HDMI I2C request failed (0x%08lx)\n",
              static_cast<unsigned long>(error));
        return false;
    }
    return true;
}

bool readAna(IOService *smc, uint8_t reg, uint32_t *value)
{
    uint8_t message[16] = {XENON_HDMI_I2C_COMMAND, 0x10, 5, 0xf0, 0, 0xf0, reg};
    if (!transmit(smc, message))
        return false;
    *value = message[4] | (uint32_t(message[5]) << 8) |
             (uint32_t(message[6]) << 16) | (uint32_t(message[7]) << 24);
    return true;
}

bool writeAna(IOService *smc, uint8_t reg, uint32_t value)
{
    uint8_t message[16] = {XENON_HDMI_I2C_COMMAND, 0x60, 0, 0xf0, 0, 0, reg, 0,
                           uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    return transmit(smc, message);
}

bool writeHdmi(IOService *smc, uint8_t reg, uint8_t value)
{
    uint8_t message[16] = {XENON_HDMI_I2C_COMMAND, 0x20, 0, 0xbd, 0, 0, reg, value};
    return transmit(smc, message);
}

bool verifyHdmi(IOService *smc, uint8_t reg, uint8_t value)
{
    uint8_t message[16] = {XENON_HDMI_I2C_COMMAND, 0x10, 1, 0xbd, 0, 0xbd, reg};
    return transmit(smc, message) && message[3] == value;
}
}

class XenonFramebufferTest;
class XenosWindowApertureClient : public IOUserClient {
    OSDeclareDefaultStructors(XenosWindowApertureClient);
public:
    bool initForFramebuffer(task_t task, void *securityID, XenonFramebufferTest *fb);
    IOReturn clientClose();
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);
private:
    task_t owner;
    XenonFramebufferTest *framebuffer;
};

class XenonFramebufferTest : public IOFramebuffer {
    OSDeclareDefaultStructors(XenonFramebufferTest)

public:
    IOService *probe(IOService *provider, SInt32 *score);
    bool start(IOService *provider);
    void free();
    bool isConsoleDevice();
    using IOFramebuffer::newUserClient;
    IOReturn newUserClient(task_t task, void *securityID, UInt32 type, IOUserClient **handler);
    IODeviceMemory *getApertureRange(IOPixelAperture aperture);
    const char *getPixelFormats();
    IOItemCount getDisplayModeCount();
    IOReturn getDisplayModes(IODisplayModeID *modes);
    IOReturn getInformationForDisplayMode(IODisplayModeID mode,
                                          IODisplayModeInformation *info);
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth);
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *info);
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth);
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth);
    bool isWindowApertureOwner(task_t task);
    IOReturn connectFlags(IOIndex connection, IODisplayModeID mode, IOOptionBits *flags);

private:
    IOReturn openWindowAperture(task_t task, void *securityID, IOUserClient **handler);
    task_t framebufferOwner;
    IOLock *ownerLock;
    IODeviceMemory *framebuffer;
    IOMemoryMap *displayMap;
    volatile void *displayRegisters;
    bool nativeModeAvailable;
    IODisplayModeID currentMode;
    XenonHDMIProfiles profiles;

    void prepareNativeModes();
    bool applyMode(IOService *smc, IODisplayModeID mode);
    bool applyScanout(const XenonHDMIProfile &profile, UInt32 width, UInt32 height);
    bool supportsMode(IODisplayModeID mode) const;
    bool waitForDisplayUpdate() const;
    UInt32 readDisplay(UInt32 reg) const;
    void writeDisplay(UInt32 reg, UInt32 value);
};

OSDefineMetaClassAndStructors(XenonFramebufferTest, IOFramebuffer)

OSDefineMetaClassAndStructors(XenosWindowApertureClient, IOUserClient);

bool XenosWindowApertureClient::initForFramebuffer(task_t task, void *securityID,
                                                    XenonFramebufferTest *fb)
{
    owner = task;
    framebuffer = fb;
    return IOUserClient::initWithTask(task, securityID, kXenosWindowApertureClient, NULL);
}

IOReturn XenosWindowApertureClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn XenosWindowApertureClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                        IOMemoryDescriptor **memory)
{
    if (!options || !memory)
        return kIOReturnBadArgument;
    *memory = NULL;
    if (type != kXenosWindowApertureMemory)
        return kIOReturnUnsupported;
    if (isInactive() || !framebuffer || owner != current_task() ||
        !framebuffer->isWindowApertureOwner(owner))
        return kIOReturnNotPrivileged;

    *memory = framebuffer->getApertureRange(kIOFBSystemAperture);
    if (!*memory)
        return kIOReturnNotReady;
    *options = kIOMapInhibitCache;
    return kIOReturnSuccess;
}

bool XenonFramebufferTest::isWindowApertureOwner(task_t task)
{
    if (!ownerLock || !task || task != current_task() || isInactive())
        return false;
    IOLockLock(ownerLock);
    const bool ownsFramebuffer = task == framebufferOwner;
    IOLockUnlock(ownerLock);
    return ownsFramebuffer;
}

IOReturn XenonFramebufferTest::openWindowAperture(task_t task, void *securityID,
                                                   IOUserClient **handler)
{
    if (!handler)
        return kIOReturnBadArgument;
    *handler = NULL;
    if (!isWindowApertureOwner(task))
        return kIOReturnNotPrivileged;
    XenosWindowApertureClient *client = new XenosWindowApertureClient;
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initForFramebuffer(task, securityID, this) || !client->attach(this)) {
        client->release();
        return kIOReturnNotReady;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNotReady;
    }
    *handler = client;
    return kIOReturnSuccess;
}

IOService *XenonFramebufferTest::probe(IOService *provider, SInt32 *score)
{
    PE_Video video = {};
    IOService::getPlatform()->getConsoleInfo(&video);
    const bool modeMatches = video.v_width == kWidth && video.v_height == kHeight &&
                             video.v_rowBytes == kRowBytes &&
                             (video.v_depth == 24 || video.v_depth == 32);
    const bool useXenosBase = video.v_baseAddr == 0;
    const unsigned long framebufferBase = useXenosBase
        ? kLinearFramebuffer : video.v_baseAddr & ~3UL;
    const bool baseMatches = framebufferBase == kLinearFramebuffer;
    IOLog("XenonFramebufferTest: probe score=%ld, console=%lux%lu stride=%lu depth=%lu "
          "base=0x%08lx expected=0x%08lx\n",
          score ? static_cast<long>(*score) : 0L,
          video.v_width, video.v_height, video.v_rowBytes, video.v_depth,
          video.v_baseAddr, static_cast<unsigned long>(kLinearFramebuffer));
    if (!modeMatches || !baseMatches) {
        IOLog("XenonFramebufferTest: probe rejected (mode=%s, base=%s)\n",
              modeMatches ? "ok" : "mismatch", baseMatches ? "ok" : "mismatch");
        return NULL;
    }
    if (useXenosBase) {
        IOLog("XenonFramebufferTest: probe using Xenos framebuffer at 0x%08lx "
              "because the console base is zero\n", framebufferBase);
    }
    IOService *match = IOFramebuffer::probe(provider, score);
    IOLog("XenonFramebufferTest: probe %s after IOFramebuffer::probe, score=%ld\n",
          match ? "passed" : "rejected", score ? static_cast<long>(*score) : 0L);
    return match;
}

bool XenonFramebufferTest::start(IOService *provider)
{
    framebuffer = IODeviceMemory::withRange(kLinearFramebuffer, kXenosFramebufferLength);
    if (!framebuffer) {
        IOLog("XenonFramebufferTest: start failed to create framebuffer aperture\n");
        return false;
    }
    framebufferOwner = NULL;
    ownerLock = IOLockAlloc();
    currentMode = kBootMode;
    prepareNativeModes();
    if (!IOFramebuffer::start(provider)) {
        IOLog("XenonFramebufferTest: IOFramebuffer::start failed\n");
        return false;
    }
    IOLog("XenonFramebufferTest: attached to inherited 1920x1080 framebuffer\n");
    return true;
}

void XenonFramebufferTest::free()
{
    if (framebufferOwner)
        task_deallocate(framebufferOwner);
    if (ownerLock)
        IOLockFree(ownerLock);
    if (displayMap)
        displayMap->release();
    if (framebuffer)
        framebuffer->release();
    IOFramebuffer::free();
}

IOReturn XenonFramebufferTest::newUserClient(task_t task, void *securityID, UInt32 type,
                                             IOUserClient **handler)
{
    if (type == kXenosWindowApertureClient)
        return openWindowAperture(task, securityID, handler);
    const IOReturn result = IOFramebuffer::newUserClient(task, securityID, type, handler);
    if (result != kIOReturnSuccess || type != kIOFBServerConnectType ||
        !handler || !*handler || !task || task != current_task())
        return result;

    char process[32] = {0};
    proc_selfname(process, sizeof(process));
    const int pid = proc_selfpid();
    if (pid <= 0 || strcmp(process, "WindowServer"))
        return result;
    if (ownerLock) {
        task_reference(task);
        IOLockLock(ownerLock);
        task_t oldOwner = framebufferOwner;
        framebufferOwner = task;
        IOLockUnlock(ownerLock);
        if (oldOwner)
            task_deallocate(oldOwner);
    }
    setProperty(XENOS_WS_FRAMEBUFFER_PID, static_cast<UInt32>(pid), 32);

    IOService *pci = getProvider();
    OSIterator *clients = pci ? pci->getClientIterator() : NULL;
    const OSSymbol *function = OSSymbol::withCStringNoCopy(XENOS_WS_FRAMEBUFFER_FUNCTION);
    IOReturn queued = kIOReturnNotReady;
    if (clients && function) {
        while (OSObject *object = clients->getNextObject()) {
            IOService *service = OSDynamicCast(IOService, object);
            if (service && service->getProperty(XENOS_WS_FRAMEBUFFER_ARMED)) {
                queued = service->callPlatformFunction(function, false, task, this, NULL, NULL);
                break;
            }
        }
    }
    if (clients)
        clients->release();
    if (function)
        function->release();
    IOLog("XenonFramebufferTest: WindowServer framebuffer open pid=%d diagnostic status=%08lx\n",
          pid, static_cast<unsigned long>(queued));
    return result;
}

bool XenonFramebufferTest::isConsoleDevice()
{
    return true;
}

IODeviceMemory *XenonFramebufferTest::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !framebuffer)
        return NULL;
    framebuffer->retain();
    return framebuffer;
}

const char *XenonFramebufferTest::getPixelFormats()
{
    return kPixelFormats;
}

IOItemCount XenonFramebufferTest::getDisplayModeCount()
{
    return nativeModeAvailable ? 2 : 1;
}

IOReturn XenonFramebufferTest::getDisplayModes(IODisplayModeID *modes)
{
    modes[0] = kBootMode;
    if (nativeModeAvailable)
        modes[1] = k720pMode;
    return kIOReturnSuccess;
}

bool XenonFramebufferTest::supportsMode(IODisplayModeID mode) const
{
    return mode == kBootMode || (nativeModeAvailable && mode == k720pMode);
}

IOReturn XenonFramebufferTest::getInformationForDisplayMode(IODisplayModeID mode,
                                                           IODisplayModeInformation *info)
{
    if (!supportsMode(mode))
        return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->nominalWidth = mode == kBootMode ? kWidth : k720pWidth;
    info->nominalHeight = mode == kBootMode ? kHeight : k720pHeight;
    info->refreshRate = 60 << 16;
    return kIOReturnSuccess;
}

IOReturn XenonFramebufferTest::connectFlags(IOIndex connection, IODisplayModeID mode,
                                           IOOptionBits *flags)
{
    if (connection != 0 || !supportsMode(mode))
        return kIOReturnUnsupportedMode;
    *flags = kDisplayModeValidFlag;
    if (mode == kBootMode)
        *flags |= kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

UInt64 XenonFramebufferTest::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex)
{
    return 0;
}

IOReturn XenonFramebufferTest::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                                   IOPixelAperture aperture, IOPixelInformation *info)
{
    IODisplayModeInformation modeInfo;
    if (depth != 0 || aperture != kIOFBSystemAperture ||
        getInformationForDisplayMode(mode, &modeInfo) != kIOReturnSuccess)
        return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->bytesPerRow = kRowBytes;
    info->bitsPerPixel = 32;
    info->pixelType = kIORGBDirectPixels;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    info->componentMasks[0] = 0x00ff0000;
    info->componentMasks[1] = 0x0000ff00;
    info->componentMasks[2] = 0x000000ff;
    memcpy(info->pixelFormat, kPixelFormats, sizeof(IO32BitDirectPixels));
    info->activeWidth = modeInfo.nominalWidth;
    info->activeHeight = modeInfo.nominalHeight;
    return kIOReturnSuccess;
}

IOReturn XenonFramebufferTest::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    if (mode)
        *mode = currentMode;
    if (depth)
        *depth = 0;
    return kIOReturnSuccess;
}

IOReturn XenonFramebufferTest::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    if (depth != 0 || !supportsMode(mode))
        return kIOReturnUnsupportedMode;
    if (mode == currentMode)
        return kIOReturnSuccess;
    if (!waitForDisplayUpdate())
        return kIOReturnTimeout;

    mach_timespec_t timeout = {1, 0};
    IOService *smc = waitForService(serviceMatching("XenonSMC"), &timeout);
    if (!smc)
        return kIOReturnNotReady;
    uint32_t control = 0;
    if (!readAna(smc, 0, &control) || control != profiles.control) {
        IOLog("XenonFramebufferTest: ANA preflight failed (control=0x%08x)\n", control);
        return kIOReturnNotReady;
    }
    if (!applyMode(smc, mode)) {
        const bool restored = applyMode(smc, currentMode);
        IOLog("XenonFramebufferTest: native mode %ld failed, previous HDMI mode %s\n",
              static_cast<long>(mode), restored ? "restored" : "could not be restored");
        return kIOReturnIOError;
    }
    currentMode = mode;
    return kIOReturnSuccess;
}

void XenonFramebufferTest::prepareNativeModes()
{
    IODeviceMemory *mmio = IODeviceMemory::withRange(kXenosMmioAddress, kXenosMmioLength);
    if (mmio) {
        displayMap = mmio->map(kIOMapInhibitCache);
        mmio->release();
    }
    if (!displayMap) {
        IOLog("XenonFramebufferTest: display registers unavailable for native HDMI switching\n");
        return;
    }

    displayRegisters = reinterpret_cast<volatile void *>(displayMap->getVirtualAddress());
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (chosen) {
        OSData *data = OSDynamicCast(OSData, chosen->getProperty(XENON_NATIVE_HDMI_PROPERTY));
        if (data && data->getLength() == sizeof(profiles))
            memcpy(&profiles, data->getBytesNoCopy(), sizeof(profiles));
        chosen->release();
    }
    nativeModeAvailable = profiles.version == XENON_NATIVE_HDMI_VERSION &&
                          profiles.control == 0x2c1 &&
                          (profiles.mode720.timing[0] & 0x1fff) == 1649 &&
                          (profiles.mode720.timing[2] & 0x1fff) == 749 &&
                          (profiles.mode1080.timing[0] & 0x1fff) == 2199 &&
                          (profiles.mode1080.timing[2] & 0x1fff) == 1124 &&
                          (readDisplay(xenonNativeTimingRegisters[0]) & 0x1fff) == 2199 &&
                          readDisplay(kSurfacePitch) == kWidth &&
                          readDisplay(kSurfaceEndX) == kWidth &&
                          readDisplay(kSurfaceEndY) == kHeight &&
                          readDisplay(kViewportStart) == 0 &&
                          readDisplay(kScalerEnable) == 0 &&
                          readDisplay(kViewportSize) == ((kWidth << 16) | kHeight);
    IOLog("XenonFramebufferTest: native HDMI switching %s\n",
          nativeModeAvailable ? "available" : "disabled (missing profile or unexpected scanout)");
}

bool XenonFramebufferTest::applyMode(IOService *smc, IODisplayModeID mode)
{
    const bool is720p = mode == k720pMode;
    const XenonHDMIProfile &profile = is720p ? profiles.mode720 : profiles.mode1080;
    const UInt32 width = is720p ? k720pWidth : kWidth;
    const UInt32 height = is720p ? k720pHeight : kHeight;

    if (!writeAna(smc, 0, 0))
        return false;
    for (unsigned int i = 0; i < sizeof(xenonNativeAnaRegisters); ++i) {
        uint32_t readback;
        const uint8_t reg = xenonNativeAnaRegisters[i];
        if (!writeAna(smc, reg, profile.ana[i]) || !readAna(smc, reg, &readback) ||
            readback != profile.ana[i]) {
            IOLog("XenonFramebufferTest: ANA register 0x%02x setup failed\n", unsigned(reg));
            return false;
        }
    }
    IOSleep(5);
    if (!writeHdmi(smc, 0x03, 0x00) || !writeHdmi(smc, 0x04, 0x18) ||
        !writeHdmi(smc, 0x14, 0x03) || !writeAna(smc, 0, profiles.control))
        return false;

    if (!applyScanout(profile, width, height))
        return false;

    for (unsigned int i = 0; i < sizeof(kAudioSetup) / sizeof(kAudioSetup[0]); ++i) {
        if (!writeHdmi(smc, kAudioSetup[i].reg, kAudioSetup[i].value)) {
            IOLog("XenonFramebufferTest: HDMI PCM setup failed at 0x02%02x\n",
                  unsigned(kAudioSetup[i].reg));
            return false;
        }
    }
    if (!verifyHdmi(smc, 0x14, 0x11)) {
        IOLog("XenonFramebufferTest: HDMI PCM enable readback failed\n");
        return false;
    }
    IOLog("XenonFramebufferTest: native HDMI %lux%lu, PCM enable readback 0x11\n",
          width, height);
    return true;
}

bool XenonFramebufferTest::applyScanout(const XenonHDMIProfile &profile,
                                        UInt32 width, UInt32 height)
{
    writeDisplay(kCrtcUpdate, 1);
    for (unsigned int i = 0; i < sizeof(profile.timing) / sizeof(profile.timing[0]); ++i)
        writeDisplay(xenonNativeTimingRegisters[i], profile.timing[i]);
    writeDisplay(kCrtcUpdate, 0);

    // The refresh engine continues using a 1920-pixel stride in both modes.
    writeDisplay(kSurfaceUpdate, 1);
    writeDisplay(kSurfaceEndY, height);
    writeDisplay(kSurfaceUpdate, 0);
    writeDisplay(kScalerUpdate, kScalerUpdateLock);
    writeDisplay(kDesktopHeight, height);
    writeDisplay(kViewportStart, 0);
    writeDisplay(kViewportSize, (width << 16) | height);
    writeDisplay(kScalerEnable, 0);
    writeDisplay(kViewportWidth, width / 4 - 1);
    writeDisplay(kScalerUpdate, 0);

    writeDisplay(kCrtcApply, 1);
    if (!waitForDisplayUpdate()) {
        writeDisplay(kCrtcApply, 0);
        IOLog("XenonFramebufferTest: display update timed out\n");
        return false;
    }
    for (unsigned int i = 0; i < sizeof(profile.timing) / sizeof(profile.timing[0]); ++i) {
        if (readDisplay(xenonNativeTimingRegisters[i]) != profile.timing[i]) {
            IOLog("XenonFramebufferTest: timing readback failed at 0x%04x\n",
                  unsigned(xenonNativeTimingRegisters[i]));
            return false;
        }
    }
    return readDisplay(kSurfaceEndY) == height &&
           readDisplay(kViewportSize) == ((width << 16) | height) &&
           readDisplay(kScalerEnable) == 0;
}

bool XenonFramebufferTest::waitForDisplayUpdate() const
{
    for (unsigned int ms = 0; ms < kApplyTimeoutMs; ++ms) {
        if (readDisplay(kCrtcApply) == 0)
            return true;
        IOSleep(1);
    }
    return readDisplay(kCrtcApply) == 0;
}

UInt32 XenonFramebufferTest::readDisplay(UInt32 reg) const
{
    const UInt32 value = OSReadBigInt32(displayRegisters, reg);
    OSSynchronizeIO();
    return value;
}

void XenonFramebufferTest::writeDisplay(UInt32 reg, UInt32 value)
{
    OSWriteBigInt32(displayRegisters, reg, value);
    OSSynchronizeIO();
}
