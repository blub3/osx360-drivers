
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint32_t gpu[0x6600/4], ana_mem[256];
static uint8_t hdmi_mem[256];
static unsigned xbox360_video_width, xbox360_video_height, failure;
static unsigned gpu_writes;
static void mdelay(unsigned ms) { assert(ms==5); }
static uint32_t xbox360_gpu_read(unsigned reg) { return gpu[reg/4]; }
static void xbox360_gpu_write(unsigned reg, uint32_t value) { gpu[reg/4]=value; gpu_writes++; }
static int xbox360_ana_read(uint8_t reg, uint32_t *value) { *value=ana_mem[reg]; return 1; }
static int xbox360_ana_write(uint8_t reg, uint32_t value) {
  ana_mem[reg]=value;
  if(failure==1 && reg==0xd1) { failure=0; return 0; }
  return 1;
}
static int xbox360_hdmi_read(uint8_t reg, uint8_t *value) { *value=hdmi_mem[reg]; return 1; }
static int xbox360_hdmi_write(uint8_t reg, uint8_t value) {
  hdmi_mem[reg]=value;
  if(failure==2 && reg==0x14) { failure=0; return 0; }
  return 1;
}
static const char *xbox360_try_1080p(void)
{
    static const struct { uint8_t reg; uint32_t value; } ana[] = {
        { 0xd0, 0x000559d5 }, { 0xd1, 0x000259d5 },
        { 0x12, 0xf852d750 }, { 0x13, 0x60780898 },
        { 0x14, 0x0006d02d }, { 0x15, 0x52020465 }
    };
    static const struct { uint8_t reg; uint8_t value; } hdmi[] = {
        { 0x03, 0x00 }, { 0x04, 0x18 }, { 0x14, 0x03 }
    };
    static const struct { uint16_t reg; uint32_t value; } timing[] = {
        { 0x6000, 0x897 }, { 0x6004, 0x00bf083f },
        { 0x6010, 0x464 }, { 0x6014, 0x00290461 }
    };
    uint32_t old_ana[sizeof(ana) / sizeof(ana[0])], control, readback;
    uint8_t old_hdmi[sizeof(hdmi) / sizeof(hdmi[0])], hdmi_value;
    unsigned int a = 0, h = 0;
    int restored = 1;

    if (xbox360_video_width == 1920)
        return "inherited 1080p";
    if (xbox360_gpu_read(0x6134) != 1280 || xbox360_gpu_read(0x6138) != 720 ||
        xbox360_gpu_read(0x6120) != 1280 || xbox360_gpu_read(0x6104) != 2 ||
        (xbox360_gpu_read(0x6000) & 0x1fff) != 1649 ||
        (xbox360_gpu_read(0x6010) & 0x1fff) != 749 ||
        (xbox360_gpu_read(0x6528) & 1))
        return "skipped: inherited scanout is not the tested 720p mode";

    if (!xbox360_ana_read(0, &control) || control != 0x2c1)
        return "skipped: HDMI transmitter not in the expected active state";
    for (unsigned int i = 0; i < sizeof(ana) / sizeof(ana[0]); i++)
        if (!xbox360_ana_read(ana[i].reg, &old_ana[i]))
            return "skipped: could not save ANA settings";
    for (unsigned int i = 0; i < sizeof(hdmi) / sizeof(hdmi[0]); i++)
        if (!xbox360_hdmi_read(hdmi[i].reg, &old_hdmi[i]))
            return "skipped: could not save HDMI settings";

    /* Disable the transmitter before changing its PLL and digital timing. */
    if (!xbox360_ana_write(0, 0))
        goto restore;
    while (a < sizeof(ana) / sizeof(ana[0])) {
        unsigned int i = a++;
        if (!xbox360_ana_write(ana[i].reg, ana[i].value) ||
            !xbox360_ana_read(ana[i].reg, &readback) || readback != ana[i].value)
            goto restore;
    }
    mdelay(5);
    while (h < sizeof(hdmi) / sizeof(hdmi[0])) {
        unsigned int i = h++;
        if (!xbox360_hdmi_write(hdmi[i].reg, hdmi[i].value) ||
            !xbox360_hdmi_read(hdmi[i].reg, &hdmi_value) || hdmi_value != hdmi[i].value)
            goto restore;
    }
    if (!xbox360_ana_write(0, control))
        goto restore;

    /* Preserve the inherited pixel format and framebuffer addresses. */
    xbox360_gpu_write(0x60e8, 1);  /* CRTC update lock */
    for (unsigned int i = 0; i < sizeof(timing) / sizeof(timing[0]); i++)
        xbox360_gpu_write(timing[i].reg, timing[i].value);
    xbox360_gpu_write(0x60e8, 0);

    xbox360_gpu_write(0x6144, 1);  /* Graphics surface update lock */
    xbox360_gpu_write(0x6120, 1920);
    xbox360_gpu_write(0x6134, 1920);
    xbox360_gpu_write(0x6138, 1080);
    xbox360_gpu_write(0x6144, 0);

    xbox360_gpu_write(0x65cc, 1);  /* Scaler update lock */
    xbox360_gpu_write(0x2844, 1920);
    xbox360_gpu_write(0x652c, 1080);
    xbox360_gpu_write(0x6580, 0);
    xbox360_gpu_write(0x6584, (1920u << 16) | 1080u);
    xbox360_gpu_write(0x6590, 0);  /* Bypass scaling at native resolution */
    xbox360_gpu_write(0x65e8, 1920 / 4 - 1);
    xbox360_gpu_write(0x65cc, 0);

    xbox360_video_width = 1920;
    xbox360_video_height = 1080;
    return "programmed experimental 1080p60";

restore:
    while (h) {
        --h;
        restored &= xbox360_hdmi_write(hdmi[h].reg, old_hdmi[h]);
    }
    while (a) {
        --a;
        restored &= xbox360_ana_write(ana[a].reg, old_ana[a]);
    }
    restored &= xbox360_ana_write(0, control);
    return restored ? "720p retained after SMC failure" :
        "SMC failure and incomplete transmitter rollback";
}


static void prepare(void) {
  xbox360_video_width=1280;
  xbox360_video_height=720;
  gpu[0x6134/4]=1280; gpu[0x6138/4]=720;
  gpu[0x6120/4]=1280; gpu[0x6104/4]=2;
  gpu[0x6000/4]=1649; gpu[0x6010/4]=749;
  gpu_writes=0;
  ana_mem[0]=0x2c1; ana_mem[0xd0]=0xd9b18; ana_mem[0xd1]=0x69b18;
  hdmi_mem[3]=0x80; hdmi_mem[4]=0x2d; hdmi_mem[0x14]=0x07;
}
int main(void) {
  prepare();
  for(unsigned f=1;f<=2;f++) {
    failure=f;
    assert(strstr(xbox360_try_1080p(),"720p retained"));
    assert(gpu_writes==0 && xbox360_video_width==1280);
    assert(ana_mem[0]==0x2c1 && ana_mem[0xd0]==0xd9b18 && ana_mem[0xd1]==0x69b18);
    assert(hdmi_mem[3]==0x80 && hdmi_mem[4]==0x2d && hdmi_mem[0x14]==0x07);
  }
  gpu[0x6120/4]=640;
  assert(strstr(xbox360_try_1080p(),"skipped") && gpu_writes==0);
  prepare();
  assert(strcmp(xbox360_try_1080p(),"programmed experimental 1080p60")==0);
  assert(xbox360_video_width==1920 && xbox360_video_height==1080);
  assert(gpu[0x6000/4]==2199 && gpu[0x6010/4]==1124 && gpu[0x6590/4]==0);
  assert(ana_mem[0]==0x2c1 && ana_mem[0xd0]==0x559d5 && hdmi_mem[0x14]==3);
  puts("Mocked 1080p transition, 720p guard, and ANA/HDMI rollback passed.");
}
