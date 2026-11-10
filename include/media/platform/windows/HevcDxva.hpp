#pragma once
#include "media/codec/HevcSyntaxParser.hpp"
#include "media/platform/windows/D3D12VideoDecoder.hpp"
namespace media::codec {
// Windows-only adapter; the bitstream parser and its tables are platform independent.
// RefPicList indices address references[]; D3D12VideoDecoder remaps them to DPB surfaces.
// color_metadata_id: matrix (bits 0-7), full range (bit 8), transfer (16-23), primaries (24-31).
struct HevcDxva {
    DXVA_PicParams_HEVC parameters{};
    DXVA_Qmatrix_HEVC matrix{};
    platform::SurfaceHandle references[15]{};
    platform::windows::NalSlice slices[256]{};
    platform::windows::DecodePicture Prepare(const HevcPicture&, core::MediaPacket, platform::BlockView) noexcept;
};
}
