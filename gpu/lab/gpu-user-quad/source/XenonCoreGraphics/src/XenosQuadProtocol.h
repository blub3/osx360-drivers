#ifndef XenosQuadProtocol_h
#define XenosQuadProtocol_h

enum {
  kXenosQuadSubmit = 0,
  kXenosQuadStatus = 1,
  kXenosQuadTextureCount = 2,
  kXenosQuadResultWords = 4,
};

enum {
  kXenosQuadPending = 0,
  kXenosQuadPassed = 1,
  kXenosQuadPixelsFailed = 2,
  kXenosQuadUnavailable = 3,
  kXenosQuadTimedOut = 4,
};

#endif
