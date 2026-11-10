#pragma once
// The GPU half of media_eq_picture_tests (Windows): picture controls and micro-motion through the production colour pipeline.
namespace eq_picture_gpu {
// Adds to `failures`; returns -1 when there is no hardware D3D12 device (the checks are skipped), else 0.
int Run(int& failures);
}
