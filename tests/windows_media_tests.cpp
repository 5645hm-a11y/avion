#include "media/platform/windows/Win32WasapiAudioSink.hpp"
#include "media/platform/windows/D3D12VideoDecoder.hpp"
#include "allocation_probe.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cstdio>
#include <cstring>

using namespace media;
using namespace media::platform;
using namespace media::platform::windows;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
int Audio() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { std::printf("COM initialization failed: %08lx\n", static_cast<unsigned long>(com)); return 1; }
    int result = 0;
    {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(2 * 1024 * 1024));
        Win32WasapiAudioSink sink;
        WasapiOptions invalid; invalid.format.encoding = static_cast<AudioEncoding>(255);
        CHECK(sink.Prepare(arena, invalid) == EndpointStatus::InvalidArgument);
        const auto status = sink.Prepare(arena);
        if (status != EndpointStatus::Ready) {
            const auto error = sink.LastError();
            // Only environmental absence/format/capability can skip; unexpected native failures fail.
            const bool unavailable = error == E_NOTFOUND || error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
                error == AUDCLNT_E_SERVICE_NOT_RUNNING || error == AUDCLNT_E_UNSUPPORTED_FORMAT || error == E_NOINTERFACE;
            std::printf("%s WASAPI prepare: %08lx\n", unavailable ? "SKIP" : "FAIL", static_cast<unsigned long>(error));
            result = unavailable && !failures ? 77 : 1;
        } else {
            CHECK(arena.Seal());
            auto feed = [&] {
                AudioWriteSpan span{};
                const auto acquired = sink.TryAcquire(16384, span);
                if (acquired == EndpointStatus::Ready) {
                    const auto format = sink.Format();
                    std::memset(span.data, 0, static_cast<std::size_t>(span.frames) * format.channels * format.bytes_per_sample);
                    CHECK(sink.Commit(span.lease, span.frames) == EndpointStatus::Ready);
                } else CHECK(acquired == EndpointStatus::WouldBlock);
            };
            feed(); CHECK(sink.Start() == EndpointStatus::Ready);
            AudioClockSample first{}, last{}; bool have_first = false;
            const auto end = GetTickCount64() + 350;
            allocation_probe::Begin();
            while (GetTickCount64() < end) {
                feed();
                const auto pumped = sink.Pump(); CHECK(pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
                AudioClockSample sample{}; CHECK(sink.Clock(sample) == EndpointStatus::Ready);
                if (sample.precise) {
                    if (!have_first) { first = sample; have_first = true; }
                    CHECK(sample.frequency == first.frequency && sample.position >= last.position);
                    CHECK(sample.monotonic_time_100ns >= last.monotonic_time_100ns);
                    last = sample;
                }
                Sleep(1); // Test harness only; the endpoint itself polls with timeout zero.
            }
            const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
            CHECK(have_first && last.position > first.position && last.monotonic_time_100ns > first.monotonic_time_100ns);
            const auto before_gap = last.generation;
            const auto starve_end = GetTickCount64() + 450;
            while (GetTickCount64() < starve_end) { (void)sink.Pump(); Sleep(1); }
            CHECK(sink.Clock(last) == EndpointStatus::Ready);
            CHECK(sink.SilenceFrames() != 0 && !last.precise && last.generation != before_gap);
            const auto recover_end = GetTickCount64() + 100;
            while (GetTickCount64() < recover_end) { feed(); (void)sink.Pump(); Sleep(1); }
            CHECK(sink.Clock(last) == EndpointStatus::Ready && last.precise && last.generation != before_gap);
            // In-process invalidation (the fault tests' hook): the stream is lost on the next pump, endpoints refuse to open
            // for 450 ms (two refused retries at 200 ms intervals), and the third retry reopens the default endpoint. The
            // machine's device is never taken from other applications. The clock keeps answering throughout (the null
            // endpoint meanwhile) and each endpoint change starts a new generation.
            const auto before_loss = last.generation;
            sink.InjectInvalidation(AUDCLNT_E_DEVICE_INVALIDATED, 450);
            bool detached = false, clock_ok = true;
            allocation_probe::Begin();
            const auto loss_end = GetTickCount64() + 1100;
            while (GetTickCount64() < loss_end) {
                feed();
                const auto pumped = sink.Pump(); clock_ok = clock_ok && (pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
                detached = detached || !sink.Attached();
                AudioClockSample sample{}; clock_ok = clock_ok && sink.Clock(sample) == EndpointStatus::Ready;
                Sleep(1);
            }
            const auto loss_allocations = allocation_probe::End();
            const auto& r = sink.Recovery();
            CHECK(clock_ok && detached && sink.Attached() && loss_allocations == 0);
            CHECK(r.losses == 1 && r.injected == 1 && r.last_loss == AUDCLNT_E_DEVICE_INVALIDATED && r.detachments == 1 && r.migrations == 1 && r.last_open == S_OK);
            CHECK(sink.Clock(last) == EndpointStatus::Ready && last.generation != before_loss);
            std::printf("WASAPI injected loss: losses=%u injected=%u detachments=%u migrations=%u attached=%u allocations=%llu\n", r.losses, r.injected,
                        r.detachments, r.migrations, sink.Attached() ? 1u : 0u, static_cast<unsigned long long>(loss_allocations));
            CHECK(sink.Stop() == EndpointStatus::Ready);
            std::printf("WASAPI: period=%u, endpoint=%u, clock frequency=%llu, silence frames=%llu, allocations=%llu\n",
                sink.PeriodFrames(), sink.EndpointFrames(), static_cast<unsigned long long>(last.frequency),
                static_cast<unsigned long long>(sink.SilenceFrames()), static_cast<unsigned long long>(allocations));
            result = failures ? 1 : 0;
        }
    }
    CoUninitialize(); return result;
}

// Original, deterministic H.264 CAVLC I_PCM fixture: one 64x64 IDR picture.
// Uncompressed macroblock syntax is encoded here; decoding still runs on the GPU.
struct Bits {
    std::array<std::byte, 8192> data{}; unsigned bits = 0;
    void Bit(unsigned v) { if (v) data[bits / 8] |= std::byte(1u << (7 - bits % 8)); ++bits; }
    void Value(unsigned v, unsigned n) { while (n) Bit((v >> --n) & 1); }
    void Ue(unsigned v) {
        const unsigned code = v + 1; unsigned n = 0;
        for (unsigned t = code; t > 1; t >>= 1) ++n;
        for (unsigned i = 0; i < n; ++i) Bit(0);
        Value(code, n + 1);
    }
    void Align() { while (bits % 8) Bit(0); }
};
unsigned H264Fixture(std::span<std::byte> output) {
    Bits b;
    b.Ue(0); b.Ue(2); b.Ue(0); b.Value(0, 4); b.Ue(0); b.Value(0, 4);
    b.Bit(0); b.Bit(0); b.Ue(0); b.Ue(1); // IDR reference marking, QP delta 0, deblocking off.
    for (unsigned mb = 0; mb != 16; ++mb) {
        b.Ue(25); b.Align();
        for (unsigned i = 0; i != 384; ++i) b.Value(128, 8);
    }
    b.Bit(1); b.Align();
    unsigned count = 0, zeros = 0; output[count++] = std::byte{0x65};
    for (unsigned i = 0; i != b.bits / 8; ++i) {
        const auto v = std::to_integer<unsigned>(b.data[i]);
        if (zeros == 2 && v <= 3) { output[count++] = std::byte{3}; zeros = 0; }
        output[count++] = b.data[i]; zeros = v == 0 ? zeros + 1 : 0;
    }
    return count;
}
unsigned H264PredictedFixture(std::span<std::byte> output) {
    Bits b;
    b.Ue(0); b.Ue(0); b.Ue(0); b.Value(1, 4); b.Value(2, 4);
    b.Bit(0); b.Bit(0); b.Bit(0); // Default ref count, no list modification, no adaptive reference marking.
    b.Ue(0); b.Ue(1); b.Ue(16); // QP delta 0, deblocking off, sixteen skipped macroblocks.
    b.Bit(1); b.Align();
    output[0] = std::byte{0x41};
    unsigned count = 1, zeros = 0;
    for (unsigned i = 0; i != b.bits / 8; ++i) {
        const auto v = std::to_integer<unsigned>(b.data[i]);
        if (zeros == 2 && v <= 3) { output[count++] = std::byte{3}; zeros = 0; }
        output[count++] = b.data[i]; zeros = v == 0 ? zeros + 1 : 0;
    }
    return count;
}
int Video() {
    ID3D12Debug* debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); Release(debug); }
    IDXGIFactory6* factory = nullptr; ID3D12Device* device = nullptr;
    auto hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { std::printf("FAIL DXGI factory: %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    for (UINT i = 0; !device; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 desc{}; hr = adapter->GetDesc1(&desc);
        if (SUCCEEDED(hr) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        Release(adapter);
    }
    Release(factory);
    if (!device) { std::puts("SKIP no hardware D3D12 device"); return 77; }
    ID3D12InfoQueue* messages = nullptr; (void)device->QueryInterface(IID_PPV_ARGS(&messages));
    ID3D12Fence *consumer = nullptr, *gate = nullptr;
    ID3D12CommandQueue* consumer_queue = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&consumer_queue));
    if (FAILED(hr)) { Release(consumer); Release(gate); Release(consumer_queue); Release(messages); Release(device); return 1; }
    int result = 0;
    {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(2 * 1024 * 1024));
        D3D12VideoDecoder decoder;
        DecoderOptions options; options.width = options.height = 64; options.surfaces = 4; options.max_bitstream_bytes = 8192;
        auto* frames = arena.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 4>>();
        auto* returns = arena.Create<core::SpscRingBuffer<core::MediaPacket, 4>>();
        auto* surface_returns = arena.Create<core::SpscRingBuffer<VideoSurfaceReturn, 4>>();
        auto* payload = static_cast<std::byte*>(arena.Allocate(8192));
        CHECK(frames && returns && surface_returns && payload);
        const auto prepared = decoder.Prepare(arena, device, consumer, options);
        if (prepared == DecodeStatus::Unsupported) {
            std::printf("SKIP H.264 shader-readable D3D12 tier 2 profile unavailable: %08lx\n", static_cast<unsigned long>(decoder.LastError()));
            result = failures ? 1 : 77;
        } else if (prepared != DecodeStatus::Ready) {
            std::printf("FAIL decoder prepare: %08lx\n", static_cast<unsigned long>(decoder.LastError())); result = 1;
        } else {
            const auto bytes = H264Fixture({payload, 8192});
            DXVA_PicParams_H264 params{}; DXVA_Qmatrix_H264 matrix{};
            params.wFrameWidthInMbsMinus1 = 3; params.wFrameHeightInMbsMinus1 = 3;
            params.num_ref_frames = 1; params.RefPicFlag = 1; params.IntraPicFlag = 1;
            params.MbsConsecutiveFlag = 1; params.MinLumaBipredSize8x8Flag = 1;
            params.chroma_format_idc = 1; params.frame_mbs_only_flag = 1;
            params.direct_8x8_inference_flag = 1; params.deblocking_filter_control_present_flag = 1;
            params.Reserved16Bits = 3;
            for (auto& ref : params.RefFrameList) ref.bPicEntry = 255;
            std::memset(&matrix, 16, sizeof(matrix));
            NalSlice slice{0, bytes}; DecodePicture picture;
            picture.packet.payload_bytes = bytes; picture.packet.payload_block_id = 2;
            picture.packet.payload_generation = 1; picture.packet.playback_epoch = 1;
            picture.packet.duration_100ns = 400000;
            picture.payload = {payload, 0, bytes, {2, 1}};
            picture.slices = {&slice, 1}; picture.h264 = &params; picture.h264_matrix = &matrix;
            picture.retain_as_reference = true;
            CHECK(arena.Seal());
            // Rejection cannot publish a frame or return a still-owned packet.
            picture.packet.payload_bytes = 8193;
            CHECK(decoder.Submit(picture, *frames, *returns) == DecodeStatus::InvalidArgument);
            CHECK(frames->PeekReadSlot() == nullptr && returns->PeekReadSlot() == nullptr);
            picture.packet.payload_bytes = bytes;
            allocation_probe::Begin();
            const auto submitted = decoder.Submit(picture, *frames, *returns);
            const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
            CHECK(submitted == DecodeStatus::Submitted);
            if (submitted == DecodeStatus::Submitted) {
                const auto* frame = frames->PeekReadSlot(); const auto* returned = returns->PeekReadSlot();
                CHECK(frame && returned && returned->payload_block_id == 2 && returned->payload_generation == 1);
                if (frame) {
                    const SurfaceHandle handle{frame->surface_id, frame->surface_generation};
                    CHECK(decoder.DrainAndClose() == DecodeStatus::WouldBlock);
                    CHECK(frame->format == core::VideoSurfaceFormat::Nv12 && decoder.Surface(handle));
                    CHECK(frame->ready_fence_value != 0 && frame->ready_fence_id == options.decode_timeline_id);
                    D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{};
                    auto state = DecodeStatus::WouldBlock; const auto deadline = GetTickCount64() + 10000;
                    while (state == DecodeStatus::WouldBlock && GetTickCount64() < deadline) { state = decoder.Statistics(handle, stats); Sleep(1); }
                    CHECK(state == DecodeStatus::Ready);
                    CHECK(stats.Status == D3D12_VIDEO_DECODE_STATUS_OK && stats.NumMacroblocksAffected == 0);
                    std::printf("H.264 GPU decode: status=%u, affected macroblocks=%llu, allocations=%llu\n",
                        static_cast<unsigned>(stats.Status), static_cast<unsigned long long>(stats.NumMacroblocksAffected), static_cast<unsigned long long>(allocations));
                    CHECK(SUCCEEDED(consumer_queue->Wait(decoder.DecodeFence(), frame->ready_fence_value)));
                    CHECK(SUCCEEDED(consumer_queue->Wait(gate, 1)));
                    CHECK(SUCCEEDED(consumer_queue->Signal(consumer, 1)));
                    CHECK(decoder.ReturnSurface({handle.id, handle.generation, 1, options.consumer_timeline_id, 1}));
                    CHECK(!decoder.ReturnSurface({handle.id, handle.generation, 1, options.consumer_timeline_id, 0}));
                    CHECK(frames->ReleaseReadSlot()); CHECK(returns->ReleaseReadSlot());
                    // Source block was returned after upload packing and can now hold the next AU.
                    const auto p_bytes = H264PredictedFixture({payload, 8192});
                    slice.bytes = p_bytes; picture.packet.payload_bytes = p_bytes; picture.payload.valid_bytes = p_bytes;
                    picture.packet.pts_100ns = picture.packet.dts_100ns = 400000;
                    params.IntraPicFlag = 0; params.frame_num = 1;
                    params.CurrFieldOrderCnt[0] = params.CurrFieldOrderCnt[1] = 2;
                    params.RefFrameList[0].bPicEntry = 0; params.UsedForReferenceFlags = 3;
                    picture.references = {&handle, 1}; picture.retain_as_reference = false;
                    CHECK(decoder.Submit(picture, *frames, *returns) == DecodeStatus::Submitted);
                    const auto* predicted = frames->PeekReadSlot();
                    CHECK(predicted != nullptr);
                    if (predicted) {
                        const SurfaceHandle next{predicted->surface_id, predicted->surface_generation};
                        CHECK(decoder.Statistics(next, stats) == DecodeStatus::WouldBlock); // Consumer gate is unsignalled.
                        CHECK(SUCCEEDED(gate->Signal(1)));
                        state = DecodeStatus::WouldBlock; const auto p_deadline = GetTickCount64() + 10000;
                        while (state == DecodeStatus::WouldBlock && GetTickCount64() < p_deadline) { state = decoder.Statistics(next, stats); Sleep(1); }
                        CHECK(state == DecodeStatus::Ready && stats.Status == D3D12_VIDEO_DECODE_STATUS_OK && stats.NumMacroblocksAffected == 0);
                        auto* returned_surface = surface_returns->AcquireWriteSlot(); CHECK(returned_surface != nullptr);
                        *returned_surface = {next.id, next.generation, 1, options.consumer_timeline_id, 0};
                        CHECK(surface_returns->CommitWriteSlot());
                        std::uint32_t processed = 0;
                        CHECK(decoder.PollReturns(*surface_returns, 1, processed) == DecodeStatus::Ready && processed == 1);
                        CHECK(frames->ReleaseReadSlot());
                        CHECK(returns->PeekReadSlot() != nullptr && returns->ReleaseReadSlot());
                        std::puts("H.264 reference decode: delayed consumer fence respected; decode status OK");
                    } else CHECK(SUCCEEDED(gate->Signal(1)));
                    CHECK(decoder.ReleaseReference(handle));
                }
            }
            CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
            result = failures ? 1 : 0;
        }
    }
    if (messages) {
        const auto count = messages->GetNumStoredMessages();
        alignas(D3D12_MESSAGE) std::array<std::byte, 4096> storage{};
        for (UINT64 i = 0; i != count; ++i) {
            SIZE_T size = storage.size(); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("D3D12 validation error: %s\n", message->pDescription); result = 1;
            }
        }
    }
    Release(consumer_queue); Release(consumer); Release(gate); Release(messages); Release(device); return result;
}
int HevcPreparation() {
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("SKIP no D3D12 device for HEVC preparation"); return 77;
    }
    unsigned supported = 0;
    for (const bool ten_bit : {false, true}) {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(2 * 1024 * 1024));
        D3D12VideoDecoder decoder; DecoderOptions options;
        options.codec = VideoCodec::Hevc; options.ten_bit = ten_bit;
        options.width = options.height = 128; options.surfaces = 4; options.max_bitstream_bytes = 8192;
        const auto state = decoder.Prepare(arena, device, nullptr, options);
        if (state == DecodeStatus::Unsupported) std::printf("HEVC %u-bit preparation unavailable\n", ten_bit ? 10u : 8u);
        else {
            CHECK(state == DecodeStatus::Ready);
            if (state == DecodeStatus::Ready) ++supported;
            CHECK(arena.Seal());
            CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
            std::printf("HEVC %u-bit preparation status=%u (no bitstream decode fixture)\n", ten_bit ? 10u : 8u, static_cast<unsigned>(state));
        }
    }
    Release(device); return failures ? 1 : supported ? 0 : 77;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "audio") == 0) return Audio();
    if (argc == 2 && std::strcmp(argv[1], "video") == 0) return Video();
    if (argc == 2 && std::strcmp(argv[1], "hevc-prepare") == 0) return HevcPreparation();
    return 1;
}
