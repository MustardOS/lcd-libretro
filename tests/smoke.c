/* Headless Libretro frontend exercising the public core API: contentless and
   manifest diagnostics, the self-test ROM set, rejection of bad content,
   video/audio/input, pixel formats, options, reset and save states. */
#include "libretro.h"
#include "test_support.h"
#include <stdint.h>

static unsigned input_mask;
static unsigned video_count, dupe_count;
static size_t audio_frames, audio_calls;
static unsigned last_w, last_h;
static size_t last_pitch;
static uint32_t video_hash;
static bool rgb565_only, allow_dupe;
static int16_t audio_peak;
static unsigned av_changes;
static const char *option_overrides[2][2];

static uint32_t checksum(const void *v, size_t n)
{
    const uint8_t *p = v;
    uint32_t h = UINT32_C(2166136261);
    while (n--)
        h = (h ^ *p++) * UINT32_C(16777619);
    return h;
}

static bool env_cb(unsigned cmd, void *data)
{
    unsigned i;
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return !rgb565_only || *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = allow_dupe;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (i = 0; i < 2; ++i)
            if (option_overrides[i][0] && !strcmp(v->key, option_overrides[i][0])) {
                v->value = option_overrides[i][1];
                return true;
            }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = option_overrides[0][0] != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av_changes++;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    last_w = width;
    last_h = height;
    last_pitch = pitch;
    if (!data) {
        dupe_count++;
        return;
    }
    video_count++;
    video_hash = checksum(data, pitch * height);
}

static size_t audio_cb(const int16_t *data, size_t frames)
{
    size_t i;
    audio_calls++;
    audio_frames += frames;
    for (i = 0; i < frames * 2u; ++i)
        if (data[i] > audio_peak)
            audio_peak = data[i];
    return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port || device != RETRO_DEVICE_JOYPAD || index)
        return 0;
    return (input_mask >> id & 1u) ? 1 : 0;
}

static void run_frames(unsigned n)
{
    while (n--)
        retro_run();
}

static unsigned char *selftest;
static size_t selftest_size;

static bool load_selftest(void)
{
    struct retro_game_info info = {"lcdlab_selftest.zip", selftest, selftest_size, NULL};
    return retro_load_game(&info);
}

static void test_contentless_and_manifest(void)
{
    const char manifest[] = "LCD-LAB/1\ngame=catch\n";
    struct retro_game_info ok = {"Catch-and-Carry.lcd", manifest, sizeof(manifest) - 1u, NULL};
    struct retro_system_av_info av;
    uint32_t before;

    CHECK(retro_load_game(NULL));
    retro_get_system_av_info(&av);
    CHECK_EQ(av.geometry.base_width, 320);
    CHECK_EQ(av.timing.sample_rate, 48000);
    run_frames(2);
    CHECK_EQ(audio_frames, 1600);
    retro_unload_game();

    CHECK(retro_load_game(&ok));
    run_frames(2);
    before = video_hash;
    input_mask = 1u << RETRO_DEVICE_ID_JOYPAD_START;
    retro_run();
    input_mask = 0;
    run_frames(5);
    CHECK(video_hash != before);
    retro_unload_game();
}

static void test_rejection(void)
{
    const char junk[] = "MAME ROM content";
    struct retro_game_info bad = {"gnw_ball.zip", junk, sizeof(junk), NULL};
    struct retro_game_info missing = {"/nonexistent/gnw_ball.zip", NULL, 0, NULL};
    CHECK(!retro_load_game(&bad));
    CHECK(!retro_load_game(&missing));
    CHECK_EQ(retro_serialize_size(), 0);
    retro_run();
}

static void test_machine(void)
{
    struct retro_system_av_info av;
    size_t size;
    void *state;
    uint32_t after, frames_before;
    unsigned i;

    video_count = dupe_count = 0;
    audio_frames = audio_calls = 0;
    audio_peak = 0;
    CHECK(load_selftest());
    retro_get_system_av_info(&av);
    CHECK_EQ(av.geometry.base_width, 540);
    CHECK_EQ(av.geometry.base_height, 480);
    CHECK_EQ(av.timing.sample_rate, 32768);
    CHECK_EQ(av.timing.fps, 60);
    CHECK(retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 128 && retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));

    /* one second: exactly 32768 audio frames, one video callback per frame */
    run_frames(60);
    CHECK_EQ(audio_frames, 32768);
    CHECK_EQ(video_count + dupe_count, 60);
    CHECK_EQ(last_w, 540);
    CHECK_EQ(last_pitch, 540u * 4u);
    CHECK_EQ(audio_peak, 0);

    /* holding BA (Right) makes a tone */
    input_mask = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    run_frames(10);
    input_mask = 0;
    CHECK(audio_peak > 4000);

    /* deterministic save states */
    size = retro_serialize_size();
    CHECK(size > 1000 && size < 4096);
    state = malloc(size);
    CHECK(retro_serialize(state, size));
    for (i = 0; i < 90; ++i) {
        input_mask = i % 9 < 3 ? 1u << RETRO_DEVICE_ID_JOYPAD_START : 0;
        retro_run();
    }
    after = video_hash;
    frames_before = video_count;
    CHECK(retro_unserialize(state, size));
    for (i = 0; i < 90; ++i) {
        input_mask = i % 9 < 3 ? 1u << RETRO_DEVICE_ID_JOYPAD_START : 0;
        retro_run();
    }
    input_mask = 0;
    CHECK_EQ(video_hash, after);
    CHECK(video_count > frames_before);
    CHECK(!retro_unserialize(state, size - 1));
    ((uint8_t *)state)[0] ^= 0xff;
    CHECK(!retro_unserialize(state, size));
    ((uint8_t *)state)[0] ^= 0xff;
    ((uint8_t *)state)[12] ^= 0xff; /* content id */
    CHECK(!retro_unserialize(state, size));
    free(state);

    /* reset is the ACL soft reset: CPU restarts, RAM is kept */
    retro_reset();
    run_frames(2);

    /* unchanged LCD frames are duplicated when the frontend allows it */
    retro_unload_game();
    allow_dupe = true;
    dupe_count = 0;
    CHECK(load_selftest());
    video_count = 0;
    run_frames(120);
    CHECK(dupe_count > 90);
    CHECK(video_count >= 4); /* a real frame at least every 30 frames */
    allow_dupe = false;

    /* option change rebuilds the display and reports the new geometry */
    option_overrides[0][0] = "lcd_lab_resolution";
    option_overrides[0][1] = "320x240";
    av_changes = 0;
    retro_run();
    option_overrides[0][0] = NULL;
    CHECK_EQ(av_changes, 1);
    CHECK_EQ(last_w, 270);
    CHECK_EQ(last_h, 240);
    retro_unload_game();

    /* RGB565 frontends */
    rgb565_only = true;
    CHECK(load_selftest());
    run_frames(3);
    CHECK_EQ(last_pitch, (size_t)last_w * 2u);
    rgb565_only = false;
    retro_unload_game();
}

static void test_background(void)
{
    size_t size = 0;
    unsigned char *zip = test_read_file("tests/fixtures/selftest_background.zip", &size);
    struct retro_game_info info = {"selftest_background.zip", NULL, 0, NULL};
    uint32_t with_bg, without_bg;
    CHECK(zip != NULL);
    if (!zip)
        return;
    info.data = zip;
    info.size = size;
    CHECK(retro_load_game(&info));
    run_frames(3);
    CHECK_EQ(last_w, 540);
    with_bg = video_hash;

    option_overrides[0][0] = "lcd_lab_background";
    option_overrides[0][1] = "Disabled";
    retro_run();
    retro_run();
    option_overrides[0][0] = NULL;
    without_bg = video_hash;
    CHECK(with_bg != without_bg);
    CHECK_EQ(last_w, 540);
    retro_unload_game();
    free(zip);
}

int main(void)
{
    struct retro_system_info sys;
    selftest = test_read_file("roms/lcdlab_selftest.zip", &selftest_size);
    CHECK(selftest != NULL);
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample_batch(audio_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    CHECK_EQ(retro_api_version(), RETRO_API_VERSION);
    retro_init();
    retro_get_system_info(&sys);
    CHECK(!strcmp(sys.valid_extensions, "zip|gnw|lcd") && sys.block_extract);
    test_contentless_and_manifest();
    test_rejection();
    if (selftest)
        test_machine();
    test_background();
    retro_deinit();
    free(selftest);
    return test_finish("smoke (libretro API: content, rejection, AV, input, options, reset, states)");
}
