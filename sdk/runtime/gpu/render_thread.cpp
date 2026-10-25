// The Direct3D 12 backend on a thread of its own. The title's render thread records each call with what the console
// takes at call time (state changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory
// are read when the draw runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks
// and locks wait for that.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <smmintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
constexpr uint32_t kBatchCommands = 64;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    Surface s1, s2;
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t> bytes;
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off, len;
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
std::vector<Range> StateRanges() {
    static const State s;
    return {
        {At(s, &s.simple), At(s, &s.textures) - At(s, &s.simple)},
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.vsc), At(s, &s.rt) - At(s, &s.vsc)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), sizeof(s.transforms[0])},
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

// equal bytes, 16 at a time: most of the state is compared on every draw
bool Same(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i = 0;
    __m128i diff = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
    return i == n || !std::memcmp(a + i, b + i, n - i);
}

using Clock = std::chrono::steady_clock;
uint64_t Ns(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner) : inner_(std::move(inner)), ranges_(StateRanges()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        cur_ = NewBatch();
        std::thread([this] { Run(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        // at exit the worker may be gone mid-batch: then the inner backend is left, not freed under it
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) (void)inner_.release();
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        cur_->cmds.push_back(c);
        for (uint32_t tex : s.textures) Use(tex);
        for (uint32_t pal : s.palettes) Use(pal);
        for (const auto& st : s.streams) Use(st.vb);
        Use(s.rt.header);
        Use(s.zs.header);
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        g_perf_capture_ns += Ns(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += Ns(t);
    }

    // rects and points are taken now; the copy itself, CPU or GPU, runs in order like the console's
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override {
        auto t = Clock::now();
        Cmd c{Op::Copy};
        c.a = count;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.s1 = src;
        c.s1.header = Header(src.header);
        c.s2 = dst;
        c.s2.header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += Ns(t);
    }

    void Present(const Surface& front) override {
        auto t = Clock::now();
        Cmd c{Op::Present};
        c.s1 = front;
        c.s1.header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += Ns(t);
        if (app::Get().interp) {
            // the replay reads guest memory as the frame left it: the title waits until it has run
            auto w = Clock::now();
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return replayed_.load() >= id || exited_; });
            g_perf_wait_kind_ns[size_t(WaitFor::Frame)] += Ns(w);
        } else {
            // the title records one frame while the one before it draws
            Wait(last_present_, WaitFor::Frame, nullptr);
        }
        last_present_ = id;
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf()) LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now", data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override {
        auto it = last_use_.find(resource);
        return it != last_use_.end() && it->second > done_.load();
    }
    void WaitResource(uint32_t resource) override {
        auto it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second, WaitFor::Resource, "resource");
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data, late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        uint8_t now[kHeaderBytes] = {};
        std::memcpy(now, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
        Recent<HeaderCopy>& r = recent_headers_[(header >> 3) & 63];
        HeaderCopy& c = r.key == header ? *r.value : headers_[header];
        r = {header, &c};
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        std::memcpy(sent, value, n);
        uint32_t head[2] = {uint32_t(At(sent_, field)), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    // what changed since the last command, in 64 byte pieces
    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        for (const Range& r : ranges_)
            for (size_t off = r.off, end = r.off + r.len; off < end; off += 64)
                Delta(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, std::min<size_t>(64, end - off));
        std::array<uint32_t, 4> textures;
        for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i]);
        Delta(&sent_.textures, &textures, sizeof(textures));
        std::array<uint32_t, 4> palettes;
        for (size_t i = 0; i < 4; i++) palettes[i] = Header(s.palettes[i]);
        Delta(&sent_.palettes, &palettes, sizeof(palettes));
        auto streams = s.streams;
        for (auto& st : streams) st.vb = Header(st.vb);
        Delta(&sent_.streams, &streams, sizeof(streams));
        Surface rt = s.rt, zs = s.zs;
        rt.header = Header(rt.header);
        zs.header = Header(zs.header);
        Delta(&sent_.rt, &rt, sizeof(rt));
        Delta(&sent_.zs, &zs, sizeof(zs));
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header) {
        if (!header) return;
        Recent<uint64_t>& r = recent_uses_[(header >> 3) & 63];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
    }
    void Written(const Surface& s) {
        if (s.header) written_.insert(mem::VirtualFromPhysical(s.data));
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = Clock::now();
        {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    void Run() {
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), L"render");
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            for (const Cmd& c : b->cmds) Execute(*b, c);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    // what a draw had that the replay moves halfway: vertex shader constants, matrices, pixel shader constants
    static constexpr int kLerpTransforms[] = {2, 3, 16, 17, 18, 19, 256};  // view, projection, texture 0-3, world
    struct DrawSnap : PrevDraw {
        uint64_t sig = 0;
        std::array<std::array<float, 4>, 16> psc;
    };

    // the same draw in the next frame: its primitive, shaders, texture and vertex data, target
    static uint64_t Sig(const State& s, const Cmd& c) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(c.a);
        mix(c.b);
        mix(c.c);
        mix(s.vs_handle);
        mix(s.ps ? s.ps->handle : 0);
        for (uint32_t t : s.textures) mix(t ? kernel::R32(t + 4) : 0);
        // the vertex data address stays out: a third of the draws take vertices the title rewrites each frame elsewhere
        for (int i = 0; i < 4; i++) mix(s.streams[i].stride);
        // the back buffers' memory rotates every swap: the target by its format and size
        mix(uint64_t(s.rt.format) << 32 | s.rt.size);
        return h;
    }

    static void Snap(const State& s, DrawSnap& d) {
        d.vsc = s.vsc;
        for (size_t i = 0; i < std::size(kLerpTransforms); i++) d.tf[i] = s.transforms[size_t(kLerpTransforms[i])];
        d.psc = s.psc;
    }

    static void ApplyDelta(State& st, const uint8_t* data, uint32_t size) {
        uint8_t* s = reinterpret_cast<uint8_t*>(&st);
        for (const uint8_t* p = data; p < data + size;) {
            uint32_t head[2];
            std::memcpy(head, p, 8);
            std::memcpy(s + head[0], p + 8, head[1]);
            p += 8 + head[1];
        }
    }

    // a command of this frame kept for the replay, its bytes and shaders with it
    void LogCommand(const Batch& b, const Cmd& c) {
        Cmd k = c;
        uint32_t n = c.op == Op::State || c.op == Op::Clear ? c.size : c.op == Op::Draw && c.d ? c.c * 2 : 0;
        k.data = uint32_t(log_bytes_.size());
        log_bytes_.insert(log_bytes_.end(), b.bytes.data() + c.data, b.bytes.data() + c.data + n);
        if (c.vs >= 0) {
            k.vs = int32_t(log_vs_.size());
            log_vs_.push_back(b.vs[size_t(c.vs)]);
        }
        if (c.ps >= 0) {
            k.ps = int32_t(log_ps_.size());
            log_ps_.push_back(b.ps[size_t(c.ps)]);
        }
        log_cmds_.push_back(k);
    }

    // the draw of the frame before this one is: same signature near the same place, the closest constants
    // cursor: where the last match put this frame's draws in the frame before, offset from the draw's own index
    const DrawSnap* Match(const DrawSnap& cur, size_t index, ptrdiff_t& offset) const {
        constexpr ptrdiff_t kBack = 16, kAhead = 32, kNear = 3;
        ptrdiff_t center = ptrdiff_t(index) + offset;
        size_t from = size_t(std::max<ptrdiff_t>(0, center - kBack));
        size_t to = size_t(std::clamp<ptrdiff_t>(center + kAhead, 0, ptrdiff_t(prev_snaps_.size())));
        const DrawSnap* best = nullptr;
        float best_d = 0;
        size_t best_k = 0;
        for (size_t k = from; k < to; k++) {
            const DrawSnap& p = prev_snaps_[k];
            if (p.sig != cur.sig) continue;
            float d = 0;
            for (size_t r = 0; r < p.vsc.size(); r++)
                for (int i = 0; i < 4; i++) d += std::fabs(p.vsc[r][i] - cur.vsc[r][i]);
            // away from where the draw order puts it, only an identical draw (nothing to move): a lookalike elsewhere on
            // the screen would pull it halfway there (the editor's selection box matched the tab's, the same sprite,
            // and the in-between pictures showed it between the two)
            if (d > 0 && std::abs(ptrdiff_t(k) - center) > kNear) continue;
            if (!best || d < best_d) {
                best = &p;
                best_d = d;
                best_k = k;
            }
        }
        if (best) offset = ptrdiff_t(best_k) - ptrdiff_t(index);
        return best;
    }

    static void Lerp(float* a, const float* b, size_t n) {
        for (size_t i = 0; i < n; i++)
            if (a[i] != b[i]) a[i] = 0.5f * (a[i] + b[i]);
    }

    // the frame's draws again, each halfway to its match in the frame before
    void Replay() {
        State rs = frame_start_;
        size_t di = 0, matched = 0;
        ptrdiff_t offset = 0;
        DrawSnap saved;
        for (const Cmd& c : log_cmds_) {
            const uint8_t* data = log_bytes_.data() + c.data;
            switch (c.op) {
                case Op::State:
                    ApplyDelta(rs, data, c.size);
                    if (c.vs >= 0) rs.vs = log_vs_[size_t(c.vs)];
                    if (c.ps >= 0) rs.ps = log_ps_[size_t(c.ps)];
                    break;
                case Op::Draw: {
                    const DrawSnap* prev = di < snaps_.size() ? Match(snaps_[di], di, offset) : nullptr;
                    di++;
                    uint32_t counting = rs.simple[kMCountEnable >> 2];
                    rs.simple[kMCountEnable >> 2] = 0;  // the title's visibility tests count its own frame only
                    if (prev) {
                        matched++;
                        Snap(rs, saved);
                        Lerp(&rs.vsc[0][0], &prev->vsc[0][0], rs.vsc.size() * 4);
                        for (size_t i = 0; i < std::size(kLerpTransforms); i++)
                            Lerp(rs.transforms[size_t(kLerpTransforms[i])].data(), prev->tf[i].data(), 16);
                        Lerp(&rs.psc[0][0], &prev->psc[0][0], rs.psc.size() * 4);
                    }
                    // motion vectors of the in-between picture: against the frame before, as shown before it
                    rs.prev = prev;
                    inner_->Draw(rs, c.a, c.b, c.c, c.d ? data : nullptr);
                    rs.prev = nullptr;
                    if (prev) {
                        rs.vsc = saved.vsc;
                        for (size_t i = 0; i < std::size(kLerpTransforms); i++)
                            rs.transforms[size_t(kLerpTransforms[i])] = saved.tf[i];
                        rs.psc = saved.psc;
                    }
                    rs.simple[kMCountEnable >> 2] = counting;
                    break;
                }
                case Op::Clear: inner_->Clear(rs, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
                case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
                default: break;
            }
        }
        stat_draws_ += uint32_t(di);
        stat_matched_ += uint32_t(matched);
    }

    // the presented surface is drawn, cleared or copied to by this frame's commands; else the picture reached it some
    // other way (written by the CPU, a read-back put back) and a replay would show what the surface held before: the
    // character editor's tattoo pages showed the skin colour page from earlier in the in-between pictures
    bool FrameDrawsTo(const Surface& front) const {
        State rs = frame_start_;
        for (const Cmd& c : log_cmds_) {
            if (c.op == Op::State) ApplyDelta(rs, log_bytes_.data() + c.data, c.size);
            else if ((c.op == Op::Draw || c.op == Op::Clear) && rs.rt.data == front.data) return true;
            else if (c.op == Op::Copy && c.s2.data == front.data) return true;
        }
        return false;
    }

    void EndFrameLog() {
        normal_offset_ = 0;
        prev_parts_.swap(cur_parts_);
        cur_parts_.clear();
        prev_snaps_.swap(snaps_);
        snaps_.clear();
        log_cmds_.clear();
        log_bytes_.clear();
        log_vs_.clear();
        log_ps_.clear();
        frame_open_ = false;
    }

    void SignalReplayed(uint64_t id) {
        {
            std::lock_guard lock(m_);
            replayed_.store(id);
        }
        done_cv_.notify_all();
    }

    // Present with interpolation: the frame halfway to the one before goes up now, this frame half a period later
    void PresentFrame(const Batch& b, const Cmd& c) {
        auto now = Clock::now();
        double since = last_present_at_.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - last_present_at_).count() : 0;
        last_present_at_ = now;
        if (since > 0 && since < 100) period_ms_ += (since - period_ms_) * 0.1;
        bool steady = since > 8 && since < 40 && period_ms_ > 8 && period_ms_ < 40;
        bool drawn = app::Get().interp && frame_open_ && FrameDrawsTo(c.s1);
        if (app::Get().interp && frame_open_ && !drawn) LOG_FIRST(4, "interp: the frame is not drawn to the surface it shows, no in-between picture");
        if (drawn && steady && !prev_snaps_.empty() && inner_->BeginReplay(c.s1)) {
            Replay();
            inner_->EndReplay();
            inner_->Present(c.s1);
            SignalReplayed(b.id);
            // FS2_INTERP_SO=1 (test): only the interpolated pictures are shown
            static const bool only = std::getenv("FS2_INTERP_SO") != nullptr;
            if (!only) {
                auto due = Clock::now() + std::chrono::microseconds(int64_t(period_ms_ * 500.0));
                while (Clock::now() + std::chrono::milliseconds(2) < due) Sleep(1);
                while (Clock::now() < due) std::this_thread::yield();
                inner_->PresentHeld();
            } else {
                inner_->EndReplay();
            }
            interp_count_++;
        } else {
            inner_->Present(c.s1);
            SignalReplayed(b.id);
        }
        frame_count_++;
        if (diag::Perf() && std::chrono::duration<double>(now - interp_log_at_).count() >= 1.0) {
            LOGI("interp: %u game frames, %u interpolated, period %.2f ms, %u of %u draws matched", frame_count_,
                 interp_count_, period_ms_, stat_matched_, stat_draws_);
            frame_count_ = interp_count_ = stat_matched_ = stat_draws_ = 0;
            if (part_total_) {
                LOGI("interp: same as the draw at that index the frame before, of %u: prim/start/count %u, shaders %u, "
                     "textures %u, vertex data %u, strides %u, target %u", part_total_, part_same_[0], part_same_[1],
                     part_same_[2], part_same_[3], part_same_[4], part_same_[5]);
                part_same_ = {};
                part_total_ = 0;
            }
            interp_log_at_ = now;
        }
        EndFrameLog();
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        bool interp = app::Get().interp;
        bool upscale = app::Get().upscaler != "off" && !app::Get().upscaler.empty();
        if (interp && c.op != Op::Present) {
            if (!frame_open_) {
                frame_start_ = state_;
                frame_open_ = true;
            }
            if (c.op == Op::State || c.op == Op::Draw || c.op == Op::Clear || c.op == Op::Copy) LogCommand(b, c);
        }
        switch (c.op) {
            case Op::State: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                for (const uint8_t* p = data; p < data + c.size;) {
                    uint32_t head[2];
                    std::memcpy(head, p, 8);
                    std::memcpy(s + head[0], p + 8, head[1]);
                    p += 8 + head[1];
                }
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                break;
            }
            case Op::Draw:
                if (interp || upscale) {
                    DrawSnap& d = snaps_.emplace_back();
                    d.sig = Sig(state_, c);
                    Snap(state_, d);
                    // under temporal anti-aliasing the match a frame before gives the motion vectors
                    if (upscale) state_.prev = Match(d, snaps_.size() - 1, normal_offset_);
                    static const bool sigdebug = std::getenv("FS2_SIGDEBUG") != nullptr;
                    if (sigdebug) {
                        std::array<uint64_t, 6> parts{};
                        parts[0] = uint64_t(c.a) << 48 ^ uint64_t(c.b) << 24 ^ c.c;
                        parts[1] = uint64_t(state_.vs_handle) << 32 ^ (state_.ps ? state_.ps->handle : 0);
                        for (uint32_t t : state_.textures) parts[2] = parts[2] * 31 + (t ? kernel::R32(t + 4) : 0);
                        for (int i = 0; i < 4; i++) parts[3] = parts[3] * 31 + (state_.streams[i].vb ? kernel::R32(state_.streams[i].vb + 4) : 0);
                        for (int i = 0; i < 4; i++) parts[4] = parts[4] * 31 + state_.streams[i].stride;
                        parts[5] = uint64_t(state_.rt.format) << 32 | state_.rt.size;
                        size_t i = cur_parts_.size();
                        cur_parts_.push_back(parts);
                        if (i < prev_parts_.size())
                            for (int k = 0; k < 6; k++) part_same_[k] += prev_parts_[i][k] == parts[k];
                        part_total_++;
                    }
                }
                inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                state_.prev = nullptr;
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: PresentFrame(b, c); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;

    // frame interpolation, on the render thread
    std::vector<DrawSnap> snaps_, prev_snaps_;
    std::vector<Cmd> log_cmds_;
    std::vector<uint8_t> log_bytes_;
    std::vector<std::shared_ptr<VertexShader>> log_vs_;
    std::vector<std::shared_ptr<PixelShader>> log_ps_;
    State frame_start_;
    bool frame_open_ = false;
    Clock::time_point last_present_at_{};
    double period_ms_ = 1000.0 / 60;
    ptrdiff_t normal_offset_ = 0;  // Match's offset for the frame's own draws (motion vectors)
    std::atomic<uint64_t> replayed_{0};
    uint32_t frame_count_ = 0, interp_count_ = 0, stat_draws_ = 0, stat_matched_ = 0;
    std::vector<std::array<uint64_t, 6>> cur_parts_, prev_parts_;  // FS2_SIGDEBUG
    std::array<uint32_t, 6> part_same_{};
    uint32_t part_total_ = 0;
    Clock::time_point interp_log_at_{};

    // the title's thread
    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;  // ring_pos_ after it was written
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    template <typename T>
    struct Recent {
        uint32_t key = 0;
        T* value = nullptr;
    };
    std::array<Recent<HeaderCopy>, 64> recent_headers_{};
    std::array<Recent<uint64_t>, 64> recent_uses_{};
    uint32_t ring_ = 0;
    uint64_t ring_pos_ = 0;
    Batch* cur_ = nullptr;
    State sent_;  // the state as the render thread will have it
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    std::unordered_set<uint64_t> logged_;   // perf diagnostic: read-backs named once, by address and kind
    uint64_t next_id_ = 0, last_present_ = 0;

    // the render thread
    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
