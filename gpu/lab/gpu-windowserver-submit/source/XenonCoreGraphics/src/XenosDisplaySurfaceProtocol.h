#ifndef XENOS_DISPLAY_SURFACE_PROTOCOL_H
#define XENOS_DISPLAY_SURFACE_PROTOCOL_H

enum {
  kXenosDiagnosticSurfaceID = 0x584e0001,
  kXenosDiagnosticSurfaceFirst = 0xff173ba2U,
  kXenosDiagnosticSurfaceSecond = 0xffb21163U,
  kXenosDiagnosticSurfaceFormat = 32
};


// The Tiger PowerPC IOAccelReadLockSurface wire reply uses 32-bit addresses.
typedef struct {
  unsigned address[4];
  unsigned rowBytes, width, height, pixelFormat, flags;
  unsigned colorTemperature[4], typeDependent[4];
} XenosTigerSurfaceInfo;

#endif
