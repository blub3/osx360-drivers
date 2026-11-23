#ifndef XenosColorTriangleShaders_hpp
#define XenosColorTriangleShaders_hpp

// Xenos microcode adapted from XSDK's tools/gen_triangle_shaders.py
// at 9e6de98f6b5d737bce0fecd2864edae94bdd2637.
// Vertex fetch constant 60 reads interleaved float4 position and RGBA,
// with an eight-dword stride and color at dword four. The VS exports
// position and interpolator 0. The PS writes interpolator 0 to target 0.

// MIT License
//
// Copyright (c) 2026 Mena
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

static const UInt32 kColorTriangleVertexShader[] = {
  0x30052003, 0x00001200, 0xc2000000,
  0x00001005, 0x00001200, 0xc4000000,
  0x00001006, 0x00002200, 0x00000000,
  0x01481000, 0x00262688, 0x00000008,
  0x01482000, 0x00262688, 0x00000408,
  0xc80f803e, 0x00000000, 0xc2010100,
  0xc80f8000, 0x00000000, 0xc2020200,
};

static const UInt32 kColorTrianglePixelShader[] = {
  0x00000000, 0x1001c400, 0x22000000,
  0xc80f8000, 0x00000000, 0xc2000000,
};

#endif
