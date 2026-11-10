#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/linux
BASE="src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp tests/allocation_probe.cpp"
W="-std=c++20 -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -pthread -Iinclude -Itests"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
build() {
  name=$1; shift
  g++ $W -O2 "$@" $BASE -o build/linux/${name}_o2
  g++ $W $SAN "$@" $BASE -o build/linux/${name}_asan
  echo "== $name -O2"; ./build/linux/${name}_o2 | tail -n 1
  echo "== $name ASan/UBSan"; ./build/linux/${name}_asan | tail -n 1
}
build hevc_codec tests/hevc_codec_tests.cpp src/codec/HevcSyntaxParser.cpp src/codec/Rbsp.cpp
build h264_codec tests/codec_tests.cpp src/codec/H264SyntaxParser.cpp src/codec/Rbsp.cpp
build demux tests/demux_tests.cpp src/demux/IsobmffParser.cpp
build matroska tests/matroska_tests.cpp src/demux/MatroskaReader.cpp
build matroska_demux tests/matroska_demux_tests.cpp src/demux/MatroskaDemuxer.cpp src/demux/Demuxer.cpp src/demux/IsobmffParser.cpp src/demux/MatroskaReader.cpp
build picture_geometry tests/picture_geometry_tests.cpp
build longform_demux tests/longform_demux_tests.cpp src/demux/IsobmffParser.cpp
build multitrack_demux tests/multitrack_demux_tests.cpp src/demux/IsobmffParser.cpp
build chapter_scene tests/chapter_scene_tests.cpp src/demux/IsobmffParser.cpp src/timeline/ChapterTimeline.cpp src/ui/KineticHud.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build timing tests/timing_tests.cpp src/clock/PredictiveMasterClock.cpp src/audio/PcmRingBuffer.cpp
build hud tests/hud_tests.cpp src/ui/KineticHud.cpp
build hud_status tests/hud_status_tests.cpp src/ui/StatusCue.cpp src/ui/KineticHud.cpp src/text/Utf8.cpp
build command_palette tests/command_palette_tests.cpp src/ui/CommandPalette.cpp src/ui/KineticHud.cpp src/text/Utf8.cpp
build subtitle tests/subtitle_tests.cpp src/subtitle/SubtitleTrack.cpp src/text/Utf8.cpp
build subtitle_search tests/subtitle_search_tests.cpp src/subtitle/SubtitleSearch.cpp src/subtitle/SubtitleTrack.cpp src/text/Utf8.cpp
build intent_dispatcher tests/intent_dispatcher_tests.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build semantic_search tests/semantic_search_tests.cpp src/ai/ConceptSearch.cpp src/subtitle/SubtitleSearch.cpp src/subtitle/SubtitleTrack.cpp src/text/Utf8.cpp
build session tests/session_tests.cpp src/session/MediaSession.cpp
build bookmark_compare tests/bookmark_compare_tests.cpp src/session/MediaSession.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build aac_config tests/aac_config_tests.cpp src/codec/AacConfig.cpp
build neural tests/neural_reference_tests.cpp src/video/NeuralUpscale.cpp
build quality_governor tests/quality_governor_tests.cpp src/render/QualityGovernor.cpp
build audio_dsp tests/audio_dsp_tests.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp
build pcm_converter tests/pcm_converter_tests.cpp src/audio/PcmConverter.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp
build time_stretch tests/time_stretch_tests.cpp src/audio/TimeStretcher.cpp
build spectrum tests/spectrum_tests.cpp src/audio/SpectrumAnalyser.cpp
build loudness tests/loudness_tests.cpp src/audio/Loudness.cpp
build ab_loop tests/ab_loop_tests.cpp src/timeline/LoopRegion.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build telemetry_explain tests/telemetry_explain_tests.cpp src/ui/TelemetryReadout.cpp src/render/QualityGovernor.cpp
build formant_synth tests/formant_synth_tests.cpp src/ai/FormantSynthesizer.cpp
build custom_wake tests/custom_wake_tests.cpp src/ai/CustomWakeWord.cpp src/ai/AvionWakeGate.cpp src/ai/AcousticIntentRecognizer.cpp src/ai/FormantSynthesizer.cpp src/ai/LocalIntentDispatcher.cpp src/audio/MicroVad.cpp src/audio/FilterbankFeatures.cpp src/text/Utf8.cpp
build target_tracking tests/target_tracking_tests.cpp src/video/TargetTracker.cpp src/ai/LocalIntentDispatcher.cpp src/audio/MasteringEq.cpp src/text/Utf8.cpp
build perception tests/perception_tests.cpp src/video/Perception.cpp src/video/Stabilizer.cpp src/audio/SoundLocator.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build perception_scalar -DMEDIA_PERCEPTION_SCALAR tests/perception_tests.cpp src/video/Perception.cpp src/video/Stabilizer.cpp src/audio/SoundLocator.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build rotation tests/rotation_tests.cpp src/video/Orientation.cpp src/demux/IsobmffParser.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build auto_horizon tests/auto_horizon_tests.cpp src/video/HorizonLevel.cpp src/video/Matte.cpp
build auto_horizon_scalar -DMEDIA_SURVEY_SCALAR tests/auto_horizon_tests.cpp src/video/HorizonLevel.cpp src/video/Matte.cpp
build avion_wake tests/avion_wake_tests.cpp src/ai/AvionWakeGate.cpp src/ai/AcousticIntentRecognizer.cpp src/ai/FormantSynthesizer.cpp src/ai/LocalIntentDispatcher.cpp src/audio/MicroVad.cpp src/audio/FilterbankFeatures.cpp src/text/Utf8.cpp
build vad tests/vad_tests.cpp src/audio/MicroVad.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build acoustic_intent tests/acoustic_intent_tests.cpp src/audio/MicroVad.cpp src/audio/FilterbankFeatures.cpp src/ai/AcousticIntentRecognizer.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp
build audio_dsp_scalar -DMEDIA_DSP_FORCE_SCALAR tests/audio_dsp_tests.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp
build eq_picture tests/eq_picture_tests.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp src/ui/PictureAudioCures.cpp
build eq_picture_scalar -DMEDIA_DSP_FORCE_SCALAR tests/eq_picture_tests.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp src/ui/PictureAudioCures.cpp
# The C SDK: its test is a strict C99 translation unit (gcc -std=c99 -pedantic), linked with the SDK and the host-driven engine,
# playing the portable fixture (written here by the portable tool) and hearing the custom wake corpus.
g++ $W -O2 tests/make_portable_fixture.cpp -o build/linux/make_portable_fixture
./build/linux/make_portable_fixture build/linux/portable_fixture.mp4 > /dev/null
SDK="src/sdk/CApi.cpp src/sdk/Probe.cpp src/demux/MatroskaReader.cpp src/engine/HostEngine.cpp src/platform/FileBlockSource.cpp src/codec/H264PcmDecoder.cpp src/codec/H264SyntaxParser.cpp src/codec/HevcSyntaxParser.cpp src/codec/Rbsp.cpp
  src/demux/IsobmffParser.cpp src/demux/MatroskaDemuxer.cpp src/demux/Demuxer.cpp src/video/TargetTracker.cpp src/video/Perception.cpp src/video/Stabilizer.cpp src/video/Orientation.cpp src/video/HorizonLevel.cpp src/video/Matte.cpp src/audio/SoundLocator.cpp src/ai/LocalIntentDispatcher.cpp src/ai/AcousticIntentRecognizer.cpp src/ai/FormantSynthesizer.cpp src/ai/AvionWakeGate.cpp
  src/ai/CustomWakeWord.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MasteringEq.cpp src/audio/MicroVad.cpp src/audio/FilterbankFeatures.cpp src/audio/PcmConverter.cpp
  src/audio/PcmRingBuffer.cpp src/audio/TimeStretcher.cpp src/clock/PredictiveMasterClock.cpp src/timeline/ChapterTimeline.cpp src/ui/KineticHud.cpp src/subtitle/SubtitleTrack.cpp
  src/subtitle/SubtitleSearch.cpp src/subtitle/EmbeddedText.cpp src/text/Utf8.cpp src/ui/PictureAudioCures.cpp src/video/Snapshot.cpp src/platform/OutputFile.cpp
  src/video/Vibrometry.cpp src/video/VirtualImu.cpp src/video/EventCamera.cpp src/video/TensorExport.cpp src/video/FlowRefiner.cpp src/codec/BitstreamPerception.cpp"
C="-std=c99 -Wall -Wextra -Wpedantic -Werror -Iinclude"
CAPI_ARGS="build/linux/portable_fixture.mp4 tests/data/voice_custom_wake.bin"
gcc $C -O2 -c tests/c_api_tests.c -o build/linux/c_api_tests_o2.o
g++ $W -O2 build/linux/c_api_tests_o2.o tests/allocation_probe_c.cpp $SDK $BASE -o build/linux/c_api_o2
gcc $C $SAN -c tests/c_api_tests.c -o build/linux/c_api_tests_asan.o
g++ $W $SAN build/linux/c_api_tests_asan.o tests/allocation_probe_c.cpp $SDK $BASE -o build/linux/c_api_asan
g++ $W -O2 tests/multi_target_tests.cpp $SDK $BASE -o build/linux/multi_target_o2
g++ $W $SAN tests/multi_target_tests.cpp $SDK $BASE -o build/linux/multi_target_asan
g++ $W -O2 tests/snapshot_tests.cpp $SDK $BASE -o build/linux/snapshot_o2
g++ $W $SAN tests/snapshot_tests.cpp $SDK $BASE -o build/linux/snapshot_asan
echo "== c_api -O2"; ./build/linux/c_api_o2 $CAPI_ARGS | tail -n 1
echo "== c_api ASan/UBSan"; ./build/linux/c_api_asan $CAPI_ARGS | tail -n 1
echo "== multi_target -O2"; ./build/linux/multi_target_o2 | tail -n 1
echo "== multi_target ASan/UBSan"; ./build/linux/multi_target_asan | tail -n 1
echo "== snapshot -O2"; ./build/linux/snapshot_o2 | tail -n 1
# API 17: host-mode tracks, subtitles after open, snapshots into memory, a Matroska file's subtitles (strict C99 too).
gcc $C -O2 -c tests/host_tracks_tests.c -o build/linux/host_tracks_o2.o
g++ $W -O2 build/linux/host_tracks_o2.o tests/allocation_probe_c.cpp $SDK $BASE -o build/linux/host_tracks_o2
gcc $C $SAN -c tests/host_tracks_tests.c -o build/linux/host_tracks_asan.o
g++ $W $SAN build/linux/host_tracks_asan.o tests/allocation_probe_c.cpp $SDK $BASE -o build/linux/host_tracks_asan
echo "== host_tracks -O2"; ./build/linux/host_tracks_o2 build/linux/portable_fixture.mp4 assets/test_fixtures/mkv/h264_aac_srt_chapters.mkv | tail -n 1
echo "== host_tracks ASan/UBSan"; ./build/linux/host_tracks_asan build/linux/portable_fixture.mp4 assets/test_fixtures/mkv/h264_aac_srt_chapters.mkv | tail -n 1
echo "== snapshot ASan/UBSan"; ./build/linux/snapshot_asan | tail -n 1
# Suites #201-#204 (docs/picture.md, "Remote vibrometry", "Event camera", "Bitstream perception", "Tensor export"; SDK API 9).
for suite in vibrometry neuromorphic_spike bitstream_perception tensor_export; do
  g++ $W -O2 tests/${suite}_tests.cpp $SDK $BASE -o build/linux/${suite}_o2
  g++ $W $SAN tests/${suite}_tests.cpp $SDK $BASE -o build/linux/${suite}_asan
  echo "== $suite -O2"; ./build/linux/${suite}_o2 | tail -n 1
  echo "== $suite ASan/UBSan"; ./build/linux/${suite}_asan | tail -n 1
done
