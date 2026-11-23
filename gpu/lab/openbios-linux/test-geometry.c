
#include <assert.h>
#include <stdio.h>
static unsigned xbox360_video_width, xbox360_video_height;
static char xbox360_scanout_info[120];
static unsigned regs[0x6600/4];
static unsigned xbox360_gpu_read(unsigned reg) { return regs[reg/4]; }
static void xbox360_read_video_geometry(void)
{
    unsigned int width = xbox360_gpu_read(0x6134);  /* D1GRPH_X_END */
    unsigned int height = xbox360_gpu_read(0x6138); /* D1GRPH_Y_END */
    unsigned int pitch = xbox360_gpu_read(0x6120);  /* D1GRPH_PITCH, in pixels */
    unsigned int htotal = xbox360_gpu_read(0x6000) & 0x1fff;
    unsigned int vtotal = xbox360_gpu_read(0x6020) & 0x1fff;
    unsigned int sync_b = xbox360_gpu_read(0x6010) & 0x1fff;
    unsigned int format = xbox360_gpu_read(0x6528); /* D1MODE_DATA_FORMAT */
    unsigned int control = xbox360_gpu_read(0x6104); /* D1GRPH_CONTROL */

    /*
     * Inherit a matching 1080p50/60 scanout. A scaled 720p surface does not
     * qualify, even when the sink is receiving a 1080p signal.
     */
    if (width == 1920 && height == 1080 && pitch == width &&
        (htotal == 2199 || htotal == 2639) &&
        (vtotal == 1124 || sync_b == 1124) && !(format & 1) &&
        control == 2 && xbox360_gpu_read(0x6124) == 0 &&
        xbox360_gpu_read(0x6128) == 0 && xbox360_gpu_read(0x612c) == 0 &&
        xbox360_gpu_read(0x6130) == 0) {
        xbox360_video_width = width;
        xbox360_video_height = height;
    }

    snprintf(xbox360_scanout_info, sizeof(xbox360_scanout_info),
             "%ux%u pitch=%u htotal=%u vtotal=%u sync-b=%u format=%x control=%x",
             width, height, pitch, htotal, vtotal, sync_b, format, control);
}


static void check(unsigned expected) {
    xbox360_video_width=1280;
    xbox360_video_height=720;
    xbox360_read_video_geometry();
    assert(xbox360_video_width==expected);
    assert(xbox360_video_height==(expected==1920 ? 1080 : 720));
}
int main(void) {
    regs[0x6134/4]=1920;
    regs[0x6138/4]=1080;
    regs[0x6120/4]=1920;
    regs[0x6000/4]=2199;
    regs[0x6020/4]=1124;
    regs[0x6104/4]=2;
    check(1920);
    regs[0x6000/4]=2639;
    regs[0x6020/4]=0;
    regs[0x6010/4]=1124;
    check(1920);
    regs[0x6528/4]=1;
    check(1280);
    regs[0x6528/4]=0;
    regs[0x6120/4]=2048;
    check(1280);
    regs[0x6120/4]=1920;
    regs[0x6128/4]=8;
    check(1280);
    regs[0x6128/4]=0;
    regs[0x6104/4]=1;
    check(1280);
    regs[0x6104/4]=2;
    regs[0x6134/4]=1280;
    regs[0x6138/4]=720;
    regs[0x6120/4]=1280;
    check(1280);
    assert(1920*1088*4 <= 0x800000);
    puts("Geometry tests passed: 1080p50/60, interlaced, padded pitch, offset, format, and scaled 720p.");
}
