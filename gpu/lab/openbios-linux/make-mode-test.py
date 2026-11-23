from pathlib import Path
import sys

root = Path(sys.argv[1])
p = root / "arch/ppc/xbox360/init.c"
s = p.read_text()
a=s.index("    volatile unsigned int *send_status", s.index("xbox360_hdmi_audio(void)"))
b=s.index('    printk("HDMI audio: transmitter setup complete', a)
s=s[:a]+'''    for (unsigned int i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        uint8_t value = setup[i].value;
        if (xbox360_video_width == 1920 && setup[i].reg == 0x14)
            value = 0x03; /* Captured 1080p HDMI value. */
        if (!xbox360_hdmi_write(setup[i].reg, value)) {
            printk("HDMI audio: SMC write failed at register %02x\\n", setup[i].reg);
            return;
        }
    }
'''+s[b:]
anchor='/*\n * Configure HDMI stereo PCM'
shared='''/* SMC I2C replies are synchronous here, before the OS owns the mailbox. */
static int
xbox360_smc_i2c(xbox360_smc_msg *msg)
{
    volatile unsigned int *send_status = (volatile unsigned int *)(XBOX360_SMC_BASE + 0x84);
    volatile unsigned int *recv_status = (volatile unsigned int *)(XBOX360_SMC_BASE + 0x94);
    volatile unsigned int *recv_data = (volatile unsigned int *)(XBOX360_SMC_BASE + 0x90);
    unsigned int ms;

    for (ms = 0; ms < 100 && !(in_le32(send_status) & 4); ms++)
        mdelay(1);
    if (ms == 100)
        return 0;
    xbox360_smc_write_ready(msg);

    for (ms = 0; ms < 100; ms++) {
        if (in_le32(recv_status) & 4) {
            out_le32(recv_status, 4);
            for (unsigned int word = 0; word < 4; word++)
                msg->data32[word] = in_be32(recv_data);
            out_le32(recv_status, 0);
            if (msg->data[0] == 0x11)
                return msg->data[1] == 0;
        }
        mdelay(1);
    }
    return 0;
}

static int
xbox360_hdmi_write(uint8_t reg, uint8_t value)
{
    xbox360_smc_msg msg = { .data = { 0x11, 0x20, 0, 0xbd, 0, 0, reg, value } };
    return xbox360_smc_i2c(&msg);
}

static int
xbox360_hdmi_read(uint8_t reg, uint8_t *value)
{
    xbox360_smc_msg msg = { .data = { 0x11, 0x10, 1, 0xbd, 0, 0xbd, reg } };
    if (!xbox360_smc_i2c(&msg))
        return 0;
    *value = msg.data[3];
    return 1;
}

static int
xbox360_ana_read(uint8_t reg, uint32_t *value)
{
    xbox360_smc_msg msg = { .data = { 0x11, 0x10, 5, 0xf0, 0, 0xf0, reg } };
    if (!xbox360_smc_i2c(&msg))
        return 0;
    *value = (uint32_t)msg.data[4] | ((uint32_t)msg.data[5] << 8) |
        ((uint32_t)msg.data[6] << 16) | ((uint32_t)msg.data[7] << 24);
    return 1;
}

static int
xbox360_ana_write(uint8_t reg, uint32_t value)
{
    xbox360_smc_msg msg = { .data = { 0x11, 0x60, 0, 0xf0, 0, 0, reg, 0,
                                     value & 0xff, (value >> 8) & 0xff,
                                     (value >> 16) & 0xff, value >> 24 } };
    return xbox360_smc_i2c(&msg);
}

'''
assert s.count(anchor)==1
s=s.replace(anchor, shared+anchor)
anchor='static void\nxbox360_boot_is_1080p(void)'
mode='''static void
xbox360_gpu_write(unsigned int reg, uint32_t value)
{
    out_be32((volatile uint32_t *)(XBOX360_GPU_BASE + reg), value);
}

/*
 * Experimental 1080p60 transition from the Slim's known 720p boot mode.
 * The clock/timing values came from paired retail v3 captures. Keep the
 * remaining transmitter state and use a native surface without scaling.
 */
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

'''
assert s.count(anchor)==1
s=s.replace(anchor,mode+anchor)
anchor='    setup_timers();'
assert s.count(anchor)==1
s=s.replace(anchor,anchor+'\n    const char *mode_test = xbox360_try_1080p();')
anchor='    push_str("xbox360-inherited-scanout");\n    fword("property");'
assert s.count(anchor)==1
s=s.replace(anchor,anchor+'''

    push_str(mode_test);
    fword("encode-string");
    push_str("xbox360-mode-test");
    fword("property");
''')
p.write_text(s)
p=root/"arch/ppc/xbox360/console.c"
s=p.read_text()
a='''\tif (!init) {
\t\tscreen_width = xbox360_video_width;'''
b='''\tif (!init || screen_width != xbox360_video_width) {
\t\tcurrent_x = 10;
\t\tcurrent_y = 10;
\t\tscreen_width = xbox360_video_width;'''
assert s.count(a)==1
p.write_text(s.replace(a,b))
