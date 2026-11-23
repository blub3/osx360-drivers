#ifndef NativeHDMI_h
#define NativeHDMI_h

#define XENON_NATIVE_HDMI_PROPERTY "xbox360-native-hdmi-profiles"
#define XENON_NATIVE_HDMI_VERSION 1
#define XENON_HDMI_I2C_FUNCTION "XenonSMCTestHDMII2C"
#define XENON_HDMI_I2C_COMMAND 0x11

static const uint8_t xenonNativeAnaRegisters[] = {0xd0, 0xd1, 0x12, 0x13, 0x14, 0x15};
static const uint16_t xenonNativeTimingRegisters[] = {0x6000, 0x6004, 0x6010, 0x6014};

typedef struct {
    uint32_t ana[sizeof(xenonNativeAnaRegisters)];
    uint32_t timing[sizeof(xenonNativeTimingRegisters) / sizeof(uint16_t)];
} XenonHDMIProfile;

// Stored as big-endian 32-bit words in Open Firmware's /chosen node.
typedef struct {
    uint32_t version;
    uint32_t control;
    XenonHDMIProfile mode720;
    XenonHDMIProfile mode1080;
} XenonHDMIProfiles;

#endif
