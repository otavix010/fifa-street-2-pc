// Temporal anti-aliasing and upscaling by the vendors' libraries: NVIDIA DLSS (NGX), AMD FSR (FidelityFX API) and
// Intel XeSS. Each takes the frame's color, depth (R32_FLOAT), motion vectors (R16G16_FLOAT, render uv from current
// to previous) and reactive mask (R8), the camera jitter in render pixels, and writes the output (R8G8B8A8, UAV).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include <d3d12.h>

namespace gpu {

struct UpscaleInputs {
    ID3D12Resource* color = nullptr;   // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* mask = nullptr;    // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* output = nullptr;  // UNORDERED_ACCESS
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    float jitter_x = 0, jitter_y = 0;  // render pixels, -0.5 to 0.5
    float motion_scale = 1;            // the motion vectors' span over the frame before: 0.5 between in-between frames
    float sharpness = 0;               // 0 to 1
    float dt_ms = 16.7f;
    bool reset = false;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual bool Dispatch(ID3D12GraphicsCommandList* cl, const UpscaleInputs& in) = 0;
    virtual const char* Name() const = 0;
};

// the render size over the output size for a mode: native 1, quality 1/1.5, balanced 1/1.7, performance 1/2
float UpscaleRatio(const std::string& mode);

// kind: dlss, fsr or xess; null with error set when the library or the GPU can't
std::unique_ptr<Upscaler> CreateUpscaler(const std::string& kind, const std::string& mode, ID3D12Device* dev,
                                         ID3D12CommandQueue* queue, uint32_t render_w, uint32_t render_h,
                                         uint32_t out_w, uint32_t out_h, std::string& error);

}  // namespace gpu
