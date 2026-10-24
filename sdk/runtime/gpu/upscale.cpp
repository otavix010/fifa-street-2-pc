#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gpu/upscale.h"

#include <cmath>
#include <cstdio>

#include "app/app.h"
#include "core/log.h"

// NVIDIA NGX (DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
// AMD FidelityFX API (FSR), loaded at run time
#include <dx12/ffx_api_dx12.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
// Intel XeSS, loaded at run time
#include <xess.h>
#include <xess_d3d12.h>

namespace gpu {

float UpscaleRatio(const std::string& mode) {
    if (mode == "quality") return 1 / 1.5f;
    if (mode == "balanced") return 1 / 1.7f;
    if (mode == "performance") return 0.5f;
    if (mode == "ultra") return 1 / 3.0f;
    return 1.0f;
}

namespace {

std::wstring ExeDirW() { return app::ExeDir().wstring(); }

// ---------------------------------------------------------------------------------------------------- DLSS

class Dlss final : public Upscaler {
public:
    ~Dlss() override {
        if (handle_) NVSDK_NGX_D3D12_ReleaseFeature(handle_);
        if (params_) NVSDK_NGX_D3D12_DestroyParameters(params_);
        if (dev_) NVSDK_NGX_D3D12_Shutdown1(dev_);
    }

    bool Init(const std::string& mode, ID3D12Device* dev, ID3D12CommandQueue* queue, uint32_t rw, uint32_t rh, uint32_t ow,
              uint32_t oh, std::string& error) {
        std::wstring dir = ExeDirW();
        // a project id of this port's own; NGX takes any GUID-like string for an unregistered title
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("5b1c8a0e-9f6d-4c2a-8e3b-2f7d6a1c4e90",
                                                                 NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(), dev);
        if (NVSDK_NGX_FAILED(r)) {
            error = "NGX init failed (" + std::to_string(int(r)) + "): not an NVIDIA RTX card, or an old driver";
            return false;
        }
        dev_ = dev;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&params_))) {
            error = "NGX capability parameters failed";
            return false;
        }
        int available = 0;
        params_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (!available) {
            error = "DLSS is not available on this GPU";
            return false;
        }
        NVSDK_NGX_PerfQuality_Value q = mode == "quality" ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                        : mode == "balanced" ? NVSDK_NGX_PerfQuality_Value_Balanced
                                        : mode == "performance" ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                        : mode == "ultra" ? NVSDK_NGX_PerfQuality_Value_UltraPerformance
                                                          : NVSDK_NGX_PerfQuality_Value_DLAA;
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = rw;
        cp.Feature.InHeight = rh;
        cp.Feature.InTargetWidth = ow;
        cp.Feature.InTargetHeight = oh;
        cp.Feature.InPerfQualityValue = q;
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        // the feature is made with a command list of its own, run at once
        ComPtrList list;
        if (!list.Make(dev, queue)) {
            error = "DLSS: no command list";
            return false;
        }
        r = NGX_D3D12_CREATE_DLSS_EXT(list.cl, 1, 1, &handle_, params_, &cp);
        list.Run();
        if (NVSDK_NGX_FAILED(r)) {
            error = "DLSS feature creation failed (" + std::to_string(int(r)) + ")";
            return false;
        }
        return true;
    }

    bool Dispatch(ID3D12GraphicsCommandList* cl, const UpscaleInputs& in) override {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
        e.Feature.pInColor = in.color;
        e.Feature.pInOutput = in.output;
        e.pInDepth = in.depth;
        e.pInMotionVectors = in.motion;
        e.pInBiasCurrentColorMask = in.mask;
        e.InJitterOffsetX = in.jitter_x;
        e.InJitterOffsetY = in.jitter_y;
        e.InRenderSubrectDimensions = {in.render_w, in.render_h};
        e.InReset = in.reset ? 1 : 0;
        // motion vectors in render uv: pixels are uv times the render size
        e.InMVScaleX = float(in.render_w) * in.motion_scale;
        e.InMVScaleY = float(in.render_h) * in.motion_scale;
        e.InPreExposure = 1;
        e.InExposureScale = 1;
        e.InFrameTimeDeltaInMsec = in.dt_ms;
        return NVSDK_NGX_SUCCEED(NGX_D3D12_EVALUATE_DLSS_EXT(cl, handle_, params_, &e));
    }

    const char* Name() const override { return "DLSS"; }

private:
    // a one-off command list for the feature's creation
    struct ComPtrList {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        ID3D12CommandQueue* q = nullptr;
        ID3D12Device* dev = nullptr;
        bool Make(ID3D12Device* d, ID3D12CommandQueue* queue) {
            dev = d;
            q = queue;
            return SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
                   SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
        }
        void Run() {
            cl->Close();
            ID3D12CommandList* lists[] = {cl};
            q->ExecuteCommandLists(1, lists);
            ID3D12Fence* f = nullptr;
            if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                q->Signal(f, 1);
                f->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 5000);
                CloseHandle(ev);
                f->Release();
            }
        }
        ~ComPtrList() {
            if (cl) cl->Release();
            if (alloc) alloc->Release();
        }
    };

    ID3D12Device* dev_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------- FSR

class Fsr final : public Upscaler {
public:
    ~Fsr() override {
        if (ctx_ && fn_.DestroyContext) fn_.DestroyContext(&ctx_, nullptr);
        if (lib_) FreeLibrary(lib_);
    }

    bool Init(ID3D12Device* dev, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, std::string& error) {
        lib_ = LoadLibraryW((ExeDirW() + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
        if (!lib_) {
            error = "amd_fidelityfx_loader_dx12.dll is missing beside the exe";
            return false;
        }
        // ffxLoadFunctions only fills these under MSBuild's _WINDOWS
        fn_.CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(lib_, "ffxCreateContext"));
        fn_.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(lib_, "ffxDestroyContext"));
        fn_.Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(lib_, "ffxConfigure"));
        fn_.Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(lib_, "ffxQuery"));
        fn_.Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(lib_, "ffxDispatch"));
        if (!fn_.CreateContext || !fn_.Dispatch) {
            error = "the FidelityFX loader has no API";
            return false;
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = dev;
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        // the game's colour is display ready (gamma): non linear, low dynamic range, exposure worked out by FSR
        desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        desc.maxRenderSize = {rw, rh};
        desc.maxUpscaleSize = {ow, oh};
        ffxReturnCode_t r = fn_.CreateContext(&ctx_, &desc.header, nullptr);
        if (r != FFX_API_RETURN_OK) {
            error = "FSR context creation failed (" + std::to_string(int(r)) + ")";
            ctx_ = nullptr;
            return false;
        }
        return true;
    }

    bool Dispatch(ID3D12GraphicsCommandList* cl, const UpscaleInputs& in) override {
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = cl;
        d.color = ffxApiGetResourceDX12(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.reactive = ffxApiGetResourceDX12(in.mask, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {in.jitter_x, in.jitter_y};
        d.motionVectorScale = {float(in.render_w) * in.motion_scale, float(in.render_h) * in.motion_scale};
        d.renderSize = {in.render_w, in.render_h};
        d.upscaleSize = {in.out_w, in.out_h};
        d.enableSharpening = in.sharpness > 0;
        d.sharpness = in.sharpness;
        d.frameTimeDelta = in.dt_ms;
        d.preExposure = 1;
        d.reset = in.reset;
        // a street football camera: near and far guessed, FSR only uses them to judge depth changes
        d.cameraNear = 0.1f;
        d.cameraFar = 1000.0f;
        d.cameraFovAngleVertical = 1.0f;
        d.viewSpaceToMetersFactor = 1.0f;
        return fn_.Dispatch(&ctx_, &d.header) == FFX_API_RETURN_OK;
    }

    const char* Name() const override { return "FSR"; }

private:
    HMODULE lib_ = nullptr;
    ffxFunctions fn_{};
    ffxContext ctx_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------- XeSS

class Xess final : public Upscaler {
public:
    ~Xess() override {
        if (ctx_ && destroy_) destroy_(ctx_);
        if (lib_) FreeLibrary(lib_);
    }

    bool Init(const std::string& mode, ID3D12Device* dev, uint32_t ow, uint32_t oh, std::string& error) {
        lib_ = LoadLibraryW((ExeDirW() + L"\\libxess.dll").c_str());
        if (!lib_) {
            error = "libxess.dll is missing beside the exe";
            return false;
        }
        auto create = reinterpret_cast<decltype(&xessD3D12CreateContext)>(GetProcAddress(lib_, "xessD3D12CreateContext"));
        auto init = reinterpret_cast<decltype(&xessD3D12Init)>(GetProcAddress(lib_, "xessD3D12Init"));
        auto velocity = reinterpret_cast<decltype(&xessSetVelocityScale)>(GetProcAddress(lib_, "xessSetVelocityScale"));
        execute_ = reinterpret_cast<decltype(&xessD3D12Execute)>(GetProcAddress(lib_, "xessD3D12Execute"));
        destroy_ = reinterpret_cast<decltype(&xessDestroyContext)>(GetProcAddress(lib_, "xessDestroyContext"));
        velocity_ = velocity;
        if (!create || !init || !execute_ || !destroy_ || !velocity) {
            error = "libxess.dll has no D3D12 API";
            return false;
        }
        if (create(dev, &ctx_) != XESS_RESULT_SUCCESS) {
            error = "XeSS context creation failed";
            ctx_ = nullptr;
            return false;
        }
        xess_d3d12_init_params_t p{};
        p.outputResolution = {ow, oh};
        p.qualitySetting = mode == "quality" ? XESS_QUALITY_SETTING_QUALITY
                           : mode == "balanced" ? XESS_QUALITY_SETTING_BALANCED
                           : mode == "performance" ? XESS_QUALITY_SETTING_PERFORMANCE
                           : mode == "ultra" ? XESS_QUALITY_SETTING_ULTRA_PERFORMANCE
                                             : XESS_QUALITY_SETTING_AA;
        p.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK |
                      XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
        xess_result_t r = init(ctx_, &p);
        if (r != XESS_RESULT_SUCCESS) {
            error = "XeSS init failed (" + std::to_string(int(r)) + ")";
            return false;
        }
        return true;
    }

    bool Dispatch(ID3D12GraphicsCommandList* cl, const UpscaleInputs& in) override {
        // motion vectors in render uv: pixels are uv times the render size
        velocity_(ctx_, float(in.render_w) * in.motion_scale, float(in.render_h) * in.motion_scale);
        xess_d3d12_execute_params_t e{};
        e.pColorTexture = in.color;
        e.pVelocityTexture = in.motion;
        e.pDepthTexture = in.depth;
        e.pResponsivePixelMaskTexture = in.mask;
        e.pOutputTexture = in.output;
        e.jitterOffsetX = in.jitter_x;
        e.jitterOffsetY = in.jitter_y;
        e.exposureScale = 1;
        e.resetHistory = in.reset ? 1 : 0;
        e.inputWidth = in.render_w;
        e.inputHeight = in.render_h;
        return execute_(ctx_, cl, &e) == XESS_RESULT_SUCCESS;
    }

    const char* Name() const override { return "XeSS"; }

private:
    HMODULE lib_ = nullptr;
    xess_context_handle_t ctx_ = nullptr;
    decltype(&xessD3D12Execute) execute_ = nullptr;
    decltype(&xessDestroyContext) destroy_ = nullptr;
    decltype(&xessSetVelocityScale) velocity_ = nullptr;
};

}  // namespace

std::unique_ptr<Upscaler> CreateUpscaler(const std::string& kind, const std::string& mode, ID3D12Device* dev,
                                         ID3D12CommandQueue* queue, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                                         std::string& error) {
    if (kind == "dlss") {
        auto u = std::make_unique<Dlss>();
        if (u->Init(mode, dev, queue, rw, rh, ow, oh, error)) return u;
    } else if (kind == "fsr") {
        auto u = std::make_unique<Fsr>();
        if (u->Init(dev, rw, rh, ow, oh, error)) return u;
    } else if (kind == "xess") {
        auto u = std::make_unique<Xess>();
        if (u->Init(mode, dev, ow, oh, error)) return u;
    } else {
        error = "unknown upscaler " + kind;
    }
    return nullptr;
}

}  // namespace gpu
