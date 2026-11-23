#ifndef XenosQuadProtocol_h
#define XenosQuadProtocol_h

#include <stdint.h>

enum {
  kXenosQuadSubmit = 0,
  kXenosQuadStatus = 1,
  kXenosQuadTextureCount = 2,
  kXenosQuadResultWords = 4,
  kXenosQuadUpload = 2,
  kXenosQuadUploadStatus = 3,
  kXenosQuadUploadResultWords = 5,
  kXenosQuadUploadSide = 16,
  kXenosQuadUploadBytes = kXenosQuadUploadSide * kXenosQuadUploadSide * 3,
  kXenosQuadReadbackPixels = 32 * 32,
};

enum {
  kXenosQuadPending = 0,
  kXenosQuadPassed = 1,
  kXenosQuadPixelsFailed = 2,
  kXenosQuadUnavailable = 3,
  kXenosQuadTimedOut = 4,
};

// FNV-1a over row-major RGB bytes in R, G, B order.
static inline uint32_t xenosQuadHashByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * 16777619U;
}

#define XENOS_QUAD_HASH_INITIAL 2166136261U

#endif
