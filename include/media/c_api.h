/* The engine's C99 SDK (docs/sdk.md). Pure C: every type is a plain struct, enum or opaque handle; no C++ type, no
 * exception and no allocation crosses this boundary. The SDK never allocates on the heap: the host gives the engine its
 * storage (media_engine_storage_bytes), and everything else comes from the engine's arenas, sized by
 * media_query_arena_budget before anything is created.
 *
 * Two ways to play (media_engine_config_t.playback):
 * - MEDIA_PLAYBACK_WINDOW (Windows): the engine's own window, GPU decoding and presentation, the audio endpoint.
 *   Lifecycle: media_engine_create -> media_engine_open -> media_engine_run (blocks; the window's thread)
 *              -> media_engine_close -> (open again ...) -> media_engine_destroy.
 *   Threads: create, open, run, close and destroy on one host thread (the one that runs). media_engine_command,
 *   media_engine_hear and media_engine_set_talk run on that thread too, from the frame callback or before run returns
 *   (they act at once).
 * - MEDIA_PLAYBACK_HOST (every platform: Windows, Linux, Android, Apple): the host drives. It calls media_engine_step
 *   on its thread (demux, decode, the clock, presentation through on_present, commands) and media_engine_pump_audio from
 *   its audio callback (the DSP, speed, volume; the clock follows what it pumped), and feeds its microphone with
 *   media_engine_feed_voice. The engine starts no thread. Video is decoded by the host's decoder (decode_video) or, for the
 *   streams it covers, by the engine's software bridge; audio is uncompressed PCM tracks, or the host's decoder (decode_audio).
 *   API 14, Windows: with MEDIA_OUTPUT_TEXTURE, H.264 and HEVC (8- and 10-bit) are decoded on the GPU by the engine's own
 *   decoder and drawn into texture output's shared textures (media_query_video_decoder says which decoder plays).
 *   Lifecycle: create -> open -> step / pump_audio ... until step says MEDIA_ERROR_EMPTY (the end) -> close -> destroy.
 *   Threads: create, open, step, command, hear, feed_voice, set_talk, close and destroy on one host thread;
 *   pump_audio on one thread (the same or the audio thread).
 * - Live sensors (MEDIA_PLAYBACK_HOST, API 3): a camera and a microphone array instead of a file, for robots, drones, vehicles
 *   and smart cameras. Lifecycle: create -> media_engine_open_sensor -> media_engine_ingest_video_frame (each frame is shown
 *   at once: tracking, perception, the audio saccade, on_present, on_frame) and media_engine_ingest_audio_samples (the sound
 *   locator; pump_audio plays them through the DSP) ... -> close -> destroy. Threads: ingest_video_frame, step, command and
 *   the queries on the host thread; ingest_audio_samples on one thread (the same or the capture thread).
 * media_engine_poll_frame may be called from one other thread (a single consumer) at any time between open and close. */
#ifndef MEDIA_C_API_H
#define MEDIA_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_API_VERSION 17u

typedef enum media_status {
    MEDIA_OK = 0,
    MEDIA_ERROR_INVALID_ARGUMENT = 1, /* A null pointer, a wrong struct size, a bad value. */
    MEDIA_ERROR_STORAGE = 2,          /* The storage given is too small or misaligned. */
    MEDIA_ERROR_STATE = 3,            /* Not now: open while open, run before open, ... */
    MEDIA_ERROR_UNSUPPORTED = 4,      /* Not on this platform (the engine plays on Windows; the rest is portable). */
    MEDIA_ERROR_OPEN = 5,             /* The media could not be prepared (media_engine_last_error has the stage). */
    MEDIA_ERROR_PLAYBACK = 6,         /* Playback failed while running. */
    MEDIA_ERROR_REFUSED = 7,          /* A command the engine understood and refused, or did not understand. */
    MEDIA_ERROR_EMPTY = 8             /* Nothing to take (media_engine_poll_frame). */
} media_status;

/* Engine modules (EngineModuleFlags, docs/sdk.md "Modules"). A module left out takes no arena memory and starts no thread:
 * media_query_arena_budget and media_query_module_budget say exactly how much each one takes. */
typedef enum media_module_flags {
    MEDIA_MODULE_VOICE = 1u << 0,           /* Microphone / loopback voice commands: the VAD, the recogniser, the wake gate. */
    MEDIA_MODULE_HUD = 1u << 1,             /* The dock, the command bar and the status cues. */
    MEDIA_MODULE_SUBTITLES = 1u << 2,       /* Sidecar and embedded timed text, the SDF glyph pipeline, quote and concept search. */
    MEDIA_MODULE_NEURAL_UPSCALE = 1u << 3,  /* Neural 2x luma super-resolution (its GPU resources; no arena, no thread). */
    MEDIA_MODULE_CHAPTERS = 1u << 4,        /* Chapters, structural scenes and the keyframe timeline. */
    MEDIA_MODULE_TELEMETRY = 1u << 5,       /* Frame signatures (media_frame_signature_t) and the spectral analysis behind them. */
    MEDIA_MODULE_ALL = (1u << 6) - 1u
} media_module_flags;

typedef enum media_wake_policy {
    MEDIA_WAKE_DEFAULT = 0,  /* "Avion" (and its Hebrew spelling): speech is acted on only when addressed by name. */
    MEDIA_WAKE_DISABLED = 1, /* Push-to-talk: no name; an utterance runs only while the host holds talk (media_engine_set_talk). */
    MEDIA_WAKE_CUSTOM = 2    /* The name in wake_word (UTF-8, at most 31 bytes, compared without letter case). */
} media_wake_policy;

typedef enum media_voice_input {
    MEDIA_VOICE_NONE = 0,       /* No capture: commands come from media_engine_command / media_engine_hear only. */
    MEDIA_VOICE_MICROPHONE = 1, /* The default capture endpoint, through the built-in recogniser (MEDIA_MODULE_VOICE). */
    MEDIA_VOICE_LOOPBACK = 2,   /* What the default output endpoint plays (tests). */
    MEDIA_VOICE_HOST = 3        /* MEDIA_PLAYBACK_HOST: 16 kHz mono samples the host feeds (media_engine_feed_voice). */
} media_voice_input;

typedef enum media_playback_mode {
    MEDIA_PLAYBACK_WINDOW = 0, /* Windows: the engine's window, GPU and audio endpoint; media_engine_run. */
    MEDIA_PLAYBACK_HOST = 1    /* Any platform: the host steps, pumps audio and presents (see above). */
} media_playback_mode;

enum { MEDIA_CODEC_H264 = 1u, MEDIA_CODEC_HEVC = 2u, MEDIA_CODEC_AAC = 4u, MEDIA_CODEC_OPUS = 5u, MEDIA_CODEC_PCM = 7u };
enum {
    MEDIA_UNIT_KEY = 1u,          /* A random-access picture (IDR / IRAP). */
    MEDIA_UNIT_FLUSH = 2u,        /* The first unit after a seek: forget every picture held before it. */
    MEDIA_PICTURE_DECODED = 1u,   /* The planes below hold the picture (the engine's software bridge decoded it). */
    MEDIA_PICTURE_HOST = 2u,      /* The host's decoder holds it: show the picture it made for this pts. */
    MEDIA_PICTURE_LANDING = 4u,   /* The picture a seek landed on (shown at once, paused or not). */
    MEDIA_PICTURE_TRACKING = 8u,  /* The view follows a tracked target (view_x, view_y move with it). */
    MEDIA_PICTURE_TEXTURE = 16u   /* API 14: decoded on the GPU and drawn into a shared texture (media_engine_acquire_output_frame);
                                     no planes. */
};
/* Host mode: one compressed picture, in decode order, for the host's decoder. Valid during the call. */
typedef struct media_video_unit {
    const uint8_t* data;         /* The access unit as stored: NAL units with nal_length_bytes-byte big-endian sizes. */
    uint32_t bytes;
    uint32_t nal_length_bytes;
    const uint8_t* config;       /* The sample entry's avcC / hvcC record. */
    uint32_t config_bytes;
    uint32_t codec;              /* MEDIA_CODEC_H264 or MEDIA_CODEC_HEVC. */
    int64_t pts_100ns, dts_100ns, duration_100ns;
    uint32_t epoch, flags;       /* MEDIA_UNIT_*. */
    uint32_t width, height;
} media_video_unit_t;
/* Returns 0 when the host decoded it (it keeps the picture until on_present names its pts, or a flush), nonzero otherwise. */
typedef int (*media_decode_video_callback)(void* context, const media_video_unit_t* unit);
/* Host mode: one compressed audio access unit (AAC, Opus) for the host's decoder. */
typedef struct media_audio_unit {
    const uint8_t* data; uint32_t bytes;
    const uint8_t* config; uint32_t config_bytes; /* AAC: the AudioSpecificConfig; Opus: dOps. */
    uint32_t codec, sample_rate, channels, flags; /* flags: MEDIA_UNIT_FLUSH after a seek or a track switch. */
    int64_t pts_100ns;
} media_audio_unit_t;
/* Writes interleaved float frames at the track's rate and channels; returns how many (negative: failed). */
typedef int32_t (*media_decode_audio_callback)(void* context, const media_audio_unit_t* unit, float* out, uint32_t max_frames);
/* Host mode: the picture to show now, from media_engine_step. Valid during the call. */
typedef struct media_picture {
    int64_t pts_100ns, duration_100ns;
    uint32_t epoch, sequence;          /* sequence: pictures presented since open. */
    uint32_t width, height;            /* The visible picture (the crop). */
    const uint8_t* luma;               /* MEDIA_PICTURE_DECODED: 8-bit 4:2:0 planes of the visible picture. */
    const uint8_t* cb;
    const uint8_t* cr;
    uint32_t luma_stride, chroma_stride;
    uint32_t flags;                    /* MEDIA_PICTURE_*. */
    float view_zoom, view_x, view_y;   /* The view to draw: zoom (1: all) and its centre (0..1), set by "zoom in", target tracking. */
    const char* subtitle;              /* The caption on screen now (UTF-8), or null. */
    const char* status;                /* The engine's status cue to show now ("Tracking lost · Holding view"), or null. */
    float view_roll;                   /* API 5: the view's turn (stabilised roll), radians, +: clockwise on screen (0: upright). */
    /* API 6 (docs/sdk.md, "Rotation, the horizon and black bars"): how to show the picture. Mirror it left to right when
     * `mirrored` (the file's tkhd), turn it `quarter_turns` clockwise (the file's and the viewer's: "rotate video"), then turn it
     * by `view_level` radians (+: clockwise on screen; the viewer's leveling turn or auto-leveling's) about the view's centre,
     * zoomed in by `level_zoom` (>= 1, the least that keeps the turned picture's corners covered). The view (view_zoom, view_x,
     * view_y) is of the stored picture, before the turns. */
    uint32_t quarter_turns;
    int32_t mirrored;
    float view_level, level_zoom;
} media_picture_t;
typedef void (*media_present_callback)(void* context, const media_picture_t* picture);
/* Custom wake words: the host's speech synthesiser saying `text` with its voice `voice` (0 .. speak_voices - 1) at `rate`
 * (-1 slower, 0, +1 faster) into `samples` (16 kHz mono); returns how many. Called during open only. */
typedef size_t (*media_speak_callback)(void* context, const char* text, uint32_t voice, int32_t rate, float* samples, size_t capacity);

struct media_frame_signature;
/* Called on the engine's thread for every picture put on screen, right after the presentation pass that showed it: it may
 * call media_engine_command, media_engine_hear and media_engine_set_talk. It should return quickly (the next picture waits);
 * the signature is valid during the call only. */
typedef void (*media_frame_callback)(void* context, const struct media_frame_signature* signature);

typedef struct media_engine_config {
    uint32_t struct_size;              /* sizeof(media_engine_config_t). */
    uint32_t modules;                  /* media_module_flags; MEDIA_MODULE_ALL by default (media_engine_config_init). */
    media_wake_policy wake_policy;
    char wake_word[32];                /* MEDIA_WAKE_CUSTOM: the name, NUL-terminated UTF-8. */
    media_voice_input voice_input;
    int32_t visible;                   /* Show the window (1) or play hidden (0, tests). */
    uint32_t maximum_seconds;          /* Stop after this much media (0: to the end). */
    media_frame_callback on_frame;     /* MEDIA_MODULE_TELEMETRY: called per picture shown (null: poll only). */
    void* on_frame_context;
    const char* subtitle_path;         /* MEDIA_MODULE_SUBTITLES: a UTF-8 SRT/WebVTT sidecar (null: none). Read in open. */
    /* Custom wake words (docs/voice.md, "Custom wake words"): the name is made hearable at open, from the engine's formant
     * synthesiser and, much better for real voices, renditions by speak (null: on Windows the system's voices, elsewhere none). */
    media_speak_callback speak;
    void* speak_context;
    uint32_t speak_voices;
    /* How to play (MEDIA_PLAYBACK_WINDOW by default on Windows, MEDIA_PLAYBACK_HOST elsewhere). The rest is host mode's. */
    media_playback_mode playback;
    uint32_t audio_rate, audio_channels;  /* What media_engine_pump_audio delivers: 8000-192000 Hz, 1 or 2 channels (0: 48000, 2). */
    media_present_callback on_present;
    void* on_present_context;
    media_decode_video_callback decode_video; /* Null: the engine's software bridge (H.264 I_PCM and skipped macroblocks). */
    media_decode_audio_callback decode_audio; /* Null: PCM tracks only. */
    void* decoder_context;
    /* API 12 (docs/sdk.md, "Embedding: API 12"). MEDIA_PLAYBACK_WINDOW: the host's window (an HWND) to play inside. The engine's
     * picture becomes a borderless child filling its client area and following its size; the host draws everything else (its
     * own controls, its own design: leave MEDIA_MODULE_HUD out for none of the engine's). Fullscreen becomes the host's to do
     * (MEDIA_EVENT_FULLSCREEN_REQUEST), Escape no longer stops playback, and destroying the window ends media_engine_run
     * (MEDIA_ENDED_CLOSED). Null: the engine's own window. API 11's struct (through decoder_context) is still accepted. */
    void* parent_window;
    /* API 13 (docs/sdk.md, "Texture output: API 13"; were API 12's reserved fields, so the struct keeps its 176 bytes).
     * MEDIA_PLAYBACK_WINDOW with MEDIA_OUTPUT_TEXTURE: no window on the screen; every picture (with the subtitles and, unless left
     * out, the engine's controls) is drawn into one of three shared Direct3D textures of output_width x output_height, for the
     * host's own renderer (a game engine, a compositor, WinUI, a 3D scene): media_engine_query_output_textures,
     * media_engine_acquire_output_frame, media_engine_release_output_frame. Not with parent_window.
     * API 14 (docs/sdk.md, "API 14"): MEDIA_PLAYBACK_HOST with MEDIA_OUTPUT_TEXTURE (Windows): hardware decoding in host mode, the
     * pictures decoded and converted on the GPU into the same textures as media_engine_step shows them (on_present says
     * MEDIA_PICTURE_TEXTURE); when the GPU cannot decode the stream, open falls back to decode_video or the software bridge (the
     * planes, as before) and the texture calls say MEDIA_ERROR_STATE. */
    uint32_t output_width, output_height; /* 16..8192 (MEDIA_OUTPUT_TEXTURE). */
    uint32_t output_flags;                /* MEDIA_OUTPUT_TEXTURE, or 0. */
    uint32_t output_format;               /* API 14 (was reserved, 0): MEDIA_OUTPUT_FORMAT_*, the textures' format (0: 8-bit). */
} media_engine_config_t;
enum { MEDIA_OUTPUT_TEXTURE = 1u };
/* API 14, the shared textures' format (docs/sdk.md, "HDR texture output"):
 * MEDIA_OUTPUT_FORMAT_RGBA8: DXGI_FORMAT_R8G8B8A8_UNORM, display-referred BT.709 (an HDR picture tone-mapped to SDR), as API 13.
 * MEDIA_OUTPUT_FORMAT_PQ10: DXGI_FORMAT_R10G10B10A2_UNORM, PQ (SMPTE ST 2084) with BT.2020 primaries: an HDR10 signal for a
 *   compositor showing HDR (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020). A PQ picture keeps its own codes, an HLG one goes through
 *   its 1000 cd/m2 OOTF, an SDR one has its white at 203 cd/m2 (BT.2408); nothing is tone-mapped.
 * MEDIA_OUTPUT_FORMAT_SCRGB16: DXGI_FORMAT_R16G16B16A16_FLOAT, scRGB: linear BT.709 primaries, 1.0 = 80 cd/m2, values above 1 and
 *   below 0 kept (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709); the same light as PQ10.
 * In the two HDR formats the windowed engine draws the picture only: no captions, controls, cues or snapshots (the host draws its
 * own). */
enum { MEDIA_OUTPUT_FORMAT_RGBA8 = 0u, MEDIA_OUTPUT_FORMAT_PQ10 = 1u, MEDIA_OUTPUT_FORMAT_SCRGB16 = 2u };

/* The cross-modal frame signature (docs/sdk.md, "Frame signatures"): what the engine has already computed about each
 * picture on screen and the sound under it, delivered at no extra cost (no extra GPU pass, no extra analysis). 64 bytes.
 * The video fields come from the conversion's statistics pass (8x8-block luma samples of the picture's crop), the audio
 * fields from the DSP's spectral analysis (4096-point FFT every 1024 frames) of the programme just before it reaches the
 * endpoint. A field that is not known is negative. */
typedef struct media_frame_signature {
    int64_t pts_100ns;           /* The picture's presentation time. */
    int64_t presented_100ns;     /* When it was put on screen (the engine's monotonic clock). */
    uint32_t sequence;           /* Signatures produced since open (a gap: the poll ring overflowed). */
    uint32_t epoch;              /* The playback epoch (a seek starts a new one). */
    float luminance_mean;        /* 0..1, of the luma samples (code value / full scale, as coded). */
    float luminance_peak;        /* 0..1, the brightest sample. */
    float tile_motion;           /* 0..1, the share of 8x8 blocks that changed by more than 1.2 % since the previous picture
                                    (-1: the first picture of an epoch or of a zoom region). */
    float speech_probability;    /* 0..1, the programme's speech probability (-1: no audio or no analysis). */
    float dominant_band_hz;      /* The centre of the loudest of the ten octave bands (31.5 Hz .. 16 kHz); -1: silence. */
    float dominant_band_db;      /* Its level, dB relative to a full-scale sine. */
    uint32_t flags;              /* MEDIA_FRAME_SCENE_CUT, MEDIA_FRAME_MEASURED. */
    uint32_t dominant_band;      /* 0..9, or 10 for silence. */
    int64_t scene_cut_pts_100ns; /* MEDIA_FRAME_SCENE_CUT: where the cut is. The live detector confirms a cut four pictures
                                    after it (it must see the new shot hold), so the flag comes on that picture's signature
                                    and names the cut's own picture here; -1 otherwise. */
} media_frame_signature_t;

enum { MEDIA_FRAME_SCENE_CUT = 1u, MEDIA_FRAME_MEASURED = 2u };

/* What one module takes (media_query_module_budget). */
typedef struct media_module_budget {
    uint64_t arena_bytes; /* Its region of the engine's arena. */
    uint32_t threads;     /* Threads it starts while open. */
    uint32_t reserved;
} media_module_budget_t;

/* What an open engine holds now (media_engine_stats). */
typedef struct media_engine_stats {
    uint64_t arena_capacity_bytes; /* The engine's arena, as committed. */
    uint64_t arena_used_bytes;     /* Of which in use. */
    uint64_t module_arena_bytes;   /* The enabled modules' regions within it. */
    uint32_t modules;              /* The modules this engine was created with. */
    uint32_t threads;              /* Threads the engine started for this media (worker, file reader, modules). */
    uint32_t presented, dropped;   /* Pictures. */
    uint32_t signatures;           /* Frame signatures produced. */
    uint32_t signatures_lost;      /* Not taken by media_engine_poll_frame before the ring (64) filled. */
    uint32_t commands, refused;    /* Commands given (media_engine_command / media_engine_hear), refused. */
    uint32_t wake_ignored;         /* Utterances the wake policy set aside. */
    uint32_t wake_templates;       /* A custom wake word's acoustic templates (0: "Avion", or a name heard as text only). */
} media_engine_stats_t;

/* Host mode: what plays now (media_engine_state). */
typedef struct media_playback_state {
    int64_t position_100ns, duration_100ns;
    uint32_t epoch;
    int32_t paused, muted, tracking;   /* tracking: 1 following a target, 2 the target lost (the view held). */
    float volume, speed;
    float view_zoom, view_x, view_y;
    uint32_t audio_track;              /* The track id playing (0: none). */
    uint32_t audio_tracks;             /* Playable audio tracks. */
    char audio_language[4];            /* ISO 639-2 of the track playing. */
    uint32_t presented, dropped, underruns; /* Pictures; audio frames of silence pumped while playing (the host was ahead). */
    int32_t saccade;                   /* API 3: the audio saccade is on (the view turns toward sudden sounds). */
    uint32_t orientation;              /* API 6: bits 0-1 the quarter turns shown (clockwise), bit 2 mirrored, bit 3 auto-leveling on,
                                          bit 4 auto-crop on (MEDIA_ORIENTATION_*). */
    float level_degrees;               /* API 6: the leveling turn, degrees, +: clockwise. */
} media_playback_state_t;
enum { MEDIA_ORIENTATION_TURNS = 3u, MEDIA_ORIENTATION_MIRRORED = 4u, MEDIA_ORIENTATION_AUTO_LEVEL = 8u, MEDIA_ORIENTATION_AUTO_CROP = 16u };

typedef struct media_engine media_engine_t; /* Opaque; lives in the host's storage. */

/* ---- Live sensors (API 3; docs/sdk.md, "Live sensors") ---- */
typedef enum media_pixel_format {
    MEDIA_PIXEL_NV12 = 1,  /* 8-bit 4:2:0: a luma plane, then interleaved Cb Cr (limited range BT.709). */
    MEDIA_PIXEL_P010 = 2,  /* 10-bit 4:2:0 in the high bits of little-endian 16-bit samples, same layout. */
    MEDIA_PIXEL_RGBA8 = 3  /* 8-bit R G B A, full range BT.709 (converted to limited-range Y'CbCr 4:2:0). */
} media_pixel_format;
typedef enum media_sample_format { MEDIA_SAMPLE_F32 = 1, MEDIA_SAMPLE_S16 = 2 } media_sample_format; /* Interleaved. */
typedef struct media_sensor_config {
    uint32_t struct_size;           /* sizeof(media_sensor_config_t). */
    uint32_t width, height;         /* The camera's frames (every frame this size): even, 64..4096 x 64..2304; 0 x 0: no camera. */
    uint32_t audio_rate;            /* The microphones: 8000..192000 Hz. */
    uint32_t audio_channels;        /* 1..8 (0: none). Channels 0 and 1 are the pair the sound locator listens to (left, right). */
    float field_of_view;            /* The camera's horizontal field of view, radians (0: 90 degrees): the IMU's and the saccade's mapping. */
    float microphone_spacing;       /* Between channels 0 and 1, metres (0: 0.2): the direction from the time difference. */
    uint32_t options;               /* API 4: MEDIA_SENSOR_GPU, MEDIA_SENSOR_STABILIZE; API 5: MEDIA_SENSOR_GPU_10BIT; API 8:
                                       MEDIA_SENSOR_GPU_FIT (0: none). */
    uint32_t output_width, output_height; /* API 4, MEDIA_SENSOR_GPU: the GPU image (even; 0 x 0: the camera's size). */
    uint32_t reserved[2];
} media_sensor_config_t;
enum { MEDIA_SENSOR_IMU = 1u }; /* media_sensor_frame_t.flags: the rotation fields hold the IMU's deltas. */
/* media_sensor_config_t.options (API 4, 5; docs/sdk.md, "GPU live ingest"):
 * MEDIA_SENSOR_GPU: every camera frame also goes through the GPU passes (Windows, Direct3D 12): uploaded into a pre-allocated
 *   texture ring, its view (zoom, pan, saccade, tracking, stabilisation) converted to an RGBA image of output_width x
 *   output_height with the picture controls, micro-motion and edge-directed or neural super-resolution
 *   (media_engine_set_picture_controls); media_query_gpu_image has the newest. MEDIA_ERROR_UNSUPPORTED elsewhere or without
 *   a hardware device (the sensor is not opened).
 * MEDIA_SENSOR_STABILIZE: electronic image stabilisation from the IMU from the start (media_engine_set_stabilization); since API 5 with roll
 *   (the view turns with the roll's shake: a level horizon stays level).
 * MEDIA_SENSOR_GPU_10BIT (API 5, with MEDIA_SENSOR_GPU): the GPU's texture ring is 10-bit (P010): MEDIA_PIXEL_P010 frames go
 *   to the GPU as they are, all 1,024 levels through the picture controls (dithered to the 8-bit image); 8-bit frames go in
 *   as their 10-bit codes. Without it P010 frames reach the GPU as the engine's 8-bit conversion.
 * MEDIA_SENSOR_GPU_FIT (API 8, with MEDIA_SENSOR_GPU; docs/sdk.md, "Fit instead of fill"): the GPU image shows the view whole.
 *   Without it an odd number of quarter turns (90 or 270 degrees) converts a region of the output's turned shape, which fills
 *   the image and leaves out the view's ends; with it the view's whole region, turned, is scaled to fit inside the image (one
 *   scale both ways, centred) and the rest of the image is black (pillarbox or letterbox bars: media_gpu_image_t's
 *   MEDIA_GPU_FITTED and picture_left .. picture_bottom). No sample of the camera's view is lost. The same at 0 or 180 degrees
 *   when the image's shape is not the view's. */
enum { MEDIA_SENSOR_GPU = 1u, MEDIA_SENSOR_STABILIZE = 2u, MEDIA_SENSOR_GPU_10BIT = 4u, MEDIA_SENSOR_GPU_FIT = 8u };
typedef struct media_sensor_frame {
    uint32_t struct_size;           /* sizeof(media_sensor_frame_t). */
    uint32_t format;                /* media_pixel_format. */
    uint32_t width, height;         /* As configured. */
    const uint8_t* planes[2];       /* NV12, P010: luma, CbCr; RGBA8: the pixels (planes[1] unused). Read during the call only. */
    uint32_t strides[2];            /* Bytes per row. */
    int64_t timestamp_100ns;        /* The sensor's capture time, increasing from frame to frame (any origin). */
    /* MEDIA_SENSOR_IMU: the camera's rotation since the previous frame, radians: yaw positive turning right (the scene moves
     * left), pitch positive tilting up (the scene moves down), roll positive turning clockwise seen from behind the camera.
     * Perception takes it out of the flow: what remains is the world's own motion. */
    float imu_yaw, imu_pitch, imu_roll;
    uint32_t flags;
} media_sensor_frame_t;
void media_sensor_config_init(media_sensor_config_t* config);
/* Opens live sensors instead of a file (host mode only): MEDIA_ERROR_STATE when open or not in host mode. */
media_status media_engine_open_sensor(media_engine_t* engine, const media_sensor_config_t* sensor);
/* One camera frame: copied (converted to the engine's 8-bit 4:2:0), then shown at once. No allocation. */
media_status media_engine_ingest_video_frame(media_engine_t* engine, const media_sensor_frame_t* frame);
/* `frames` interleaved frames of the configured channels; `timestamp_100ns` the first one's capture time (-1: unknown). */
media_status media_engine_ingest_audio_samples(media_engine_t* engine, const void* samples, media_sample_format format, uint32_t frames,
                                               int64_t timestamp_100ns);

/* ---- Perception (API 3; docs/picture.md, "Perception"; MEDIA_MODULE_TELEMETRY, pictures of 128 x 128 samples or more) ---- */
enum {
    MEDIA_TELEMETRY_FLOW = 1u,      /* The flow was measured (an earlier picture to compare with). */
    MEDIA_TELEMETRY_LOOMING = 2u,   /* Something approaches: ttc_seconds and the hazard are set. */
    MEDIA_TELEMETRY_EGO = 4u,       /* The camera's rotation (IMU) was taken out of the flow. */
    MEDIA_TELEMETRY_SOUND = 8u,     /* A sound was localised (the sound_ fields). */
    MEDIA_TELEMETRY_SACCADE = 16u,  /* The audio saccade is on. */
    MEDIA_TELEMETRY_STABILIZED = 32u, /* API 4: stabilisation is on (the view_ fields include it). */
    MEDIA_TELEMETRY_GROUND = 64u,     /* API 5: the camera travels forward over a ground: its flow is expected, not an approach
                                         (horizon_y, focus_x). */
    MEDIA_TELEMETRY_FOVEA = 128u,     /* API 5: the approach was found by the fovea's finer tiles (small or distant). */
    MEDIA_TELEMETRY_HORIZON = 256u,   /* API 6: the horizon's tilt is known (estimated_horizon_deg). */
    MEDIA_TELEMETRY_BARS = 512u,      /* API 6: black bars are encoded in the picture (active_left .. active_bottom: inside them). */
    MEDIA_TELEMETRY_OBJECTS = 1024u,  /* API 7: objects are perceived (object_count > 0). */
    MEDIA_TELEMETRY_VIBRATION = 2048u,/* API 9: a vibration stands above the noise (vibration_hz, vibrometry_confidence > 0). */
    MEDIA_TELEMETRY_VITALS = 4096u,   /* API 9: a pulse-rate component does (vital_bpm, vital_confidence > 0). */
    MEDIA_TELEMETRY_VIRTUAL_IMU = 8192u /* API 10: the camera's motion to this picture is the virtual IMU's (no IMU deltas came
                                           with it): ego_dx .. ego_turn; perception, stabilisation, vibrometry and the event
                                           camera took it as an IMU's. */
};
/* API 10: media_frame_telemetry_t.vibrometry_flags. */
enum { MEDIA_VIBROMETRY_FOLLOWING = 1u }; /* The region follows the tracked target's path: its window goes on while it moves. */
/* API 9: where remote vibrometry found its component (media_frame_telemetry_t.vibrometry_channel, vital_channel). */
enum { MEDIA_VIBROMETRY_LUMA = 0, MEDIA_VIBROMETRY_CB = 1, MEDIA_VIBROMETRY_CR = 2, MEDIA_VIBROMETRY_SHIFT_X = 3, MEDIA_VIBROMETRY_SHIFT_Y = 4 };
/* API 7 (docs/sdk.md, "Several objects at once"; docs/picture.md): an object perception keeps track of, from picture to picture.
 * Up to MEDIA_MAX_OBJECTS at once, the most urgent first: confirmed approaches by time to contact (soonest first), then moving
 * or salient objects by how much of the scene's attention they hold (energy). 64 bytes. */
enum { MEDIA_MAX_OBJECTS = 4 };
enum {
    MEDIA_OBJECT_APPROACHING = 1u, /* It approaches the camera (confirmed on its track): ttc_seconds and divergence are set. */
    MEDIA_OBJECT_FOVEA = 2u,       /* Its approach was found by the fovea's finer tiles (small or distant). */
    MEDIA_OBJECT_TARGET = 4u,      /* Target tracking follows it (media_frame_telemetry_t.target_track_id). */
    MEDIA_OBJECT_OCCLUDED = 8u     /* API 8: hidden behind another object it is crossing: where its track's motion puts it (tiles 0). */
};
typedef struct media_detected_object {
    uint32_t track_id;              /* 1, 2, ...: kept by the same object while it is seen; not reused while the sensor is open. */
    uint32_t flags;                 /* MEDIA_OBJECT_*. */
    float x, y;                     /* Its centre, fractions of the picture (0 left / top .. 1). */
    float width, height;            /* Its extent, fractions of the picture. */
    float velocity_x, velocity_y;   /* Its track's motion, picture widths / heights per second (+x right, +y down; 0 when new). */
    float ttc_seconds;              /* MEDIA_OBJECT_APPROACHING: time to contact (-1 otherwise). */
    float divergence;               /* Its expansion, 1/s (0: none). */
    float saliency;                 /* 0..1: its tiles' mean saliency. */
    float energy;                   /* Its tiles' saliency added up (in tiles of the 16 x 16 grid). */
    uint32_t support;               /* Expansion windows that found its approach. */
    uint32_t tiles;                 /* Salient tiles in it (0: an approach outside the saliency mask). */
    uint32_t age;                   /* Pictures its track has been seen in (1: new). */
    uint32_t reserved;
} media_detected_object_t;
typedef struct media_frame_telemetry {
    uint32_t struct_size;           /* sizeof(media_frame_telemetry_t). */
    uint32_t flags;                 /* MEDIA_TELEMETRY_*. */
    int64_t pts_100ns;              /* The picture measured (the newest shown). */
    uint32_t sequence;              /* Pictures measured since open. */
    float motion_x, motion_y;       /* The dominant motion (median tile), picture widths / heights per second (+x right, +y down). */
    float flow_coverage;            /* The share of the 16 x 16 tiles whose flow is reliable. */
    float divergence;               /* The strongest approach's expansion rate, 1/s (0: none). */
    float ttc_seconds;              /* Time to contact, 2 / divergence (-1: nothing approaching). */
    float hazard_x, hazard_y;       /* Where it approaches (its focus of expansion), fractions of the picture. */
    float salient_fraction;         /* Of the tiles in the saliency mask (media_query_saliency_grid). */
    float sound_azimuth;            /* The newest localised sound: -1 the left channel's side .. +1 the right's. */
    float sound_level_db;           /* Its onset's level, dB relative to a full-scale sine. */
    int64_t sound_pts_100ns;        /* Its onset (the samples' timestamps; -1 unknown). */
    uint32_t sound_events;          /* Sounds localised since open. */
    uint32_t saccades;              /* Times the view turned toward one. */
    float view_zoom, view_x, view_y;/* The view now. */
    float view_roll;                /* API 5: its turn (stabilised roll), radians, +: clockwise on screen. */
    float horizon_y, focus_x;       /* API 5, MEDIA_TELEMETRY_GROUND: the ground's horizon and focus of expansion, fractions (-1: none). */
    /* API 6 (docs/sdk.md, "Rotation, the horizon and black bars"): of the pictures the engine holds (its software bridge, live
     * sensors), surveyed as the windowed engine's statistics pass surveys its own. */
    float estimated_horizon_deg;    /* MEDIA_TELEMETRY_HORIZON: the stored picture's tilt, degrees, +: the horizon's right end
                                       lower; the median of the recent pictures' (auto-leveling turns by minus it); 0 otherwise. */
    uint32_t active_left, active_top, active_right, active_bottom; /* The picture inside black bars encoded in it, samples of the
                                       stored picture (right, bottom exclusive); the whole picture without bars or before they are known. */
    float level_degrees;            /* The leveling turn now (the viewer's or auto-leveling's), degrees, +: clockwise. */
    uint32_t reserved[2];
    /* API 7 (docs/sdk.md, "Several objects at once"). */
    uint32_t object_count;          /* MEDIA_TELEMETRY_OBJECTS: objects[0 .. object_count - 1], the most urgent first. */
    uint32_t target_track_id;       /* The object target tracking follows (0: none, or tracking something that is not one). */
    media_detected_object_t objects[MEDIA_MAX_OBJECTS];
    /* API 9 (docs/sdk.md, "Remote vibrometry"; docs/picture.md): periodic motion and flicker measured without a sensor, on a
     * region held still in the scene (the target's while tracking, else the view's fovea steadied against the camera's shake)
     * of the pictures the engine holds (its software bridge, live sensors), over the last 256 pictures (~8.5 s at 30/s). */
    float vibration_hz;             /* MEDIA_TELEMETRY_VIBRATION: the strongest component, 0.7 Hz .. min(15 Hz, 0.45 x the picture rate);
                                       -1: none above the noise. */
    float vibrometry_confidence;    /* 0..1: how far it stands above the spectrum's noise floor (0: noise, a still scene). */
    float vibration_amplitude;      /* Its amplitude: codes (luma, Cb, Cr) or samples of displacement (shift). */
    uint32_t vibrometry_channel;    /* MEDIA_VIBROMETRY_*: what vibrates (the brightness, a colour, the content's position). */
    float vital_bpm;                /* MEDIA_TELEMETRY_VITALS: the strongest component in 0.75 .. 3.5 Hz x 60 (a pulse: 45 .. 210 BPM;
                                       remote photoplethysmography on a face's colour, or its motion); -1: none. */
    float vital_confidence;         /* 0..1, as vibrometry_confidence. */
    uint32_t vital_channel;         /* MEDIA_VIBROMETRY_*. */
    float vibrometry_seconds;       /* The window's span (0: nothing measured yet; nothing is reported before 2.5 s). */
    float vibrometry_left, vibrometry_top, vibrometry_right, vibrometry_bottom; /* The region measured, samples of the picture. */
    /* API 10 (docs/sdk.md, "API 10"; docs/picture.md, "The virtual IMU", "Moving targets"). */
    uint32_t vibrometry_flags;      /* MEDIA_VIBROMETRY_FOLLOWING. */
    float ego_dx, ego_dy;           /* The virtual IMU: the scene's motion at the centre since the previous picture, samples (+x right,
                                       +y down); 0 when it found none (the background did not agree, a first picture). */
    float ego_turn;                 /* Its turn about the centre, radians (+: clockwise on the picture). */
    uint32_t ego_tiles, ego_inliers;/* Its tiles measured, and the background's among them. */
} media_frame_telemetry_t;
/* API 5's media_frame_telemetry_t (112 bytes, through horizon_y and focus_x), API 6's (136 bytes, through reserved), API 7's
 * and 8's (400 bytes, through objects) and API 9's (448 bytes, through vibrometry_bottom) are still accepted: struct_size says
 * which. */
/* The newest picture's perception and the newest localised sound. MEDIA_ERROR_EMPTY before the first measured picture. */
media_status media_query_frame_telemetry(const media_engine_t* engine, media_frame_telemetry_t* telemetry);
enum { MEDIA_SALIENCY_TILES = 16 };
typedef struct media_saliency_grid {
    uint32_t struct_size;           /* sizeof(media_saliency_grid_t). */
    uint32_t salient_tiles;         /* Tiles in the mask. */
    int64_t pts_100ns;
    uint16_t mask[MEDIA_SALIENCY_TILES];               /* Row r, bit c: tile (c, r) is salient (look here). */
    uint8_t weight[MEDIA_SALIENCY_TILES * MEDIA_SALIENCY_TILES]; /* 0..255: 0.55 change + 0.30 motion + 0.15 edges (row-major). */
    uint8_t flow_valid[MEDIA_SALIENCY_TILES * MEDIA_SALIENCY_TILES];
    float flow_x[MEDIA_SALIENCY_TILES * MEDIA_SALIENCY_TILES]; /* Each tile's own motion, picture widths per second. */
    float flow_y[MEDIA_SALIENCY_TILES * MEDIA_SALIENCY_TILES]; /* Picture heights per second. */
} media_saliency_grid_t;
/* The foveated saliency grid of the newest picture measured (MEDIA_ERROR_EMPTY before it). */
media_status media_query_saliency_grid(const media_engine_t* engine, media_saliency_grid_t* grid);
/* The audio saccade ("look at sound", "עקוב אחרי הקול"): on, each sudden sound turns the view toward where it came from (at
 * 2x when the view was whole); MEDIA_ERROR_UNSUPPORTED without a stereo pair and a picture. Host mode. */
media_status media_engine_set_audio_saccade(media_engine_t* engine, int on);
/* Target tracking onto one of the perceived objects (API 7; "next target", "מטרה הבאה", "cible suivante" and, in the windowed
 * engine, Tab while tracking): `track_id` from media_frame_telemetry_t.objects, or MEDIA_TARGET_NEXT for the one after the object
 * followed now in the table's order (the most urgent when none is: it starts tracking). The view goes to it at once (and the
 * audio zoom's beam with it); the tracker locks onto it on the next picture. MEDIA_ERROR_EMPTY: no such object in view;
 * MEDIA_ERROR_REFUSED: MEDIA_TARGET_NEXT while the only object is the one followed. Host mode. */
enum { MEDIA_TARGET_NEXT = 0u };
media_status media_engine_track_object(media_engine_t* engine, uint32_t track_id);

/* ---- Rotation, the horizon and black bars (API 6; docs/sdk.md, "Rotation, the horizon and black bars") ---- */
/* The viewer's turns: `quarter_turns` clockwise (after the file's tkhd) and a leveling turn of `level_degrees` (-15..15,
 * +: clockwise; kept in tenths), shown through media_picture_t; auto-leveling stops. MEDIA_ERROR_UNSUPPORTED without a picture,
 * MEDIA_ERROR_INVALID_ARGUMENT outside the range. Host mode (the windowed engine takes the commands: "rotate video", ...). */
media_status media_engine_set_orientation(media_engine_t* engine, uint32_t quarter_turns, float level_degrees);
/* Auto-leveling ("level horizon"): the leveling turn follows the horizon's tilt the engine finds in the pictures it holds
 * (its software bridge, live sensors; a host decoder's pictures are not the engine's to survey). Host mode. */
media_status media_engine_set_auto_level(media_engine_t* engine, int on);
/* Auto-crop ("crop black bars"): the view fills with the picture inside black bars encoded in it (letterbox, pillarbox) as soon
 * as they are known, and follows them; a zoom command turns it off. With MEDIA_SENSOR_GPU the GPU image is leveled and cropped
 * the same way, and since API 7 quarter-turned too (media_gpu_image_t.quarter_turns). Host mode. */
media_status media_engine_set_auto_crop(media_engine_t* engine, int on);

/* ---- Stabilisation and the GPU image (API 4; docs/picture.md, "Stabilisation", "GPU live ingest") ---- */
/* Electronic image stabilisation from the frames' IMU deltas: the view (media_picture_t, telemetry, the GPU image) moves with
 * the shake inside a 1.25x crop, pans pass through (rotation slower than ~0.5 Hz). Perception always takes the IMU's rotation
 * out of its flow and saliency, stabilised or not. Since API 10 frames without IMU deltas (MEDIA_SENSOR_IMU unset) and the
 * software bridge's files are steadied by the virtual IMU, the camera's motion read off the background (MEDIA_MODULE_TELEMETRY;
 * MEDIA_TELEMETRY_VIRTUAL_IMU). MEDIA_ERROR_UNSUPPORTED without pictures the engine holds. Since API 11 the windowed engine too: its
 * view, each picture measured by the virtual IMU before it is converted (MEDIA_MODULE_TELEMETRY, a picture of 128 samples a side or
 * more: MEDIA_ERROR_UNSUPPORTED otherwise). */
media_status media_engine_set_stabilization(media_engine_t* engine, int on);
enum { MEDIA_RECONSTRUCT_BILINEAR = 0, MEDIA_RECONSTRUCT_EDGE = 1, MEDIA_RECONSTRUCT_NEURAL = 2 };
typedef struct media_picture_controls {
    uint32_t struct_size;           /* sizeof(media_picture_controls_t). */
    float brightness;               /* -0.5..0.5 (0: as shot). */
    float contrast, saturation;     /* 0..2 (1: as shot). */
    float gamma;                    /* 0.5..2.5 (1). */
    float shadow_lift;              /* 0..1: lifts the shadows, highlights from Y' 0.75 untouched (0: off). */
    float micro_motion;             /* 1..8: small temporal changes magnified this many times (1: off). */
    uint32_t reconstruction;        /* MEDIA_RECONSTRUCT_*: while the view is upscaled to the image. */
    uint32_t reserved[3];
} media_picture_controls_t;
void media_picture_controls_init(media_picture_controls_t* controls); /* Neutral, edge-directed. */
/* MEDIA_SENSOR_GPU: from the next frame. MEDIA_ERROR_UNSUPPORTED without the GPU path. */
media_status media_engine_set_picture_controls(media_engine_t* engine, const media_picture_controls_t* controls);
/* MEDIA_GPU_TEN_BIT (API 5): converted from 10-bit samples. MEDIA_GPU_MICRO_FOLLOWED (API 5): micro-motion's reference followed
 * the view's motion and the scene's (IMU) for this image instead of starting over (docs/picture.md, "Roll and compensated
 * micro-motion"). */
enum { MEDIA_GPU_NEURAL = 1u, MEDIA_GPU_EDGE = 2u, MEDIA_GPU_GRADED = 4u, MEDIA_GPU_MICRO_MOTION = 8u, MEDIA_GPU_STABILIZED = 16u,
       MEDIA_GPU_TEN_BIT = 32u, MEDIA_GPU_MICRO_FOLLOWED = 64u,
       MEDIA_GPU_FITTED = 128u /* API 8 (MEDIA_SENSOR_GPU_FIT): the view fitted whole, black bars around picture_left .. picture_bottom. */ };
typedef struct media_gpu_image {
    uint32_t struct_size;           /* sizeof(media_gpu_image_t). */
    uint32_t width, height, stride; /* stride: bytes per row. */
    const uint8_t* rgba;            /* 8-bit R G B A (full range BT.709); valid until two more frames have been ingested. */
    int64_t pts_100ns;              /* The frame's timestamp. */
    uint32_t sequence;              /* Frames converted since open. */
    uint32_t flags;                 /* MEDIA_GPU_*: how this image was made. */
    uint32_t left, top, right, bottom; /* The camera's region converted (the view). */
    float gpu_ms;                   /* The upload and the passes on the GPU (-1: unknown). */
    uint32_t dropped;               /* Frames refused so far because the GPU was still busy with three. */
    float roll;                     /* API 5: the view's turn as converted (radians, +: clockwise on screen); the region is upright. */
    /* API 7 (docs/sdk.md, "Quarter turns of the GPU image"): the image is the view turned `quarter_turns` clockwise (the
     * orientation's: the viewer's "rotate video", media_engine_set_orientation), mirrored first when `mirrored`. Its size stays
     * output_width x output_height: an odd number of turns converts a region of the camera of the output's turned shape
     * (output_height : output_width), as large as the view holds, about the view's centre, so the turned picture fills the
     * image undistorted; left .. bottom is that region, upright in the camera. */
    uint32_t quarter_turns;
    int32_t mirrored;
    uint32_t reserved;
    /* API 8: where the picture lies in the image (pixels; right, bottom exclusive): inside the bars with MEDIA_GPU_FITTED, the
     * whole image otherwise. API 7's struct (through reserved) is still accepted. */
    uint32_t picture_left, picture_top, picture_right, picture_bottom;
} media_gpu_image_t;
/* The newest GPU image whose work has completed (no wait); MEDIA_ERROR_EMPTY before the first. */
media_status media_query_gpu_image(media_engine_t* engine, media_gpu_image_t* image);

/* ---- Forensic snapshots (API 8; docs/sdk.md, "Snapshots") ----
 * The picture as the engine made it, written as an uncompressed 24-bit BMP (top-down rows, no compression, no dependency) with
 * a sidecar of its telemetry beside it (the same name, .json: the picture's time, the image's hash, the view, the orientation,
 * the horizon, the picture controls, the objects perceived and the target). No allocation; the frames keep coming.
 * - Host mode with MEDIA_SENSOR_GPU: the newest GPU image, exactly as media_query_gpu_image gives it (zoom, pan, tracking,
 *   stabilisation, turns, fit, leveling, picture controls, micro-motion, super-resolution). Written during the call.
 * - Host mode otherwise: the view of the picture the engine holds (its software bridge's, a camera's): the view's region
 *   (zoom, pan, tracking) of the 8-bit 4:2:0 picture, converted to RGB (BT.709, limited range) and turned by the quarter turns
 *   and mirror; the leveling turn and the stabiliser's roll are the host's to draw (the sidecar has them). Written during the
 *   call. MEDIA_ERROR_UNSUPPORTED while the host's decoder holds the pictures.
 * - The windowed engine: the picture as composed on screen (the window's video rectangle: zoom, leveling, the split
 *   comparison, Pinned | Live), before captions, the dock and the status cues; S, "take snapshot", "צילום מסך", "capturer
 *   l'image" in the nine languages take one too. Copied on the GPU with the next composite and written by the engine's
 *   snapshot thread: the call returns at once (MEDIA_OK: on its way), media_query_snapshot says when it is written.
 * `path`: the BMP's UTF-8 path (its folder must exist), or null for the snapshot folder (media_engine_set_snapshot_folder; by
 * default "Pictures\MediaEngine" in the user's profile on Windows, the working directory elsewhere) and a name from the media
 * and the picture's time. MEDIA_ERROR_EMPTY: no picture yet; MEDIA_ERROR_STATE: the windowed engine is still writing one. */
enum { MEDIA_SNAPSHOT_GPU_IMAGE = 1u, MEDIA_SNAPSHOT_VIEW = 2u, MEDIA_SNAPSHOT_WINDOW = 3u };
typedef struct media_snapshot {
    uint32_t struct_size;           /* sizeof(media_snapshot_t). */
    uint32_t sequence;              /* Snapshots written since open (this one's number, from 1). */
    int32_t status;                 /* media_status: MEDIA_OK written; MEDIA_ERROR_OPEN the file could not be written. */
    uint32_t source;                /* MEDIA_SNAPSHOT_*: what was captured. */
    uint32_t width, height;         /* The image written. */
    int64_t pts_100ns;              /* The picture's presentation time. */
    uint64_t frame_hash;            /* FNV-1a 64 of the image's R G B bytes, rows top to bottom (the sidecar's "frame_hash"). */
    char path[512];                 /* The BMP written (UTF-8); the sidecar is the same path with .json for .bmp. */
} media_snapshot_t;
/* Where snapshots without a path go (UTF-8, at most 511 bytes; null: the default). The folder must exist. */
media_status media_engine_set_snapshot_folder(media_engine_t* engine, const char* folder);
media_status media_engine_save_snapshot(media_engine_t* engine, const char* path);
/* The last snapshot written (or failed). MEDIA_ERROR_EMPTY before the first, and while the windowed engine writes one. */
media_status media_query_snapshot(const media_engine_t* engine, media_snapshot_t* snapshot);
/* API 17: a snapshot into the host's memory instead of a file: the same image and frame hash as media_engine_save_snapshot (the
 * windowed engine's window picture, host mode's GPU image or view), as 8-bit pixels with alpha 255, rows `stride` bytes apart
 * (0: width x 4), top row first. Synchronous in both modes (the windowed engine's snapshot thread is waited for, at most 3 s);
 * no file, no sidecar, nothing allocated. `width`, `height`, `pts_100ns` and `frame_hash` are set on return; MEDIA_ERROR_STORAGE:
 * the buffer is smaller than stride x height (width and height say what it needs: call again with a larger one). */
enum { MEDIA_PIXELS_BGRA8 = 1u, MEDIA_PIXELS_RGBA8 = 2u };
typedef struct media_snapshot_buffer {
    uint32_t struct_size;           /* sizeof(media_snapshot_buffer_t). */
    uint32_t format;                /* MEDIA_PIXELS_BGRA8 or MEDIA_PIXELS_RGBA8. */
    void* pixels;                   /* The host's buffer. */
    uint64_t capacity_bytes;        /* Its size. */
    uint32_t stride;                /* Bytes from a row to the next (0: width x 4). */
    uint32_t width, height;         /* Out: the image. */
    uint32_t reserved;
    int64_t pts_100ns;              /* Out: the picture's presentation time. */
    uint64_t frame_hash;            /* Out: as media_snapshot_t's. */
} media_snapshot_buffer_t;
media_status media_engine_snapshot_to_memory(media_engine_t* engine, media_snapshot_buffer_t* buffer);

/* ---- The event camera (API 9; docs/sdk.md, "Event camera"; docs/picture.md) ----
 * The pictures the engine holds (its software bridge, live sensors; host mode) as a neuromorphic event camera's output: each
 * pixel of a sensor of at most 640 x 480 (the picture box-averaged: 4K is 6 x 6 samples a pixel) fires an ON or OFF event each
 * time its log intensity moves by the contrast threshold from where it last fired, the crossings between two pictures spread
 * over the interval in time order. Stabilised (the default): the pixels' references are fixed to the scene and moved by the
 * camera's IMU motion (media_sensor_frame_t), so the camera shaking or turning fires nothing and only what moves in the scene
 * (or changes its brightness) does; x, y are then the scene's (the sensor's frame, re-based after a pan of 6 pixels or a turn
 * of 0.03 radians). 16 bytes an event; the ring holds 65,536. */
typedef struct media_event_spike {
    uint16_t x, y;                  /* The event sensor's pixel (media_event_camera_state_t.width x height). */
    int8_t polarity;                /* +1 brighter (ON), -1 darker (OFF). */
    uint8_t contrast;               /* The change that fired it: |log intensity change| in 1/64 units (255: 4 or more). */
    uint16_t reserved;
    int64_t timestamp_us;           /* The pictures' clock, microseconds: between the previous picture and this one (16 slices). */
} media_event_spike_t;
enum { MEDIA_EVENTS_UNSTABILIZED = 1u }; /* media_event_camera_config_t.flags: compare pixel for pixel (the camera's motion fires). */
typedef struct media_event_camera_config {
    uint32_t struct_size;           /* sizeof(media_event_camera_config_t). */
    float contrast_threshold;       /* Natural log units (0: 0.15, ~16 %); 0.01 .. 2. */
    uint32_t max_events_per_pixel;  /* For one picture (0: 4); beyond, the pixel's reference jumps to the picture. */
    uint32_t flags;                 /* MEDIA_EVENTS_*. */
    uint32_t reserved[4];
} media_event_camera_config_t;
typedef struct media_event_camera_state {
    uint32_t struct_size;           /* sizeof(media_event_camera_state_t). */
    int32_t on;
    uint32_t width, height;         /* The event sensor. */
    uint32_t scale;                 /* Picture samples per sensor pixel, across and down. */
    uint32_t pictures;              /* Pictures compared since it was turned on (or the engine opened). */
    uint64_t events;                /* Written to the ring. */
    uint64_t dropped;               /* Not written: the ring was full (the latest of a picture's first). */
    uint32_t rebases;               /* Times the scene's frame was carried into the camera's. */
    uint32_t last_events;           /* The newest picture's. */
} media_event_camera_state_t;
void media_event_camera_config_init(media_event_camera_config_t* config);
/* On with `config`, off with null (the ring's events stay for media_poll_event_spikes). MEDIA_ERROR_UNSUPPORTED without a
 * picture the engine holds or without MEDIA_MODULE_TELEMETRY. Host mode; since API 10 the windowed engine too: its sensor is
 * the tracking grid (at most 480 x 272 cells: a 4K picture's 8 x 8 blocks), every picture's grid read back while it is on,
 * stabilised by the virtual IMU's motion between grids (the camera's motion read off the background), x, y in grid cells. */
media_status media_engine_set_event_camera(media_engine_t* engine, const media_event_camera_config_t* config);
/* The oldest `capacity` events at most, in time order; `count` says how many. MEDIA_ERROR_EMPTY: none. One consumer thread
 * (any, between open and close), as media_engine_poll_frame. */
media_status media_poll_event_spikes(media_engine_t* engine, media_event_spike_t* spikes, uint32_t capacity, uint32_t* count);
media_status media_query_event_camera(const media_engine_t* engine, media_event_camera_state_t* state);

/* ---- Bitstream perception (API 9; docs/sdk.md, "Bitstream perception"; docs/picture.md) ----
 * What a compressed H.264 or HEVC picture says about the scene before anything is decoded: its type, its QP, its cost (bits
 * per pixel at QP 26), the new detail it carries (its cost against the recent intra pictures'), bursts, and the probability
 * that it starts a new shot. Under a microsecond a picture, no engine needed (for a host scanning hours of recordings, or one
 * that decodes only when something happens: call it from its decode_video callback with the unit and its config). The
 * inspector is the caller's storage; no allocation. */
typedef struct media_bitstream_inspector { uint64_t opaque[1280]; } media_bitstream_inspector_t; /* 10 KiB. */
enum { MEDIA_PICTURE_TYPE_NONE = 0, MEDIA_PICTURE_TYPE_I = 1, MEDIA_PICTURE_TYPE_P = 2, MEDIA_PICTURE_TYPE_B = 3 };
enum {
    MEDIA_BITSTREAM_KEY = 1u,            /* An IDR (H.264) or IRAP (HEVC) picture. */
    MEDIA_BITSTREAM_PARAMETER_SETS = 2u, /* The unit carries parameter sets (taken). */
    MEDIA_BITSTREAM_SCENE_CUT = 4u,      /* scene_cut_probability >= 0.5. */
    MEDIA_BITSTREAM_MOTION_BURST = 8u,   /* Much more new detail than the recent pictures of its kind (burst >= 2), not a cut. */
    MEDIA_BITSTREAM_QP = 16u,            /* qp is known. */
    MEDIA_BITSTREAM_REVISION = 32u       /* revised_index and revised_scene_cut_probability are set. */
};
typedef struct media_bitstream_info {
    uint32_t struct_size;           /* sizeof(media_bitstream_info_t). */
    uint32_t index;                 /* Units inspected before this one. */
    uint32_t bytes, slices;         /* Its slices' NAL units. */
    uint32_t picture_type;          /* MEDIA_PICTURE_TYPE_* (B when any slice is B, else P when any is P). */
    uint32_t flags;                 /* MEDIA_BITSTREAM_*. */
    int32_t qp;                     /* The slices' mean QP (-1: unknown: a header this reader could not follow). */
    float complexity;               /* Bits per pixel, scaled to QP 26 when the QP is known. */
    float activity;                 /* Inter pictures: complexity over the recent intra pictures' (0 still .. 1 all new); -1 intra. */
    float burst;                    /* Complexity over the recent mean of its kind (P or B; 1: as usual). */
    float scene_cut_probability;    /* 0..1. */
    uint32_t width, height;         /* Its sequence's coded size (0: not known yet). */
    /* MEDIA_BITSTREAM_REVISION: an earlier inter picture's cut, judged by this next picture of its kind (P, B): a cut is followed
     * by pictures of the new shot as cheap as ever, a burst of new detail goes on costing. The earlier unit's index and its
     * probability now (0.95 or more: confirmed; under 0.3: withdrawn, a burst). */
    uint32_t revised_index;
    float revised_scene_cut_probability;
} media_bitstream_info_t;
/* `codec`: MEDIA_CODEC_H264 or MEDIA_CODEC_HEVC; `config`: the avcC / hvcC record (media_video_unit_t.config; null: parameter
 * sets in band); `nal_length_bytes`: 1, 2 or 4 (0: the record's). */
media_status media_bitstream_inspector_init(media_bitstream_inspector_t* inspector, uint32_t codec, const uint8_t* config, uint32_t config_bytes,
                                            uint32_t nal_length_bytes);
/* One access unit as stored (length-prefixed NAL units), in decode order. MEDIA_ERROR_EMPTY: no picture in it (parameter sets,
 * SEI); MEDIA_ERROR_INVALID_ARGUMENT: not whole NAL units. */
media_status media_inspect_bitstream_packet(media_bitstream_inspector_t* inspector, const uint8_t* data, uint32_t bytes, media_bitstream_info_t* info);

/* ---- Tensor export (API 9; docs/sdk.md, "Tensor export"; docs/picture.md) ----
 * A region of the picture shown last (the engine's software bridge, live sensors; host mode) as a model's input: NCHW (N = 1),
 * each channel a contiguous height x width plane in the caller's buffer, float32 or float16, ready for PyTorch, ONNX Runtime or
 * TensorRT without a copy. Each element is its cell of the region area-averaged on each plane, converted (BT.709, limited
 * range) to R'G'B' in [0, 1] and normalised. Written during the call, no allocation. */
enum { MEDIA_TENSOR_F32 = 1, MEDIA_TENSOR_F16 = 2 };
enum { MEDIA_TENSOR_UNIT = 0, MEDIA_TENSOR_IMAGENET = 1, MEDIA_TENSOR_SIGNED = 2 }; /* [0, 1]; (v - mean) / std; [-1, 1]. */
enum {
    MEDIA_TENSOR_FOVEA = 0,  /* The target (its object's box grown by a fifth) while tracking, else the view's fovea (half the view
                                across and down, steadied against the shake), shaped as the tensor. */
    MEDIA_TENSOR_VIEW = 1,   /* The view shown (zoom, pan, tracking, stabilisation), stretched to the tensor. */
    MEDIA_TENSOR_FULL = 2,   /* The whole picture, stretched to the tensor. */
    MEDIA_TENSOR_REGION = 3  /* left .. bottom: fractions of the picture. */
};
enum { MEDIA_TENSOR_BGR = 1u }; /* Channel planes B, G, R instead of R, G, B. */
typedef struct media_tensor_request {
    uint32_t struct_size;           /* sizeof(media_tensor_request_t). */
    uint32_t width, height;         /* The tensor's (1 .. 8192). */
    uint32_t channels;              /* 3: R G B (or B G R) planes; 1: luma (Y'). */
    uint32_t format;                /* MEDIA_TENSOR_F32 or MEDIA_TENSOR_F16. */
    uint32_t normalize;             /* MEDIA_TENSOR_UNIT, _IMAGENET (mean 0.485 0.456 0.406, std 0.229 0.224 0.225; grey 0.449, 0.226), _SIGNED. */
    uint32_t region;                /* MEDIA_TENSOR_FOVEA, _VIEW, _FULL, _REGION. */
    uint32_t flags;                 /* MEDIA_TENSOR_BGR. */
    float left, top, right, bottom; /* MEDIA_TENSOR_REGION. */
} media_tensor_request_t;
typedef struct media_tensor_info {
    uint32_t struct_size;           /* sizeof(media_tensor_info_t). */
    uint32_t region;                /* What was taken (MEDIA_TENSOR_*). */
    float left, top, right, bottom; /* The region, samples of the picture (fractional). */
    int64_t pts_100ns;              /* The picture's time. */
    uint64_t bytes;                 /* Written. */
    uint32_t target_track_id;       /* The object tracked when the fovea was the target (0: none). */
    uint32_t flags;                 /* API 11: MEDIA_TENSOR_TONE_MAPPED, MEDIA_TENSOR_HELD (reserved, 0, before). */
} media_tensor_info_t;
#define MEDIA_TENSOR_TONE_MAPPED 1u /* An HDR picture (PQ, HLG): the elements are what the screen shows, not coded values. */
#define MEDIA_TENSOR_HELD 2u        /* While paused: from the source held for the picture on screen, nothing decoded for it. */
/* 224 x 224, R G B, float32 in [0, 1], the fovea. */
void media_tensor_request_init(media_tensor_request_t* request);
/* The bytes a request's tensor takes (0: an invalid request). */
size_t media_tensor_bytes(const media_tensor_request_t* request);
/* MEDIA_ERROR_EMPTY: no picture yet; MEDIA_ERROR_UNSUPPORTED: the host's decoder holds the pictures; MEDIA_ERROR_STORAGE:
 * `bytes` too small. `info` may be null. Since API 10 the windowed engine too: the region of the next picture it converts, read
 * on the GPU from the decoded picture (each element's Y', Cb and Cr area means) and converted here; the call waits for it (at
 * most a second: MEDIA_ERROR_EMPTY before the first picture), at most 1024 x 1024 elements, one call at a time
 * (MEDIA_ERROR_INVALID_ARGUMENT for another while one waits). API 11: while paused, the picture on screen (MEDIA_TENSOR_HELD: from
 * its held source; else the call waits for one decode of it, whose source is held from then on, and once a tensor has been
 * exported each pause holds it from the start); a PQ or HLG picture's elements are tone-mapped as the screen shows them, in
 * [0, 1] (MEDIA_TENSOR_TONE_MAPPED); host mode's pictures are 8-bit and taken as BT.709. */
media_status media_export_foveated_tensor(media_engine_t* engine, const media_tensor_request_t* request, void* tensor, size_t bytes,
                                          media_tensor_info_t* info);

/* Defaults: every module, the "Avion" wake word, no voice input, visible, to the end. */
void media_engine_config_init(media_engine_config_t* config);
/* The bytes of arena memory an engine with this configuration commits when it opens media: the core (file blocks, demux
 * and codec tables, queues, audio) plus each enabled module's region. Exact: media_engine_stats reports this capacity. 0 for an
 * invalid configuration. */
uint64_t media_query_arena_budget(const media_engine_config_t* config);
/* One module's share (a single MEDIA_MODULE_* flag); MEDIA_ERROR_INVALID_ARGUMENT for anything else. */
media_status media_query_module_budget(uint32_t module, media_module_budget_t* budget);
/* Whether the built-in recogniser can hear `wake_word` (every word of it is in its vocabulary). A word it cannot hear
 * still works for media_engine_hear (a host's own speech recognition) and typed commands. */
int media_wake_word_hearable(const char* wake_word);
/* Since API 2 a custom name is hearable when its spelling can be read (Latin or Hebrew letters): media_wake_word_hearable
 * says so, and open synthesises its templates. */

/* Storage for one engine: at least this many bytes, aligned to media_engine_storage_alignment(). */
size_t media_engine_storage_bytes(void);
size_t media_engine_storage_alignment(void);
media_status media_engine_create(const media_engine_config_t* config, void* storage, size_t storage_bytes, media_engine_t** engine);
/* `path`: a UTF-8 local path or an http(s):// URL. */
media_status media_engine_open(media_engine_t* engine, const char* path);
/* API 15: open with options. MEDIA_PLAYBACK_WINDOW and an http(s):// URL: `http_headers` are sent with every range GET (a
 * cloud drive's "Authorization: Bearer ..."). With headers, redirects are not followed, so a token never reaches another
 * host; a redirect fails the open with its status as the code (also 401, 403, 404: media_engine_last_error). Range, If-Match
 * and Host are the engine's own and are refused (MEDIA_ERROR_INVALID_ARGUMENT). */
typedef struct media_open_options {
    uint32_t struct_size;        /* sizeof(media_open_options_t). */
    uint32_t reserved;           /* 0. */
    const char* http_headers;    /* UTF-8, "Name: value" lines separated by "\r\n", at most 4095 characters; null: none. */
} media_open_options_t;
media_status media_engine_open_ex(media_engine_t* engine, const char* path, const media_open_options_t* options);
/* API 15, any thread: the open in progress on `engine` stops at its next wait (the network probe, the container's metadata
 * reads) and returns MEDIA_ERROR_OPEN, stage 1, code E_ABORT (0x80004004). Nothing happens when no open is in progress: a later
 * open is not affected. A host that switches files calls this, then waits for the open call to return before the next open. */
void media_engine_cancel_open(media_engine_t* engine);
/* Plays until the end, maximum_seconds, the window closing or media_engine_stop; then returns. */
media_status media_engine_run(media_engine_t* engine);
/* Any thread: ends media_engine_run soon. */
void media_engine_stop(media_engine_t* engine);
media_status media_engine_close(media_engine_t* engine);
void media_engine_destroy(media_engine_t* engine);

/* A command in any of the nine languages (docs/intents.md): "pause", "audio zoom", "חשוך", "בטל", ... */
media_status media_engine_command(media_engine_t* engine, const char* utf8);
/* A recognised utterance from the host's own speech recognition, through the wake policy (name, window, push-to-talk). */
media_status media_engine_hear(media_engine_t* engine, const char* utf8);
/* MEDIA_WAKE_DISABLED: whether the host's talk button is held. */
void media_engine_set_talk(media_engine_t* engine, int pressed);
/* Changes the wake policy of an engine (between open and close too). */
media_status media_engine_set_wake(media_engine_t* engine, media_wake_policy policy, const char* wake_word);

/* The oldest frame signature not taken yet (MEDIA_MODULE_TELEMETRY); MEDIA_ERROR_EMPTY when there is none. One consumer. */
media_status media_engine_poll_frame(media_engine_t* engine, media_frame_signature_t* signature);
media_status media_engine_stats(const media_engine_t* engine, media_engine_stats_t* stats);
/* The last failure's engine stage and HRESULT-style code (0: none). The stages are listed in docs/sdk-reference.md. */
uint32_t media_engine_last_error(const media_engine_t* engine, int32_t* code);
/* API 15: the last failure in words, NUL-terminated in `text` (`bytes` at least 1): what the stage means and, after a failed
 * open, why the media was refused (media_probe's reason for it: "H.264 High 4:2:2 Intra, 4:2:2 10-bit: not decodable here",
 * "Matroska/WebM container: not supported (MP4 and MOV only)"). "" without a failure. MEDIA_ERROR_INVALID_ARGUMENT for a null
 * pointer or `bytes` 0; a text longer than `bytes` - 1 is cut at a UTF-8 character boundary. */
media_status media_engine_last_error_text(const media_engine_t* engine, char* text, size_t bytes);

/* ---- Probe (API 15): what a file holds and whether the engine can play it, without an engine, a window, a GPU or audio ----
 * Reads the container's metadata only (a few blocks; well under 100 ms for a two-hour MP4 on a local disk) through the
 * portable reader every mode uses. `playable`: media_engine_open in MEDIA_PLAYBACK_WINDOW is expected to succeed: a video
 * track whose codec, profile, chroma format, bit depth and scan the engine decodes (H.264 progressive 4:2:0 8-bit; HEVC Main
 * and Main 10, 4:2:0), or no video and AAC sound (music mode). The GPU's decoder can still refuse a size or level it lacks:
 * open says so. Any thread; no state is kept; it reserves its working memory (28 MiB of pages, most never touched) and frees it. */
#define MEDIA_PROBE_MAX_TRACKS 16u
typedef enum media_probe_container {
    MEDIA_CONTAINER_UNKNOWN = 0, MEDIA_CONTAINER_MP4 = 1, /* ISO BMFF: .mp4, .m4v, .m4a, .mov (QuickTime), .3gp */
    MEDIA_CONTAINER_MATROSKA = 2, /* .mkv, .webm */ MEDIA_CONTAINER_AVI = 3, MEDIA_CONTAINER_MPEG_TS = 4, MEDIA_CONTAINER_MPEG_PS = 5,
    MEDIA_CONTAINER_FLV = 6, MEDIA_CONTAINER_ASF = 7, /* .wmv, .wma */ MEDIA_CONTAINER_MP3 = 8, MEDIA_CONTAINER_FLAC = 9,
    MEDIA_CONTAINER_OGG = 10, MEDIA_CONTAINER_WAV = 11
} media_probe_container;
typedef enum media_track_kind { MEDIA_TRACK_OTHER = 0, MEDIA_TRACK_VIDEO = 1, MEDIA_TRACK_AUDIO = 2, MEDIA_TRACK_SUBTITLE = 3 } media_track_kind;
typedef struct media_probe_track_t {
    uint32_t id;                  /* The container's track id. */
    uint32_t kind;                /* media_track_kind */
    char codec[8];                /* The sample entry's fourcc as text ("avc1", "hvc1", "av01", "mp4a", "ac-3", "tx3g"...). */
    char description[64];         /* "H.264 High, 4:2:0 8-bit, progressive", "AAC, 2 ch, 48000 Hz", "timed text". */
    uint32_t supported;           /* 1: the engine decodes this track. */
    uint32_t width, height;       /* Video: the coded picture (sample entry). */
    uint32_t channels, sample_rate;
    int64_t duration_100ns;       /* 0: not declared. */
    char language[4];             /* ISO 639-2 ("und": none). */
    char reason[124];             /* Not supported: why, in English (""  when supported). */
} media_probe_track_t;
typedef struct media_probe_t {
    uint32_t struct_size;         /* sizeof(media_probe_t): set by the caller. */
    uint32_t container;           /* media_probe_container */
    uint32_t playable;            /* 1: media_engine_open is expected to play it (above). */
    uint32_t track_count;         /* Tracks described (at most MEDIA_PROBE_MAX_TRACKS). */
    int64_t duration_100ns;       /* The longest track's; 0 when unknown. */
    char reason[160];             /* Not playable: why; playable with limits: what is missing ("no sound: AC-3 is not decoded"). */
    media_probe_track_t tracks[MEDIA_PROBE_MAX_TRACKS];
} media_probe_t;
/* MEDIA_OK when the file was read (playable or not: see `playable`); MEDIA_ERROR_OPEN when it could not be read at all (missing,
 * unreadable: `reason` says); MEDIA_ERROR_INVALID_ARGUMENT for null pointers or a wrong struct_size. `path`: UTF-8, local. */
media_status media_probe(const char* path, media_probe_t* info);

/* ---- Host mode (MEDIA_PLAYBACK_HOST) ---- */
/* One pass: reads and decodes ahead, follows the clock, presents the picture due (on_present, then on_frame), runs the
 * commands spoken since the last pass. `now_100ns`: the host's monotonic clock (any origin, never going back). MEDIA_OK while
 * playing; MEDIA_ERROR_EMPTY at the end (or maximum_seconds); MEDIA_ERROR_PLAYBACK when it failed. */
media_status media_engine_step(media_engine_t* engine, int64_t now_100ns);
/* The host's audio callback: `frames` interleaved float frames (audio_channels at audio_rate) that will be heard from
 * `now_100ns` on (the host's clock, its output latency included). Silence while paused or starved. */
media_status media_engine_pump_audio(media_engine_t* engine, float* out, uint32_t frames, int64_t now_100ns);
/* MEDIA_VOICE_HOST: the host's microphone, 16 kHz mono, any number of samples; recognised commands run at the next step. */
media_status media_engine_feed_voice(media_engine_t* engine, const float* samples, uint32_t frames);
media_status media_engine_state(const media_engine_t* engine, media_playback_state_t* state);

/* ---- Embedding: commands from the host's thread, events back (API 12; docs/sdk.md, "Embedding: API 12") ----
 * For a host that owns the interface: its buttons and sliders post commands, and it learns what happened from events, without
 * waiting on the engine's thread. Both are single-producer, single-consumer lock-free rings in the engine's storage: no lock, no
 * allocation. A command posted while paused is applied at once (the engine's thread is woken), not at the next picture. */
typedef enum media_event_kind {
    MEDIA_EVENT_OPENED = 1,              /* Playback started: duration_100ns, width x height (the picture as displayed). */
    MEDIA_EVENT_STATE = 2,               /* paused, muted, volume or speed changed (all four are set on every event). */
    MEDIA_EVENT_POSITION = 3,            /* Ten times a second of playing (four before API 15), and once on pausing: position_100ns. */
    MEDIA_EVENT_SEEK_LANDED = 4,         /* A seek's first picture is on screen: position_100ns (its pts), latency_100ns. */
    MEDIA_EVENT_COMMAND = 5,             /* A posted command ran: command_id (the host's), status (MEDIA_OK or MEDIA_ERROR_REFUSED). */
    MEDIA_EVENT_FULLSCREEN_REQUEST = 6,  /* parent_window: the viewer asked for fullscreen (F, F11, Alt+Enter); the host decides. */
    MEDIA_EVENT_ENDED = 7                /* media_engine_run is returning: reason (MEDIA_ENDED_*), status (its media_status). */
} media_event_kind;
enum { MEDIA_ENDED_MEDIA = 0, MEDIA_ENDED_STOPPED = 1, MEDIA_ENDED_CLOSED = 2, MEDIA_ENDED_ERROR = 3, MEDIA_ENDED_LIMIT = 4 };
typedef struct media_event {
    uint32_t struct_size;               /* sizeof(media_event_t). */
    uint32_t kind;                      /* media_event_kind. */
    int64_t time_100ns;                 /* When it happened (the engine's monotonic clock, QueryPerformanceCounter). */
    int64_t position_100ns, duration_100ns;
    int64_t latency_100ns;              /* MEDIA_EVENT_SEEK_LANDED: from the request to the picture on screen. */
    uint32_t command_id;                /* MEDIA_EVENT_COMMAND: the id given to media_engine_post_command. */
    int32_t status;                     /* MEDIA_EVENT_COMMAND, MEDIA_EVENT_ENDED: a media_status. */
    int32_t paused, muted;
    float volume, speed;                /* volume 0..1 (the output gain); speed 1: as recorded. */
    uint32_t width, height;             /* MEDIA_EVENT_OPENED. */
    uint32_t reason;                    /* MEDIA_EVENT_ENDED: MEDIA_ENDED_*. */
    uint32_t lost;                      /* Events dropped before this one because the host had not polled (the ring holds 256). */
} media_event_t;
/* MEDIA_API_VERSION of the library loaded (a binding checks it against the header it was written for). */
uint32_t media_api_version(void);
/* Queues `utf8` (a command of the grammar, at most 231 bytes; 247 before API 15) for the engine's thread; `command_id` comes back in its
 * MEDIA_EVENT_COMMAND. One producer thread at a time (the host's UI thread), between open and close. MEDIA_ERROR_REFUSED:
 * the queue (32) is full. MEDIA_PLAYBACK_WINDOW: the engine's thread runs it in its next pass (woken now). Host mode: the
 * next media_engine_step runs it, and its MEDIA_EVENT_COMMAND is host mode's only event (the host steps: it knows the rest). */
media_status media_engine_post_command(media_engine_t* engine, const char* utf8, uint32_t command_id);
/* API 15: structured controls, for a host's buttons and sliders (no text to build or parse). Same ring, same thread rules and the
 * same MEDIA_EVENT_COMMAND (status MEDIA_OK, or MEDIA_ERROR_REFUSED: not seekable, the speed refused, no picture to step from)
 * as media_engine_post_command; they run exactly what the matching command runs. A value outside its range is refused at once
 * (MEDIA_ERROR_INVALID_ARGUMENT), nothing is queued. */
typedef enum media_control {
    MEDIA_CONTROL_SEEK = 1,          /* value: the position, seconds (>= 0); past the end lands on the last picture. */
    MEDIA_CONTROL_PAUSE = 2,         /* value: 1 pause, 0 play. */
    MEDIA_CONTROL_VOLUME = 3,        /* value: 0..2: 0..1 a 60 dB perceptual taper (1 = as recorded), 1..2 a boost to x2 (API 16) through a soft limiter that never hard-clips; unmutes. */
    MEDIA_CONTROL_MUTE = 4,          /* value: 1 mute, 0 unmute. */
    MEDIA_CONTROL_SPEED = 5,         /* value: 0.25..4 (2 before API 16), pitch kept. */
    MEDIA_CONTROL_STEP_FRAME = 6,    /* value: +1 the next picture, -1 the previous (pauses first). Windowed engine only. */
    MEDIA_CONTROL_SEEK_RELATIVE = 7, /* value: seconds from the position now (negative: back); clamped to the media. */
    MEDIA_CONTROL_AUDIO_TRACK = 8,   /* API 16 (host mode since API 17): value: a selectable audio track's id (media_engine_track_info), spliced in. */
    MEDIA_CONTROL_SUBTITLE_TRACK = 9 /* API 16 (host mode since API 17): value: a subtitle track's id, 0 the sidecar, -1 none. */
} media_control;
media_status media_engine_post_control(media_engine_t* engine, uint32_t control, double value, uint32_t command_id);
/* ---- Tracks (API 16; docs/sdk-reference.md, "Tracks") ----
 * Every track of the file (refused ones too, with selectable 0) and a subtitle sidecar (id 0), fixed from open to close: any
 * thread may list them. `active`: the audio track playing and the subtitle track shown, as the engine last published them (it
 * changes after a MEDIA_CONTROL_AUDIO_TRACK / _SUBTITLE_TRACK has run). Both modes since API 17 (host mode: the audio tracks it
 * plays, PCM or through the host's decoder; the file's own subtitle tracks, MP4 timed text and Matroska text/ASS; a live sensor
 * has none). `codec` for Matroska tracks: their MP4 name ("avc1", "hvc1", "av01", "mp4a", "Opus"; "text", "ssa" for subtitles). */
#define MEDIA_MAX_TRACKS 17u
typedef struct media_track_info {
    uint32_t struct_size;        /* sizeof(media_track_info_t). */
    uint32_t id;                 /* The container's track id (the sidecar: 0); MEDIA_CONTROL_*_TRACK takes it. */
    uint32_t kind;               /* media_track_kind */
    char codec[8];               /* The sample entry's fourcc as text ("avc1", "mp4a", "ac-3", "tx3g"; "srt" for the sidecar). */
    char language[4];            /* ISO 639-2/T, "" when the file does not say. */
    char title[32];              /* The file's label for it (UTF-8), "" when it has none. */
    uint32_t channels, sample_rate, width, height;
    uint32_t selectable;         /* 1: it can be chosen now. 0: refused, undecodable, another rate than the endpoint's, chapters. */
    uint32_t active;             /* 1: playing (audio) or shown (subtitles); the video track that plays. */
    uint32_t is_default;         /* The file's default among its alternatives. */
    uint32_t reserved;
} media_track_info_t;
media_status media_engine_track_count(const media_engine_t* engine, uint32_t* count);
media_status media_engine_track_info(const media_engine_t* engine, uint32_t index, media_track_info_t* info);
/* API 17: subtitles after open. A .srt or .vtt file (UTF-8 path, at most 4 MiB) becomes the sidecar (track id 0) and is shown at
 * once, the file's own subtitle tracks kept; a later call replaces it. The text is parsed into memory the engine reserved at
 * open and swapped in; nothing is allocated and the pictures keep coming. Host mode: on the host's thread, in the call. The
 * windowed engine: parsed by the caller's thread, swapped in by the window's (waited for, at most 3 s). Needs the subtitles
 * module (media_engine_config_t.modules). MEDIA_ERROR_OPEN: the file cannot be read or holds no cue (the subtitles shown are
 * unchanged); MEDIA_ERROR_UNSUPPORTED: no subtitles module. */
media_status media_engine_attach_subtitles(media_engine_t* engine, const char* path);
/* The oldest event not taken yet; MEDIA_ERROR_EMPTY when there is none. One consumer thread; readable from open until the
 * next open or destroy (after close too: MEDIA_EVENT_ENDED is the last). */
media_status media_engine_poll_event(media_engine_t* engine, media_event_t* event);
/* ---- Texture output (API 13; docs/sdk.md, "Texture output: API 13") ----
 * The engine draws each picture into a free one of three textures (R8G8B8A8_UNORM, or since API 14 output_format's 10-bit PQ or FP16
 * scRGB; the common state between frames; host mode's are also simultaneous-access UAVs) and
 * signals `fence` with the frame's value; the host's renderer opens the textures and both fences once per generation
 * (ID3D11Device1::OpenSharedResource1 / ID3D11Device5::OpenSharedFence, or ID3D12Device::OpenSharedHandle) on the adapter
 * named here, then every frame: acquire the newest, make its GPU wait for fence_value (ID3D11DeviceContext4::Wait, or
 * ID3D12CommandQueue::Wait), draw with the texture, signal host_fence with a value of its own, and release the frame with that
 * value; the engine's queue waits for it (on the GPU) before drawing into that texture again. Nothing waits on a CPU, there
 * is no lock, and a texture is never drawn into while the host holds it (hold one frame at a time: with two held, the engine
 * has nowhere to draw and waits). The handles stay the engine's: they are valid until the generation changes (a resize, a
 * device recovery), when the host opens the new ones. One host thread calls these four. */
typedef struct media_output_textures {
    uint32_t struct_size;               /* sizeof(media_output_textures_t). */
    uint32_t count;                     /* 3. */
    uint32_t width, height;
    uint32_t format;                    /* DXGI_FORMAT_R8G8B8A8_UNORM (28). */
    uint32_t generation;                /* Changes when the textures are made again: open the new ones. */
    void* textures[3];                  /* NT handles of the shared textures. */
    void* fence;                        /* NT handle of the engine's fence: a frame is drawn when it reaches its fence_value. */
    void* host_fence;                   /* NT handle of the host's fence: signal it, and release a frame with the value. */
    uint32_t adapter_luid_low;          /* The adapter (LUID) to open them on: the host's device must be on the same one. */
    int32_t adapter_luid_high;
    /* API 14 (API 13's struct, 72 bytes on x64, is still accepted). */
    uint32_t color_space;               /* DXGI_COLOR_SPACE_TYPE of the pixels: 0 (sRGB-like G2.2 BT.709), 12 (PQ BT.2020), 1 (scRGB). */
    float sdr_white_nits;               /* HDR formats: where an SDR picture's white is (203 cd/m2); 0 for 8-bit. */
} media_output_textures_t;
typedef struct media_output_frame {
    uint32_t struct_size;               /* sizeof(media_output_frame_t). */
    uint32_t index;                     /* Into media_output_textures_t.textures. */
    uint64_t fence_value;               /* Wait for the engine's fence to reach it before reading the texture. */
    int64_t pts_100ns;                  /* The media time of the picture drawn. */
    uint32_t generation;                /* The textures' (another than the host opened: query and open them again). */
    uint32_t sequence;                  /* Frames drawn since open. */
    /* API 14 (API 13's struct, 32 bytes, is still accepted). */
    uint32_t transfer;                  /* The picture's H.273 transfer characteristics: 1 (BT.709), 16 (PQ), 18 (HLG), ... (0: unknown). */
    uint32_t reserved;
} media_output_frame_t;
/* MEDIA_ERROR_STATE: not open, or no texture output; MEDIA_ERROR_EMPTY: a resize under way (ask again). */
media_status media_engine_query_output_textures(media_engine_t* engine, media_output_textures_t* textures);
/* The newest frame drawn, now held by the host. MEDIA_ERROR_EMPTY: none yet, or none newer than the one held. */
media_status media_engine_acquire_output_frame(media_engine_t* engine, media_output_frame_t* frame);
/* Done with frame `index`: the engine waits for host_fence to reach `host_fence_value` (0: no wait) before drawing into it. */
media_status media_engine_release_output_frame(media_engine_t* engine, uint32_t index, uint64_t host_fence_value);
/* Draw at this size from the next picture (16..8192): new textures, a new generation. */
media_status media_engine_resize_output(media_engine_t* engine, uint32_t width, uint32_t height);

/* ---- API 14 (docs/sdk.md, "API 14") ----
 * Who decodes the open media's video: MEDIA_DECODER_HARDWARE (the GPU: the windowed engine, and host mode with texture output
 * when the GPU took the stream), MEDIA_DECODER_HOST (the host's decode_video), MEDIA_DECODER_SOFTWARE (the engine's software
 * bridge), MEDIA_DECODER_NONE (no video, live sensors, not open). */
enum { MEDIA_DECODER_NONE = 0u, MEDIA_DECODER_SOFTWARE = 1u, MEDIA_DECODER_HOST = 2u, MEDIA_DECODER_HARDWARE = 3u };
uint32_t media_query_video_decoder(const media_engine_t* engine);

/* Since API 12 media_engine_state answers for MEDIA_PLAYBACK_WINDOW too, from any thread: position, duration, paused, muted,
 * volume, speed, presented and dropped (the rest 0), as of the engine's last pass. */

#ifdef __cplusplus
}
#endif
#endif
