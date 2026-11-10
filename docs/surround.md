# Multichannel AAC: 5.1 Playback, Stereo Fold-Down and the 7.1 Limit

5.1 AAC-LC now plays end to end: container, `AudioSpecificConfig` and PCE parsing, native decoding, a negotiated `WAVEFORMATEXTENSIBLE` layout, DSP, and the WASAPI endpoint. A surround endpoint receives all six channels. A stereo endpoint (headphones, laptop speakers) gets the programme folded to two channels by the DSP: binaural rendering by default, or an ITU downmix, with a seamless toggle.

**7.1 AAC cannot be decoded with native Windows components on this system.** It is recognised, reported and dropped; the video still plays (see [the 7.1 limit](#the-71-limit)).

```text
esds DecoderSpecificInfo ─► codec::ParseAacConfig (AOT, rate, channelConfiguration, PCE ─► speaker mask)
   ─► Win32AacDecoder: whole ASC to the Microsoft AAC MFT ─► float PCM + channel mask ─► codec::MapToOutput (1, 2 or 5.1)
   ─► routing: endpoint mix format (QueryMix) + SurroundOutput
        surround endpoint / Passthrough ─► DSP in place on 6 channels ─► PCM ring (6 ch, mask 0x3F) ─► WASAPI
        stereo endpoint / Stereo        ─► scratch (6 ch) ─► DSP fold-down ─► channels 0/1 ─► PCM ring (2 ch) ─► WASAPI
```

## Configuration parsing (`media::codec`, portable)

The demuxer already hands over the whole `esds` `DecoderSpecificInfo`. `ParseAacConfig` decodes it without allocating:
- the object type, with the 31 escape;
- the sampling-frequency index, or the explicit 24-bit rate;
- `channelConfiguration`;
- `GASpecificConfig`: `frameLengthFlag`, `dependsOnCoreCoder` and `extensionFlag`;
- for configuration 0, the `program_config_element`: front, side, back, LFE, associated-data and coupling elements, the mixdown fields, byte alignment relative to the ASC, and the comment.

It returns a status and the speaker mask:

| Configuration | Speakers (mask) |
|---|---|
| 1, 2 | FC (`0x4`); FL FR (`0x3`) |
| 3, 4, 5 | + FC (`0x7`); + BC (`0x107`); FC + back pair (`0x37`) |
| 6 | 5.1: FL FR FC LFE BL BR (`0x3F`) |
| 7 | 7.1 wide (`0xFF`) |
| 11 | 6.1 (`0x70F`) |
| 12 | 7.1: + SL SR (`0x63F`) |
| PCE | Front: SCE → FC, CPEs → FL/FR, then FLC/FRC. Side CPE → SL/SR. Back CPE → BL/BR, back SCE → BC. LFE → LFE |

Results other than a speaker mask:
- **`Unsupported`:** other object types (HE-AAC 5/29, USAC, …), 960-sample frames, core coders, configurations 13 and 14, coupling channels, and PCE layouts without a WAVEFORMATEXTENSIBLE assignment.
- **`Invalid`:** reserved values.
- **`Truncated`:** the bytes end early.

`MapToOutput(mask)` places decoded channels on the engine's output layouts:
- mono and stereo pass through;
- 3- to 6-channel programmes land on 5.1: missing speakers stay silent, side surrounds take the surround pair, and a back centre is split to both surrounds at −3 dB;
- anything with more than six channels, or with front-of-centre or top speakers, is refused.

## Decoding (`Win32AacDecoder`)

- **Input type.** The MFT receives the full ASC, up to 64 bytes and including a PCE, after the 12-byte `HEAACWAVEINFO` tail.
- **Output type.** The float type with the programme's channel count is chosen. Its `MF_MT_AUDIO_CHANNEL_MASK` (for example `0x3F` for 5.1, `0x37` for 5.0) drives `MapToOutput`; mono and stereo types carry no mask.
- **Order.** PCM arrives in WAVEFORMATEXTENSIBLE order.
- **Measured.** With a distinct tone per speaker, the native 5.1 encode decodes each tone in its own channel (FL 300, FR 500, FC 700, LFE 60, BL 1100, BR 1300 Hz), at the encoded level 0.122. The same access units described by a PCE decode identically.
- **Refusals.** More than `kPlatformChannels` (6), HE-AAC and other unsupported configurations fail `Prepare` with `kUnsupportedLayout` (`0xA0000AAC`).
- **Memory.** Steady-state decoding allocates nothing. The output buffer (64 KiB) holds a 6-channel frame (24 KiB).

## Routing (`PlaybackCoordinator`)

`Prepare` parses the ASC first. A track that cannot be decoded here becomes `AudioStatus::UnsupportedLayout`:
- its packets are returned undecoded, and the endpoint's silence clocks the video;
- `Routing()` reports the source layout.

This replaces the previous fatal stage 9 for such files.

The worker prepares the decoder before the endpoint, then reads the endpoint's shared mix format (`Win32WasapiAudioSink::QueryMix`) and applies `PlaybackOptions::surround`:

| `SurroundOutput` | Endpoint | Output |
|---|---|---|
| `Auto` (default) | Mix format has every speaker of the programme | All channels, mask `0x3F` |
| `Auto` | Stereo (or smaller) mix | Fold-down to 2 channels, binaural on |
| `Passthrough` | Any | All channels. If the shared engine refuses the format, the stream opens with `AUTOCONVERTPCM` and Windows converts it (`Routing().converting`) |
| `Stereo` | Any | Fold-down to 2 channels, binaural on |

- **Masks.** The sink's default masks are `0x3F` for 6 channels and `0x63F` for 8.
- **Fold-down path.** The fold-down reads each decoded span (at most 1024 frames) into an arena scratch buffer sized for 8 channels, runs the DSP, and copies channels 0 and 1 into the 2-channel ring span. The end-of-stream silence goes through the same path, so filter and delay tails finish.
- **Memory.** Nothing is allocated after `Prepare`.
- **Controls.** S toggles binaural rendering in either mode:
  - with fold-down: binaural ↔ ITU downmix;
  - with passthrough: ears in channels 0/1 with the others faded ↔ untouched 5.1.
- **Demo.** `--surround auto|passthrough|stereo` selects the mode, and the demo prints the routing:

```text
Audio: ready; source 6 ch mask 03f -> decoded 6 ch mask 03f; endpoint 2 ch mask 003 -> output 2 ch (DSP fold-down)
```

## Fold-down and binaural rendering (`DspProcessor`)

`Configure(rate, channels, fold_down = true)`, for 6 or 8 channels, renders the programme into channels 0 and 1 and keeps the others at exact zero:
- **Spatial off: an ITU-R BS.775 downmix.** L = FL + FC/√2 + left surround(s)/√2, R likewise, LFE omitted. It is scaled by 0.5, the binaural renderer's per-ear weight, so a centred voice has the same level in both modes.
- **Spatial on: the existing Brown–Duda binaural render.** Virtual speakers sit at ±30° (fronts), 0° (centre), ±110° (5.1 surrounds), and ±90°/±150° (7.1 sides/backs), with Woodworth ITDs, head-shadow ILDs and a rear pinna low-pass.
- **Toggle.** A 30 ms crossfade between the two.
- **Never a passthrough.** A fold-down processor always runs, so the soft clipper also bounds a full-scale coherent downmix.

## Verification

| Test | Checks |
|---|---|
| `media_aac_config_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **Real encoder configurations:** `11 90`, `11 88`, `11 B0`, and HE-AAC `2B 39 88 00` → Unsupported.<br>**Configurations 5, 6, 7, 11, 12:** the masks above.<br>**Encoding variants:** explicit rate, USAC escape, 960-sample frames, core coder.<br>**Reserved or invalid fields:** reserved index, reserved configuration, `extensionFlag`, 22.2.<br>**PCEs:**<br>• 5.1 accepted by the platform decoder, and 7.1<br>• side 5.1, 7.1 wide, back centre, stereo, mono<br>• unrepresentable layouts, coupling, two LFEs, an empty programme<br>• every truncation of a PCE<br>**`MapToOutput`:** identity layouts, 5.0 with a silent LFE, side 5.1, 4.0 with the −3 dB back-centre split, refusals, and power preservation for every mask up to `0xFFF`.<br>**Exhaustive:** all 1-, 2- and 3-byte inputs.<br>**Allocations:** 0 |
| `media_audio_dsp_tests` (MSVC, GCC SSE and forced scalar, ASan/UBSan) | **Weights:** exact ITU fold-down weights for every 5.1/7.1 channel, with channels 2+ exactly zero.<br>**Binaural:** fold-down binaural equals the verified render (±30/0/±110/±150/±90°, delays and head shadow checked by `Spatial()`) within 1e-7 for an impulse on every speaker.<br>**Toggles:** crossfades in fold-down and passthrough never step more than the steady states.<br>**Clipping:** full-scale coherent 5.1/7.1 stays below 1.<br>**Allocations:** 0 |
| `media_aac_surround_tests` | **Native 5.1:** 6 channels, mask `0x3F`, per-speaker tones in their own channels at 0.122, 0 steady-state allocations.<br>**PCE-described 5.1** on the same access units: identical.<br>**5.0:** mask `0x37` mapped to 5.1, constructed frames decoded frame for frame.<br>**Refused:** 7.1 (configuration 12, and a PCE) and HE-AAC, with `kUnsupportedLayout` |
| `media_playback_surround_{auto,passthrough,stereo}_test` | Real H.264 + native 5.1 AAC fixture (`--surround51`):<br>• 72/72 frames, 144,384 PCM frames, 0 drops, 0 allocations<br>• routing source/decoded 6 ch `0x3F`, output 2 ch fold-down (auto, stereo) or 6 ch (passthrough)<br>• three live S-key toggles; the DSP processed every frame in fold-down |
| `media_playback_surround71_test` | H.264 + 7.1 AAC-LC fixture (`--surround71`, PCE `0x63F`): `UnsupportedLayout`, 72/72 frames, 0 drops, no PCM |

On this machine the default endpoint's mix format is stereo (`0x3`). Auto therefore folds down, and the forced-passthrough test opens a 6-channel stream that the Windows audio engine accepts and downmixes itself. A real 5.1 or 7.1 endpoint was not available.

## The 7.1 limit

Measured with a probe on Windows 11 25H2 (10.0.26200). `MFTEnumEx` lists one AAC decoder, the Microsoft AAC Audio Decoder MFT.
- **Accepted by the decoder:** AAC-LC with channel configurations 1–6, and PCE-described layouts of up to 6 channels.
- **Rejected by the decoder:** every 8-channel input type, with `MF_E_INVALIDMEDIATYPE` (`0xC00D36B4`): configuration 7, configuration 12, a 7.1 PCE, the ADTS payload type, `MFAudioFormat_ADTS`, and an explicit `0x63F` mask.
- **The encoder:** it produces 1, 2 or 6 channels of AAC-LC. Asked for 8 channels, it switches to HE-AAC (ASC `2B 39 88 00`); an explicit AAC-LC request for 8 channels is refused.

Two workarounds were tested and fail:
- **Split decoding by element tags.** One instance takes the 5.1 subset, a second the side pair. The MFT matches elements by PCE tag, but drops any access unit containing an element its PCE does not list.
- **Splitting access units into elements.** The encoder's frames sometimes carry non-zero bits after `ID_END`, so element boundaries cannot be found without decoding the Huffman-coded side information and spectra. That is a full AAC syntax parser, whose ISO codebook tables are not available here.

So:
- 7.1 AAC files are recognised (PCE or configuration 12 → 8 channels, `0x63F`), reported as `UnsupportedLayout`, and play video-only.
- **Below the decoder, every stage handles 8 channels:** configuration parsing, the sink's 7.1 mask, the 8-channel PCM ring, and the DSP's 7.1 binaural render and fold-down, which the tests cover with synthetic 7.1 PCM.
- The 7.1 fixture is built from silent but valid AAC-LC elements (`ONLY_LONG`, `max_sfb` 0). The same construction decodes frame for frame on the platform decoder in its 5.0 form.

## Open

- **7.1 decode** needs either a platform decoder that accepts 8 channels, or an in-house AAC-LC decoder (Huffman codebooks, inverse quantisation, M/S, TNS, IMDCT). Once one exists, a decoded 8-channel mask maps straight onto the 7.1 layout the DSP and sink already support.
- **Not verified on surround hardware.** No 5.1 or 7.1 endpoint was available; the multichannel sink path ran through Windows' own downmix on a stereo endpoint.
- **Other codecs.** HE-AAC, E-AC-3, DTS and multichannel Opus are not decoded.
- **Downmix.** The ITU downmix omits the LFE; the binaural render includes it at 0.25 per ear.
