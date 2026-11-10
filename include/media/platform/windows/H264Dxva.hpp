#pragma once
#include "media/codec/H264SyntaxParser.hpp"
#include "media/platform/windows/D3D12VideoDecoder.hpp"
namespace media::codec {
// Windows-only adapter; the bitstream parser and its tables are platform independent.
struct H264Dxva {
    DXVA_PicParams_H264 parameters{};
    DXVA_Qmatrix_H264 matrix{};
    platform::SurfaceHandle references[16]{};
    platform::windows::NalSlice slices[256]{};
    platform::windows::DecodePicture Prepare(const H264Picture&, core::MediaPacket, platform::BlockView) noexcept;
};
}
