#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cut {

double clipGainAt(const Clip& c, Tick local) {
    double db = c.gainDb + (c.volume.type.empty() ? 0.0 : c.volume.p("level", local));
    double g = dbToGain(db);
    if (c.fadeIn > 0 && local < c.fadeIn) g *= std::clamp(double(local) / double(c.fadeIn), 0.0, 1.0);
    if (c.fadeOut > 0 && local > c.duration - c.fadeOut)
        g *= std::clamp(double(c.duration - local) / double(c.fadeOut), 0.0, 1.0);
    return g;
}

ClipAudio* Mixer::clipAudio(const RenderSnapshot& snap, const Clip& c, int64_t from) {
    ClipState& st = clips_[c.id];
    st.lastUsed = blockCounter_;
    if (st.audio || st.failed) return st.audio.get();
    // Adopt the reader of a clip of the same media that has finished playing (the previous
    // piece of a split clip or speed ramp): reading continues where it stopped instead of
    // opening and seeking the file again at every cut.
    for (auto& [id, other] : clips_)
        if (id != c.id && other.audio && other.asset == c.assetId && other.stream == c.stream && other.endSample <= from &&
            other.renderedTo >= other.endSample) {  // (finished: its last samples are rendered too)
            st.audio = std::move(other.audio);
            st.asset = c.assetId;
            st.stream = c.stream;
            return st.audio.get();
        }
    const AssetRef* a = snap.asset(c.assetId);
    if (!a || a->offline || c.stream < 0 || size_t(c.stream) >= a->info.audio.size()) {
        st.failed = true;
        return nullptr;
    }
    if (std::getenv("CUTLINE_MIX_LOG"))
        std::fprintf(stderr, "mixer: opening audio for clip %llu (asset %llu stream %d) at %.3f s\n", (unsigned long long)c.id,
                     (unsigned long long)c.assetId, c.stream, from / 48000.0);
    auto ca = std::make_unique<ClipAudio>();
    std::string err;
    if (!ca->open(a->path, a->info.audio[size_t(c.stream)].index, a->info.origin, err)) {
        st.failed = true;
        return nullptr;
    }
    st.audio = std::move(ca);
    st.asset = c.assetId;
    st.stream = c.stream;
    return st.audio.get();
}

void Mixer::render(const RenderSnapshot& snap, int64_t ts, float* out, int n) {
    ++blockCounter_;
    std::memset(out, 0, size_t(n) * 8);
    const Sequence& seq = snap.seq;
    trackBuf_.assign(size_t(n) * 2, 0.f);
    clipBuf_.resize(size_t(n) * 2);
    const Tick t0 = samplesToTicks(ts, 48000);
    const Tick t1 = samplesToTicks(ts + n, 48000);
    bool anySolo = false;
    for (const auto& tr : seq.tracks)
        if (tr.kind == TrackKind::Audio && tr.solo) anySolo = true;

    MeterSnapshot local;
    float mpk[2] = {0, 0};
    std::vector<Id> produced;  // tracks with audio in this block (their buffers in trackOut_)
    for (const auto& tr : seq.tracks) {
        if (tr.kind != TrackKind::Audio) continue;
        std::array<float, 2> tpk{0, 0};
        const bool audible = !tr.muted && (!anySolo || tr.solo);
        if (!audible) {
            local.tracks.push_back({tr.id, tpk});
            continue;
        }
        std::fill(trackBuf_.begin(), trackBuf_.end(), 0.f);
        bool any = false;
        // in time order: a piece that continues the previous one (split / speed ramp) takes over its
        // reader in the block where they meet, after the previous one has rendered its last samples
        for (const Clip* cp : seq.clipsOnTrack(tr.id)) {
            const Clip& c = *cp;
            if (!c.enabled || c.kind == ClipKind::Text) continue;
            // audible range including crossfade handles
            Tick a = c.start, b = c.end();
            const Transition* tin = nullptr;
            const Transition* tout = nullptr;
            TickRange rin{}, rout{};
            for (const auto& x : seq.transitions) {
                if (x.trackId != tr.id) continue;
                if (x.clipB == c.id) {
                    tin = &x;
                    rin = transitionRange(seq, x);
                    a = std::min(a, rin.a);
                }
                if (x.clipA == c.id) {
                    tout = &x;
                    rout = transitionRange(seq, x);
                    b = std::max(b, rout.b);
                }
            }
            if (b <= t0 || a >= t1) continue;
            int64_t s0 = std::max(ts, ticksToSamples(a, 48000));
            int64_t s1 = std::min(ts + n, ticksToSamples(b, 48000));
            if (s1 <= s0) continue;
            int cnt = int(s1 - s0);
            if (c.kind == ClipKind::Nested) {
                // the nested sequence's whole mix, from the clip's source time (speed 1)
                auto it = snap.nested.find(c.nestedSeq);
                if (it == snap.nested.end()) continue;
                ClipState& cs = clips_[c.id];
                cs.lastUsed = blockCounter_;
                if (!cs.sub) cs.sub = std::make_unique<Mixer>();
                const int64_t src = ticksToSamples(c.sourceIn, 48000) + (s0 - ticksToSamples(c.start, 48000)) + ClipFxChain::latency(c);
                cs.sub->render(*it->second, std::max<int64_t>(0, src), clipBuf_.data(), cnt);
            } else {
                ClipAudio* ca = clipAudio(snap, c, s0);
                if (!ca) continue;
                ca->render(c, s0 + ClipFxChain::latency(c), clipBuf_.data(), cnt);  // (effects delay it back)
                ClipState& done = clips_[c.id];
                done.endSample = ticksToSamples(b, 48000);
                done.renderedTo = s1;
            }
            if (hasAudioEffects(c)) {
                ClipState& cs = clips_[c.id];
                if (cs.nextSample != s0) cs.fx.reset();  // jumped: filters / envelopes start fresh
                cs.fx.process(c, samplesToTicks(s0, 48000) - c.start, clipBuf_.data(), cnt);
                cs.nextSample = s0 + cnt;
            }
            // Gain envelope (keyframes, fades, constant-power crossfade) sampled every 32 samples
            // and linearly interpolated in between.
            auto gainAt = [&](int64_t s) {
                Tick t = samplesToTicks(s, 48000);
                const bool inXf = tin && rin.contains(t), outXf = tout && rout.contains(t);
                if ((t < c.start && !inXf) || (t >= c.end() && !outXf)) return 0.0;
                double g = clipGainAt(c, std::clamp<Tick>(t - c.start, 0, c.duration));
                if (inXf) g *= std::sin(double(t - rin.a) / double(rin.len()) * 1.5707963267948966);
                if (outXf) g *= std::cos(double(t - rout.a) / double(rout.len()) * 1.5707963267948966);
                return g;
            };
            const int step = 32;
            const int npts = cnt / step + 2;
            gains_.resize(size_t(npts));
            for (int p = 0; p < npts; ++p) gains_[size_t(p)] = gainAt(s0 + std::min(p * step, cnt));
            float* dst = trackBuf_.data() + size_t(s0 - ts) * 2;
            for (int k = 0; k < cnt; ++k) {
                const int p = k / step;
                const int ka = p * step, kb = std::min(ka + step, cnt);
                const double f = kb > ka ? double(k - ka) / double(kb - ka) : 0.0;
                const float g = float(gains_[size_t(p)] + (gains_[size_t(p) + 1] - gains_[size_t(p)]) * f);
                dst[size_t(k) * 2] += clipBuf_[size_t(k) * 2] * g;
                dst[size_t(k) * 2 + 1] += clipBuf_[size_t(k) * 2 + 1] * g;
            }
            any = true;
        }
        if (any) {
            const double g = dbToGain(tr.volumeDb);
            const double pan = std::clamp(tr.pan, -1.0, 1.0);
            const float gl = float(g * std::min(1.0, 1.0 - pan)), gr = float(g * std::min(1.0, 1.0 + pan));
            std::vector<float>& tb = trackOut_[tr.id];
            tb.resize(size_t(n) * 2);
            for (int i = 0; i < n; ++i) {
                tb[size_t(i) * 2] = trackBuf_[size_t(i) * 2] * gl;
                tb[size_t(i) * 2 + 1] = trackBuf_[size_t(i) * 2 + 1] * gr;
            }
            produced.push_back(tr.id);
        }
        local.tracks.push_back({tr.id, tpk});
    }
    // ducking: a track follows its key track's level (after the key's fader), then everything sums
    for (Id id : produced) {
        const Track* tr = seq.track(id);
        std::vector<float>& tb = trackOut_[id];
        if (tr && tr->duckUnder) {
            const auto key = std::find(produced.begin(), produced.end(), tr->duckUnder);
            duck(id, key != produced.end() ? trackOut_[tr->duckUnder].data() : nullptr, tr->duckDb, ts, tb.data(), n);
        }
        std::array<float, 2> tpk{0, 0};
        for (int i = 0; i < n; ++i) {
            const float l = tb[size_t(i) * 2], r = tb[size_t(i) * 2 + 1];
            tpk[0] = std::max(tpk[0], std::abs(l));
            tpk[1] = std::max(tpk[1], std::abs(r));
            out[i * 2] += l;
            out[i * 2 + 1] += r;
        }
        for (auto& [tid, pk] : local.tracks)
            if (tid == id) pk = tpk;
    }
    const float mg = float(dbToGain(seq.masterVolumeDb));
    for (int i = 0; i < n; ++i) {
        out[i * 2] *= mg;
        out[i * 2 + 1] *= mg;
        mpk[0] = std::max(mpk[0], std::abs(out[i * 2]));
        mpk[1] = std::max(mpk[1], std::abs(out[i * 2 + 1]));
    }
    if (loudReset_.exchange(false)) loud_.reset();
    loud_.push(out, n);
    // integrated() walks every block: refresh it about twice a second
    const bool updIntegrated = (++loudSteps_ % 50) == 0;
    const double integrated = updIntegrated ? loud_.integrated() : 0;
    std::lock_guard<std::mutex> l(meterMutex_);
    meters_.momentary = loud_.momentary();
    meters_.shortTerm = loud_.shortTerm();
    if (updIntegrated) meters_.integrated = integrated;
    if (meters_.tracks.size() != local.tracks.size()) meters_.tracks = local.tracks;
    else
        for (size_t i = 0; i < local.tracks.size(); ++i) {
            meters_.tracks[i].first = local.tracks[i].first;
            for (int ch = 0; ch < 2; ++ch)
                meters_.tracks[i].second[ch] = std::max(meters_.tracks[i].second[ch], local.tracks[i].second[ch]);
        }
    meters_.master[0] = std::max(meters_.master[0], mpk[0]);
    meters_.master[1] = std::max(meters_.master[1], mpk[1]);
}

MeterSnapshot Mixer::takeMeters() {
    std::lock_guard<std::mutex> l(meterMutex_);
    MeterSnapshot m = meters_;
    for (auto& t : meters_.tracks) t.second = {0, 0};
    meters_.master = {0, 0};
    return m;
}

void Mixer::duck(Id track, const float* key, double duckDb, int64_t ts, float* buf, int n) {
    DuckState& d = ducks_[track];
    if (d.next != ts) d = DuckState{};  // jumped: start undocked
    d.next = ts + n;
    constexpr double thr = 0.01;  // -40 dBFS on the key
    const double floorGain = dbToGain(std::min(0.0, duckDb));
    const double det = std::exp(-1.0 / (0.010 * 48000)), down = std::exp(-1.0 / (0.080 * 48000)),
                 up = std::exp(-1.0 / (0.400 * 48000));
    for (int i = 0; i < n; ++i) {
        const double x = key ? std::max(std::abs(key[i * 2]), std::abs(key[i * 2 + 1])) : 0.0;
        d.env = std::max(x, det * d.env);
        double target = 1.0;
        if (d.env > thr) {
            target = floorGain;
            d.hold = int(0.25 * 48000);  // stay down through short pauses between words
        } else if (d.hold > 0) {
            --d.hold;
            target = floorGain;
        }
        const double k = target < d.gain ? down : up;
        d.gain = k * d.gain + (1 - k) * target;
        buf[i * 2] = float(buf[i * 2] * d.gain);
        buf[i * 2 + 1] = float(buf[i * 2 + 1] * d.gain);
    }
}

void Mixer::reset() {
    // a new playback session: keep the open readers (opening a file costs ~60 ms, which would
    // delay every Play) but forget positions, effect / ducking state and the handoff bookkeeping;
    // readers seek on their first read
    for (auto& [id, st] : clips_) {
        st.fx.reset();
        st.nextSample = -1;
        st.endSample = INT64_MAX;
        st.renderedTo = INT64_MIN;
        st.failed = false;
        if (st.sub) st.sub->reset();
    }
    ducks_.clear();
}

void Mixer::resetLoudness() {
    loudReset_ = true;
    std::lock_guard<std::mutex> l(meterMutex_);
    meters_.momentary = meters_.shortTerm = meters_.integrated = -1e9;
}

void Mixer::prune() {
    for (auto it = clips_.begin(); it != clips_.end();) {
        if (blockCounter_ - it->second.lastUsed > 2000) it = clips_.erase(it);
        else ++it;
    }
}

} // namespace cut
