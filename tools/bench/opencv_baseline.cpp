// The OpenCV side of the head-to-head benchmark (docs/benchmarks.md): the same 640x480 grey pictures AVION's harness reads, the
// same timing and the same allocation counter (tools/bench/malloc_count.so, read through bench_alloc_count). Written the way a
// careful OpenCV user would: every output Mat made once before the loop and reused.
//   opencv_baseline flow   <gray.raw> <w> <h> <pictures> <threads>   DIS (ultrafast, fast) and Farneback dense optical flow
//   opencv_baseline survey <gray.raw> <w> <h> <pictures> <threads>   Scharr gradients and the structure tensor's 8x8 block sums
// `threads`: cv::setNumThreads (1: one core, as AVION's measurements; 0: OpenCV's default pool).
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double Micro(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }
long long Allocations() {
    using Count = unsigned long long (*)();
    static const auto count = reinterpret_cast<Count>(dlsym(RTLD_DEFAULT, "bench_alloc_count"));
    return count ? static_cast<long long>(count()) : -1;
}
double Percentile(std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1) + 0.5))]; }

struct Result { double mean, p50, p99; long long allocations; };

int Flow(const std::vector<cv::Mat>& pics, int threads) {
    constexpr int kWarm = 30; // As AVION's harness: its first 30 pictures fill Perception's history.
    const auto run = [&](const char* name, auto&& step) {
        std::vector<double> calls; calls.reserve(pics.size());
        for (int f = 1; f < kWarm; ++f) step(pics[f - 1], pics[f]);
        const long long before = Allocations();
        const auto start = Clock::now();
        for (std::size_t f = kWarm; f < pics.size(); ++f) {
            const auto t = Clock::now();
            step(pics[f - 1], pics[f]);
            calls.push_back(Micro(Clock::now() - t));
        }
        const double total = Micro(Clock::now() - start);
        const long long after = Allocations();
        const auto n = static_cast<double>(calls.size());
        // calls.push_back never reallocates (reserved): the count is the flow's own.
        std::printf("{\"bench\":\"opencv_flow\",\"method\":\"%s\",\"threads\":%d,\"opencv\":\"%s\",\"width\":%d,\"height\":%d,\"pictures\":%zu,"
                    "\"ms_mean\":%.4f,\"ms_p50\":%.4f,\"ms_p99\":%.4f,\"allocations\":%lld}\n",
                    name, threads, CV_VERSION, pics[0].cols, pics[0].rows, calls.size(), total / n / 1000, Percentile(calls, 0.5) / 1000,
                    Percentile(calls, 0.99) / 1000, before < 0 ? -1 : after - before);
    };
    cv::Mat flow(pics[0].size(), CV_32FC2);
    auto ultrafast = cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_ULTRAFAST);
    run("dis_ultrafast", [&](const cv::Mat& a, const cv::Mat& b) { ultrafast->calc(a, b, flow); });
    auto fast = cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_FAST);
    run("dis_fast", [&](const cv::Mat& a, const cv::Mat& b) { fast->calc(a, b, flow); });
    run("farneback", [&](const cv::Mat& a, const cv::Mat& b) { cv::calcOpticalFlowFarneback(a, b, flow, 0.5, 3, 15, 3, 5, 1.2, 0); });
    return 0;
}

int Survey(const std::vector<cv::Mat>& pics, int threads) {
    const int w = pics[0].cols, h = pics[0].rows;
    cv::Mat dx(h, w, CV_16S), dy(h, w, CV_16S), fx(h, w, CV_32F), fy(h, w, CV_32F), xx(h, w, CV_32F), yy(h, w, CV_32F), xy(h, w, CV_32F);
    cv::Mat bxx(h / 8, w / 8, CV_32F), byy(h / 8, w / 8, CV_32F), bxy(h / 8, w / 8, CV_32F);
    const auto step = [&](const cv::Mat& p) {
        cv::Scharr(p, dx, CV_16S, 1, 0); cv::Scharr(p, dy, CV_16S, 0, 1);
        dx.convertTo(fx, CV_32F); dy.convertTo(fy, CV_32F);
        cv::multiply(fx, fx, xx); cv::multiply(fy, fy, yy); cv::multiply(fx, fy, xy);
        // Block means over 8x8 (INTER_AREA at an integer factor): the structure tensor of each block, as AVION's survey.
        cv::resize(xx, bxx, bxx.size(), 0, 0, cv::INTER_AREA); cv::resize(yy, byy, byy.size(), 0, 0, cv::INTER_AREA); cv::resize(xy, bxy, bxy.size(), 0, 0, cv::INTER_AREA);
    };
    for (const auto& p : pics) step(p); // Warm: OpenCV's first calls set up its kernels and pools.
    double best = 1e30; long long allocations = 0;
    for (int pass = 0; pass < 5; ++pass) {
        const long long before = Allocations();
        const auto start = Clock::now();
        for (const auto& p : pics) step(p);
        best = std::min(best, Micro(Clock::now() - start) / static_cast<double>(pics.size()));
        allocations += before < 0 ? 0 : Allocations() - before;
    }
    std::printf("{\"bench\":\"opencv_survey\",\"threads\":%d,\"opencv\":\"%s\",\"width\":%d,\"height\":%d,\"pictures\":%zu,\"ms_mean\":%.4f,\"allocations\":%lld}\n",
                threads, CV_VERSION, w, h, pics.size(), best / 1000, Allocations() < 0 ? -1 : allocations);
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc < 7) { std::fputs("usage: opencv_baseline flow|survey <gray.raw> <w> <h> <pictures> <threads>\n", stderr); return 1; }
    const int w = std::atoi(argv[3]), h = std::atoi(argv[4]), n = std::atoi(argv[5]), threads = std::atoi(argv[6]);
    cv::setNumThreads(threads);
    std::FILE* f = std::fopen(argv[2], "rb");
    if (!f) return 2;
    std::vector<cv::Mat> pics;
    for (int i = 0; i < n; ++i) {
        cv::Mat m(h, w, CV_8U);
        if (std::fread(m.data, 1, static_cast<std::size_t>(w) * h, f) != static_cast<std::size_t>(w) * h) break;
        pics.push_back(m);
    }
    std::fclose(f);
    if (pics.size() < 40) return 2;
    return std::strcmp(argv[1], "flow") == 0 ? Flow(pics, threads) : Survey(pics, threads);
}
