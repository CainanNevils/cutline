#pragma once
// D3D11 timestamp queries, ring-buffered so reading never stalls the pipeline.
#include "render/gpu.h"

#include <array>

namespace cut {

class GpuTimer {
public:
    void init(ID3D11Device* dev) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        for (auto& s : slots_) {
            d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            dev->CreateQuery(&d, &s.disjoint);
            d.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&d, &s.begin);
            dev->CreateQuery(&d, &s.end);
        }
    }
    void begin(ID3D11DeviceContext* ctx) {
        auto& s = slots_[cur_ % slots_.size()];
        ctx->Begin(s.disjoint.Get());
        ctx->End(s.begin.Get());
    }
    void end(ID3D11DeviceContext* ctx) {
        auto& s = slots_[cur_ % slots_.size()];
        ctx->End(s.end.Get());
        ctx->End(s.disjoint.Get());
        s.pending = true;
        ++cur_;
    }
    // Most recent completed measurement in ms (non-blocking; -1 if none ready).
    double poll(ID3D11DeviceContext* ctx) {
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& s = slots_[(cur_ + i) % slots_.size()];
            if (!s.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
            if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            if (ctx->GetData(s.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (ctx->GetData(s.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            s.pending = false;
            if (!dj.Disjoint && dj.Frequency) last_ = double(b - a) * 1000.0 / double(dj.Frequency);
            ++completed_;
        }
        return last_;
    }
    uint64_t completed() const { return completed_; }
    double last() const { return last_; }

private:
    struct Slot {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    std::array<Slot, 6> slots_;
    size_t cur_ = 0;
    double last_ = -1;
    uint64_t completed_ = 0;
};

} // namespace cut
