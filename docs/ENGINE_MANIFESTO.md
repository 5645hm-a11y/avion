# NEXT-GENERATION MEDIA ENGINE: THE BREAKTHROUGH MANIFESTO & CORE DIRECTIVES

> **The engine's laws**: every contributor to this repository works to them. Every module, interface, and implementation detail must strictly align with this document.

---

## 1. THE STRATEGIC QUESTION: WHY WILL THE WORLD USE THIS ENGINE?

We are not building another conventional media player. VLC, mpv, and MPC have been playing back passive video streams reliably for over two decades. Building another player simply to avoid dependencies is an engineering exercise, not an industry breakthrough.

### Where the Breakthrough Hides:
Traditional players were designed in the early 2000s around **passive consumption**—reading compressed bytes from a local spinner drive, passing them through heavy OOP hierarchies, and dumping pixels onto a display.

The future of media is **interactive, low-latency, cloud-native, and computationally active (Live Neural Data)**. This engine wins because:

1. **Instant, Zero-Lag Responsiveness (Zero-Seek Latency):**
   * *The Problem*: Legacy players stutter, buffer, and freeze for 300–1200ms during scrubbing or seeking, particularly on 4K/8K media and network streams.
   * *Our Advantage*: High-throughput asynchronous I/O (IOCP / Overlapped) coupled with pre-allocated memory arenas and instant sample lookups enables timeline scrubbing that behaves like sliding a physical sheet of paper—absolutely zero perceptual delay.

2. **From Passive Pipeline to Real-Time Neural Surface (AI-Native by Design):**
   * *The Problem*: Video frames in traditional engines are passive pixel buffers. Adding machine-learning enhancement (upscaling, motion interpolation, audio isolation) requires external frameworks, CPU-GPU memory roundtrips, and massive frame drops.
   * *Our Advantage*: Zero-copy Direct3D VRAM staging surfaces treat video as GPU tensors from the start. Shaders and lightweight neural compute passes (Super-Resolution, Voice Isolation, Dynamic Contrast) run inline with zero host memory overhead and sub-millisecond execution budgets.

3. **Sub-Millisecond Clock Discipline (Zero Micro-Stutter):**
   * *The Problem*: Coarse clock synchronization between audio endpoints and display sync rates causes hidden micro-jitters and aggressive, jerky frame drops.
   * *Our Advantage*: Hardware-disciplined second-order Phase-Locked Loop (PLL) clock synchronization tracking the WASAPI audio position, maintaining continuous, cinema-grade presentation accuracy without jarring frame drops.

4. **Seamless Cloud & Remote Streaming (Direct-to-Memory):**
   * *The Problem*: Legacy engines struggle with remote/cloud streams (e.g., Google Drive / S3 chunks), forcing complex caching daemons, temporary disk writes, or severe buffer halts.
   * *Our Advantage*: A unified block-streaming abstraction where remote network blocks land in the exact same zero-allocation ring buffers as local NVMe blocks. A 60GB 4K file in the cloud streams and seeks as effortlessly as local storage.

5. **Future-Proof Visual Presentation (Borderless Glassmorphic Canvas):**
   * *The Problem*: Outdated, chunky desktop window chrome and sluggish 60Hz UI wrappers that consume high CPU cycles.
   * *Our Advantage*: Pure GPU-driven composition (DirectComposition / SwapChain) delivering fluid 120Hz/240Hz/VRR borderless rendering with embedded shaders, GPU glassmorphic effects, and instant telemetry overlays.

---

## 2. THE NON-NEGOTIABLE CORE CONSTRAINTS

Every line of code submitted to this repository must respect the following laws:

### A. Zero Third-Party Multimedia Dependencies
* **STRICT BAN**: No FFmpeg/libav, no libmpv, no libVLC, no SDL, no Boost, no Qt, no dynamic plugin loaders.
* **ALLOWED**: Pure C++ (C++20/C++23) and native Windows OS primitives only: `Win32`, `COM`, Direct3D 11/12, DXGI, DirectComposition, and `WASAPI`.

### B. Zero Runtime Heap Allocations (Allocation-Free Steady-State)
* Dynamic allocation during active playback is strictly prohibited. No `malloc`, `free`, `new`, `delete`, or reallocating containers (`std::vector::push_back`, `std::string`, `std::map`).
* Memory is pre-allocated at startup via `LinearMemoryArena` backed by `VirtualAlloc` with eager page touching.
* Once the engine transitions to `Primed` or `Running`, arenas must be `Seal()`ed. Any subsequent allocation request must hard-fail.

### C. Pure Lock-Free SPSC Concurrency
* No mutexes, condition variables, locks, or OS wait-state context switches inside real-time streaming paths.
* Inter-stage coordination operates exclusively through Single Producer Single Consumer (`SpscRingBuffer`) channels utilizing explicit C++ acquire-release memory order barriers.
* Cache-line isolation (`alignas(64)`) is mandatory on read/write cursors to completely prevent false sharing between CPU cores.

### D. Zero-Copy & GPU-First Data Flow
* Decoded video data must never return to CPU host RAM.
* Demuxing, decoding, color conversion (YUV to RGB), scaling, tone mapping, and neural compute passes happen directly on dedicated hardware/VRAM timelines.
* CPU queues transport lightweight, cache-aligned (64-byte) descriptors (`MediaPacket`, `VideoFrameDescriptor`), never bulk payloads.

### E. Authorized Exception: Platform CPU-MFT Decode Fallback (author's decision, 2026-11-04, SDK API 17)
* Hardware GPU decode (D3D12 Video / DXVA) is always preferred and always tried first.
* When the host GPU has no silicon support for a stream (e.g. AV1 on Intel UHD 630 / Gen9.5), falling back to the **platform's own Media Foundation decoder (an OS-provided MFT, never a bundled third-party library)** is mandatory, so AVION plays the file natively instead of refusing it (stage 4) and leaving the host to fall back to another player.
* On this path only, decoded pictures exist in CPU memory once: the MFT's output is copied into pre-allocated, Arena-sized upload (staging) surfaces and from there onto the GPU, where the normal pipeline (colour conversion, scaling, tone mapping, neural passes, presentation) takes over. Nothing else returns to the CPU.
* Every other law still holds on this path: the MFT and its buffers are created in `Preparing`, no allocation by the engine in steady state, lock-free SPSC hand-offs, no CPU blocking on frame submission. The path is reported (`media_query_video_decoder`: software) so hosts and benchmarks can tell it from hardware decode.

---

## 3. ENGINE STATE MACHINE

The player engine lifecycle is deterministic and discrete:

```text
  [ Closed ]
      │
      ▼
 [ Preparing ]  ───► Reserve/commit Arenas, create GPU/WASAPI endpoints, query formats
      │
      ▼
   [ Primed ]   ───► Memory Sealed; fill initial packet & frame ring buffers
      │
      ▼
  [ Running ]   ───► Real-time lock-free streaming, PLL clock tracking, GPU presentation
      │
      ├───► (Seek / Format Change / Device Loss) ───► Coordinated drain to [ Preparing ]
      ▼
  [ Draining ]  ───► Flush pipeline queues without releasing pre-allocated pools
      │
      ▼
  [ Stopped ]   ───► Join threads; await new input or proceed to shutdown
4. AGENT IMPLEMENTATION CHECKLIST
Before submitting or refactoring any code in this repository, verify:

[ ] Does this commit add any external wrapper or third-party library? (Must be: NO)

[ ] Is there any malloc, new, or standard dynamic allocation in the steady-state path? (Must be: NO)

[ ] Are CPU queues single-producer, single-consumer, and free of mutexes? (Must be: YES)

[ ] Are read and write cursors isolated to separate 64-byte cache lines? (Must be: YES)

[ ] Does the design respect the GPU timeline and prevent CPU blocking on frame submission? (Must be: YES)

[ ] Is the code compatible with MSVC x64 warnings-as-errors (/WX)? (Must be: YES)

Build for the next decade of media systems—performant, uncompromising, and radically modern.