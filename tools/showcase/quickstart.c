/* The site's quickstart (docs/site, "Download"): a camera in, perception, an AI tensor and event spikes out, in C99 through
 * c_api.h only. The part between the markers is what the site shows; the rest makes it a program that runs: 90 frames of a
 * textured square approaching a textured wall at 30 frames a second. Prints what the engine perceived; exit 0 when it worked. */
#include "media/c_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { W = 640, H = 480, FRAMES = 90 };
static unsigned char luma[W * H], chroma[W * H / 2];

/* Frame f: a wall of soft waves, a square of finer texture growing from 60 to 180 samples about the centre. */
static void make_frame(unsigned f) {
    const double half = 30.0 * 5.0 / (5.0 - 3.3 * f / FRAMES);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const double u = x - W / 2.0, v = y - H / 2.0;
            const int inside = fabs(u) < half && fabs(v) < half;
            const double s = inside ? 30.0 / half : 1.0; /* The square's texture grows with it. */
            const double t = inside ? 150 + 40 * sin(u * s * 0.45) * cos(v * s * 0.38) : 100 + 35 * sin(x * 0.17) * cos(y * 0.21);
            luma[y * W + x] = (unsigned char)(t < 0 ? 0 : t > 255 ? 255 : t);
        }
    for (unsigned i = 0; i < W * H / 2; ++i) chroma[i] = 128;
}

int main(void) {
    void* block = malloc(media_engine_storage_bytes() + media_engine_storage_alignment());
    if (!block) return 1;
    unsigned char* storage = (unsigned char*)block;
    storage += (media_engine_storage_alignment() - (size_t)storage % media_engine_storage_alignment()) % media_engine_storage_alignment();
    static float tensor[3 * 224 * 224];
    static media_event_spike_t spikes[4096];
    unsigned long long events = 0, looming = 0;
    media_frame_telemetry_t t;
    media_tensor_info_t info;
    int ok = 1;
/* quickstart: begin */
media_engine_config_t config;
media_engine_config_init(&config);
config.playback = MEDIA_PLAYBACK_HOST;        /* The host drives: live sensors, any platform. */
config.modules = MEDIA_MODULE_TELEMETRY;      /* Perception: flow, TTC, objects, vibrometry. */
media_engine_t* engine = NULL;
media_engine_create(&config, storage, media_engine_storage_bytes(), &engine);

media_sensor_config_t sensor;
media_sensor_config_init(&sensor);
sensor.width = W; sensor.height = H;          /* The arena is committed and sealed here. */
media_engine_open_sensor(engine, &sensor);

media_sensor_frame_t frame = {0};
frame.struct_size = sizeof frame; frame.format = MEDIA_PIXEL_NV12;
frame.width = W; frame.height = H;
frame.planes[0] = luma; frame.planes[1] = chroma;
frame.strides[0] = frame.strides[1] = W;

for (unsigned f = 0; f < FRAMES; ++f) {
    make_frame(f);                            /* Your camera's NV12 frame. */
    frame.timestamp_100ns = (int64_t)f * 333333;
    media_engine_ingest_video_frame(engine, &frame);   /* No allocation. */
    if (f == 0) {                             /* Once a picture is held. */
        media_event_camera_config_t ec;
        media_event_camera_config_init(&ec);  /* 15 % contrast, stabilised. */
        media_engine_set_event_camera(engine, &ec);
    }
    t.struct_size = sizeof t;
    if (media_query_frame_telemetry(engine, &t) == MEDIA_OK && (t.flags & MEDIA_TELEMETRY_LOOMING))
        ++looming;                            /* t.ttc_seconds, t.hazard_x/y, t.objects[] */

    uint32_t n = 0;                           /* Neuromorphic ON/OFF spikes since the last poll. */
    while (media_poll_event_spikes(engine, spikes, 4096, &n) == MEDIA_OK && n) events += n;
}

media_tensor_request_t request;               /* 224 x 224, RGB planes, float32 in [0, 1], the fovea. */
media_tensor_request_init(&request);
request.normalize = MEDIA_TENSOR_IMAGENET;    /* Ready for a torchvision-style model. */
info.struct_size = sizeof info;
media_export_foveated_tensor(engine, &request, tensor, sizeof tensor, &info);

media_engine_close(engine);
media_engine_destroy(engine);
/* quickstart: end */
    printf("looming on %llu of %d frames, last TTC %.2f s; %llu events; tensor %llu bytes of region [%.0f, %.0f]-[%.0f, %.0f]\n", looming, FRAMES,
           t.ttc_seconds, events, (unsigned long long)info.bytes, info.left, info.top, info.right, info.bottom);
    ok = looming > 0 && events > 0 && info.bytes == sizeof tensor;
    free(block);
    return ok ? 0 : 1;
}
