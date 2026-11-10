/* The site's visual showcase, made by the engine itself (docs/site, "What it sees"). A C99 host of the SDK in host mode with
 * a live camera: it pushes prepared RGBA frames (tools/showcase/make_showcase.py writes them, with the scene's truth) through
 * media_engine_ingest_video_frame and keeps what the engine gives back: forensic snapshots (media_engine_save_snapshot: the
 * GPU image or the view, BMP + JSON sidecar), the telemetry of every picture, and the event camera's spikes. Nothing here
 * draws or measures; the Python side only lays out what this wrote.
 *   media_showcase <frames.rgba> <out prefix> <width> <height> <fps> <frames> [options]
 *   --gpu W H          MEDIA_SENSOR_GPU, the GPU image W x H (snapshots are then the GPU image)
 *   --lift X --gamma G --recon bilinear|edge|neural   picture controls (GPU only)
 *   --auto-level  --stabilize  --events stab|raw  --track F (MEDIA_TARGET_NEXT at frame F)  --imu file (float yaw pitch roll a frame)
 *   --snap F           a snapshot after frame F (repeatable): <prefix>_f<F>.bmp / .json
 * Writes <prefix>_telemetry.jsonl (a line a frame) and, with --events, <prefix>_events.bin (per frame: u32 frame, u32 count,
 * count x 16-byte media_event_spike_t). Relative paths: the SDK takes UTF-8 and argv is the ANSI code page on Windows. */
#include "media/c_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_SNAPS 16
static media_event_spike_t g_spikes[8192];

static void sleep_briefly(void) {
#ifdef _WIN32
    SwitchToThread();
#endif
}
static unsigned long long ticks_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: media_showcase frames.rgba prefix width height fps frames [options]\n"); return 2; }
    const char* input = argv[1]; const char* prefix = argv[2];
    const unsigned w = (unsigned)atoi(argv[3]), h = (unsigned)atoi(argv[4]), frames = (unsigned)atoi(argv[6]);
    const double fps = atof(argv[5]);
    unsigned gpu_w = 0, gpu_h = 0, snaps[MAX_SNAPS], snap_count = 0, track_at = 0xffffffffu, recon = MEDIA_RECONSTRUCT_EDGE;
    int auto_level = 0, stabilize = 0, events = 0, raw_events = 0, controls = 0;
    float lift = 0, gamma = 1;
    const char* imu_path = NULL;
    for (int i = 7; i < argc; ++i) {
        if (!strcmp(argv[i], "--gpu") && i + 2 < argc) { gpu_w = (unsigned)atoi(argv[++i]); gpu_h = (unsigned)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--lift") && i + 1 < argc) { lift = (float)atof(argv[++i]); controls = 1; }
        else if (!strcmp(argv[i], "--gamma") && i + 1 < argc) { gamma = (float)atof(argv[++i]); controls = 1; }
        else if (!strcmp(argv[i], "--recon") && i + 1 < argc) {
            ++i; controls = 1;
            recon = !strcmp(argv[i], "neural") ? MEDIA_RECONSTRUCT_NEURAL : !strcmp(argv[i], "bilinear") ? MEDIA_RECONSTRUCT_BILINEAR : MEDIA_RECONSTRUCT_EDGE;
        }
        else if (!strcmp(argv[i], "--auto-level")) auto_level = 1;
        else if (!strcmp(argv[i], "--stabilize")) stabilize = 1;
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) { events = 1; raw_events = !strcmp(argv[++i], "raw"); }
        else if (!strcmp(argv[i], "--track") && i + 1 < argc) track_at = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--imu") && i + 1 < argc) imu_path = argv[++i];
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc && snap_count < MAX_SNAPS) snaps[snap_count++] = (unsigned)atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    FILE* in = fopen(input, "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", input); return 1; }
    FILE* imu = imu_path ? fopen(imu_path, "rb") : NULL;
    if (imu_path && !imu) { fprintf(stderr, "cannot open %s\n", imu_path); return 1; }
    const size_t frame_bytes = (size_t)w * h * 4;
    unsigned char* pixels = (unsigned char*)malloc(frame_bytes);
    void* storage_block = malloc(media_engine_storage_bytes() + media_engine_storage_alignment());
    if (!pixels || !storage_block) return 1;
    unsigned char* storage = (unsigned char*)storage_block;
    storage += (media_engine_storage_alignment() - (size_t)storage % media_engine_storage_alignment()) % media_engine_storage_alignment();

    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* engine = NULL;
    if (media_engine_create(&config, storage, media_engine_storage_bytes(), &engine) != MEDIA_OK) { fprintf(stderr, "create failed\n"); return 1; }
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = w; sensor.height = h;
    if (gpu_w) { sensor.options = MEDIA_SENSOR_GPU; sensor.output_width = gpu_w; sensor.output_height = gpu_h; }
    media_status status = media_engine_open_sensor(engine, &sensor);
    if (status != MEDIA_OK) { fprintf(stderr, "open_sensor: %d\n", (int)status); return 1; }
    if (controls) {
        media_picture_controls_t c; media_picture_controls_init(&c);
        c.shadow_lift = lift; c.gamma = gamma; c.reconstruction = recon;
        if (media_engine_set_picture_controls(engine, &c) != MEDIA_OK) { fprintf(stderr, "picture controls refused\n"); return 1; }
    }
    if (stabilize && media_engine_set_stabilization(engine, 1) != MEDIA_OK) { fprintf(stderr, "stabilisation refused\n"); return 1; }

    char path[512];
    snprintf(path, sizeof path, "%s_telemetry.jsonl", prefix);
    FILE* tel = fopen(path, "wb");
    FILE* ev = NULL;
    if (events) { snprintf(path, sizeof path, "%s_events.bin", prefix); ev = fopen(path, "wb"); }
    if (!tel || (events && !ev)) { fprintf(stderr, "cannot write beside %s\n", prefix); return 1; }

    media_sensor_frame_t frame; memset(&frame, 0, sizeof frame);
    frame.struct_size = sizeof frame; frame.format = MEDIA_PIXEL_RGBA8; frame.width = w; frame.height = h;
    frame.planes[0] = pixels; frame.strides[0] = w * 4;
    unsigned long long total_events = 0;
    for (unsigned f = 0; f < frames; ++f) {
        if (fread(pixels, 1, frame_bytes, in) != frame_bytes) { fprintf(stderr, "short input at frame %u\n", f); return 1; }
        if (imu) {
            float d[3];
            if (fread(d, sizeof(float), 3, imu) != 3) { fprintf(stderr, "short IMU at frame %u\n", f); return 1; }
            frame.flags = MEDIA_SENSOR_IMU; frame.imu_yaw = d[0]; frame.imu_pitch = d[1]; frame.imu_roll = d[2];
        }
        frame.timestamp_100ns = 10000000 + (int64_t)(f * 1e7 / fps + 0.5);
        if (f == 1) {
            /* Once a picture is held: these need one. */
            if (auto_level && media_engine_set_auto_level(engine, 1) != MEDIA_OK) { fprintf(stderr, "auto-level refused\n"); return 1; }
            if (events) {
                media_event_camera_config_t ec; media_event_camera_config_init(&ec);
                if (raw_events) ec.flags = MEDIA_EVENTS_UNSTABILIZED;
                if (media_engine_set_event_camera(engine, &ec) != MEDIA_OK) { fprintf(stderr, "event camera refused\n"); return 1; }
            }
        }
        if (f == track_at && media_engine_track_object(engine, MEDIA_TARGET_NEXT) != MEDIA_OK) fprintf(stderr, "track refused at frame %u\n", f);
        if ((status = media_engine_ingest_video_frame(engine, &frame)) != MEDIA_OK) { fprintf(stderr, "ingest %u: %d\n", f, (int)status); return 1; }

        media_gpu_image_t image; memset(&image, 0, sizeof image); image.struct_size = sizeof image;
        if (gpu_w) {
            /* The showcase's own pacing: wait for this frame's image (a real host polls and moves on). */
            const unsigned long long start = ticks_ms();
            while (!(media_query_gpu_image(engine, &image) == MEDIA_OK && image.sequence >= f + 1) && ticks_ms() - start < 3000) sleep_briefly();
        }
        uint32_t n = 0, frame_events = 0;
        while (events && media_poll_event_spikes(engine, g_spikes, 8192, &n) == MEDIA_OK && n) {
            const uint32_t head[2] = {f, n};
            fwrite(head, sizeof head, 1, ev); fwrite(g_spikes, sizeof(media_event_spike_t), n, ev);
            frame_events += n;
        }
        total_events += frame_events;

        media_frame_telemetry_t t; memset(&t, 0, sizeof t); t.struct_size = sizeof t;
        const int have = media_query_frame_telemetry(engine, &t) == MEDIA_OK;
        fprintf(tel, "{\"f\":%u,\"events\":%u", f, frame_events);
        if (have) {
            fprintf(tel, ",\"flags\":%u,\"motion\":[%.5f,%.5f],\"ttc\":%.4f,\"divergence\":%.4f,\"hazard\":[%.4f,%.4f],\"horizon_deg\":%.3f,\"level_deg\":%.3f",
                    t.flags, t.motion_x, t.motion_y, t.ttc_seconds, t.divergence, t.hazard_x, t.hazard_y, t.estimated_horizon_deg, t.level_degrees);
            fprintf(tel, ",\"view\":[%.4f,%.4f,%.4f,%.5f],\"ego\":[%.4f,%.4f,%.6f,%u,%u],\"active\":[%u,%u,%u,%u]", t.view_zoom, t.view_x, t.view_y, t.view_roll,
                    t.ego_dx, t.ego_dy, t.ego_turn, t.ego_tiles, t.ego_inliers, t.active_left, t.active_top, t.active_right, t.active_bottom);
            fprintf(tel, ",\"vib\":[%.4f,%.3f,%.4f,%u],\"vital\":[%.3f,%.3f,%u],\"vib_seconds\":%.3f,\"vib_region\":[%.1f,%.1f,%.1f,%.1f],\"target\":%u,\"objects\":[",
                    t.vibration_hz, t.vibrometry_confidence, t.vibration_amplitude, t.vibrometry_channel, t.vital_bpm, t.vital_confidence, t.vital_channel,
                    t.vibrometry_seconds, t.vibrometry_left, t.vibrometry_top, t.vibrometry_right, t.vibrometry_bottom, t.target_track_id);
            for (uint32_t o = 0; o < t.object_count && o < MEDIA_MAX_OBJECTS; ++o) {
                const media_detected_object_t* d = &t.objects[o];
                fprintf(tel, "%s{\"id\":%u,\"flags\":%u,\"box\":[%.4f,%.4f,%.4f,%.4f],\"v\":[%.4f,%.4f],\"ttc\":%.3f,\"saliency\":%.3f,\"age\":%u}", o ? "," : "",
                        d->track_id, d->flags, d->x, d->y, d->width, d->height, d->velocity_x, d->velocity_y, d->ttc_seconds, d->saliency, d->age);
            }
            fputs("]", tel);
        }
        if (gpu_w) fprintf(tel, ",\"gpu\":{\"seq\":%u,\"flags\":%u,\"ms\":%.3f,\"region\":[%u,%u,%u,%u],\"roll\":%.5f}", image.sequence, image.flags, image.gpu_ms,
                           image.left, image.top, image.right, image.bottom, image.roll);
        fputs("}\n", tel);

        for (unsigned s = 0; s < snap_count; ++s)
            if (snaps[s] == f) {
                snprintf(path, sizeof path, "%s_f%04u.bmp", prefix, f);
                status = media_engine_save_snapshot(engine, path);
                if (status != MEDIA_OK) { fprintf(stderr, "snapshot %u: %d\n", f, (int)status); return 1; }
            }
    }
    media_event_camera_state_t es; memset(&es, 0, sizeof es); es.struct_size = sizeof es;
    if (events) (void)media_query_event_camera(engine, &es);
    printf("%s: %u frames, %llu events (dropped %llu, sensor %ux%u)\n", prefix, frames, total_events, (unsigned long long)es.dropped, es.width, es.height);
    fclose(tel); if (ev) fclose(ev); fclose(in); if (imu) fclose(imu);
    media_engine_close(engine);
    media_engine_destroy(engine);
    free(pixels); free(storage_block);
    return 0;
}
