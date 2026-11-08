/*
 * Songate · mood.cpp
 * See mood.hpp. The numbers were tuned on the user's library (tools: mixtrain mood, then a script that sorted and listed it).
 */
#include "mood.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>


namespace mood {

const char* name(int m) {
    static const char* n[Count] = { "Happy", "Party", "Relaxed", "Sad", "Aggressive", "Workout", "Focus", "Dark", "Epic" };
    return m >= 0 && m < Count ? n[m] : "";
}

const char* desc(int m) {
    static const char* d[Count] = { "Bright, upbeat, in a major key", "A strong beat to dance to", "Soft and unhurried",
                                    "Slow, quiet and in a minor key", "Loud, harsh and driving", "Fast, driving and steady",
                                    "Even and steady, to work to", "Minor, deep and brooding", "Big builds and drops" };
    return m >= 0 && m < Count ? d[m] : "";
}

namespace {

/** Each value's rank among the others, 0..1: the library's own spread. */
std::vector<float> ranks(const std::vector<float>& v) {
    std::vector<int> order(v.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return v[size_t(a)] < v[size_t(b)]; });   // (ties keep their order)
    std::vector<float> r(v.size());
    const float d = v.size() > 1 ? float(v.size() - 1) : 1.f;
    for (size_t p = 0; p < order.size(); p++) r[size_t(order[p])] = float(p) / d;
    return r;
}

/** How near a tempo is to `centre` (by `width` either way), folded by octaves; 0 without a steady beat. Dancing: 118 ± 40. */
float tempoFit(float b, float centre = 118, float width = 40) {
    if (b <= 0) return 0;
    while (b < 85) b *= 2;
    while (b > 170) b /= 2;
    return std::max(0.f, 1 - std::fabs(b - centre) / width);
}

}  // namespace

void Index::build(const std::vector<const mixan::Mood*>& tracks, const std::vector<Label>* labels) {
    best.assign(tracks.size(), -1);
    sound.assign(tracks.size(), -1);
    score.assign(tracks.size(), 0.f);
    known = 0;
    std::vector<int> idx;   // the tracks measured
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i] && tracks[i]->valid) idx.push_back(int(i));
    known = int(idx.size());
    if (idx.size() < 2) return;
    auto col = [&](auto get) {
        std::vector<float> v;
        v.reserve(idx.size());
        for (int i : idx) v.push_back(get(*tracks[size_t(i)]));
        return ranks(v);
    };
    const auto rLoud = col([](const mixan::Mood& m) { return m.loud; }), rDyn = col([](const mixan::Mood& m) { return m.dyn; }),
               rCalm = col([](const mixan::Mood& m) { return m.calm; }), rBright = col([](const mixan::Mood& m) { return m.bright; }),
               rHf = col([](const mixan::Mood& m) { return m.hf; }), rOnsets = col([](const mixan::Mood& m) { return m.onsets; }),
               rPerc = col([](const mixan::Mood& m) { return m.perc; }), rFlux = col([](const mixan::Mood& m) { return m.flux; }),
               rMinor = col([](const mixan::Mood& m) { return m.minor; }), rBass = col([](const mixan::Mood& m) { return m.bass; }),
               rBpm = col([](const mixan::Mood& m) { return m.bpm; }), rPulse = col([](const mixan::Mood& m) { return m.pulse; }),
               rKick = col([](const mixan::Mood& m) { return m.kickPulse; });
    // the five moods' raw scores, then each as a z-score over the library
    std::vector<float> raw[Count];
    for (auto& v : raw) v.resize(idx.size());
    for (size_t k = 0; k < idx.size(); k++) {
        const mixan::Mood& m = *tracks[size_t(idx[k])];
        const float fast = m.beat > 0.3f ? rBpm[k] : 0.3f;
        const float E = 0.22f * rOnsets[k] + 0.18f * rPerc[k] + 0.16f * rBright[k] + 0.12f * rFlux[k] + 0.12f * rLoud[k] +
                        0.12f * (1 - rCalm[k]) + 0.08f * fast;
        const float D = 0.40f * rPulse[k] + 0.25f * rKick[k] + 0.20f * tempoFit(m.bpm) + 0.15f * rBass[k];
        const float V = 0.50f * (1 - rMinor[k]) + 0.20f * rBright[k] + 0.30f * D;
        const float harsh = 0.35f * rHf[k] + 0.25f * rBright[k] + 0.2f * rFlux[k] + 0.2f * (1 - rDyn[k]);
        raw[Happy][k] = V * 0.55f + (1 - std::fabs(E - 0.6f)) * 0.3f + D * 0.15f;
        raw[Party][k] = D * 0.55f + E * 0.30f + rBass[k] * 0.15f;
        raw[Relaxed][k] = (1 - E) * 0.6f + V * 0.4f;
        raw[Sad][k] = (1 - E) * 0.45f + (1 - V) * 0.55f;
        raw[Aggressive][k] = E * 0.4f + harsh * 0.25f + (1 - V) * 0.25f + (1 - D) * 0.10f;
        // BETA 3's four (moodtry.py on the PC's 1,369 songs: 7 to 16 % each)
        const float wfit = m.beat > 0.3f ? tempoFit(m.bpm, 142, 22) : 0;
        raw[Workout][k] = E * 0.40f + wfit * 0.30f + rPulse[k] * 0.15f + rKick[k] * 0.15f;
        raw[Focus][k] = (1 - rDyn[k]) * 0.28f + (1 - rFlux[k]) * 0.24f + (1 - std::fabs(E - 0.4f)) * 0.2f + (1 - rHf[k]) * 0.13f + rPulse[k] * 0.15f;
        raw[Dark][k] = rMinor[k] * 0.40f + (1 - rBright[k]) * 0.25f + rBass[k] * 0.20f + (1 - std::fabs(E - 0.5f)) * 0.15f;
        raw[Epic][k] = rDyn[k] * 0.45f + rCalm[k] * 0.20f + rLoud[k] * 0.20f + rFlux[k] * 0.15f;
    }
    float mu[Count], sd[Count];
    for (int c = 0; c < Count; c++) {
        double s = 0, s2 = 0;
        for (float x : raw[c]) { s += x; s2 += double(x) * x; }
        mu[c] = float(s / double(idx.size()));
        sd[c] = float(std::sqrt(std::max(1e-12, s2 / double(idx.size()) - double(mu[c]) * mu[c])));
    }
    // your choices: the songs you set, each a point among the features; every song is pulled towards the moods of those near it
    // (a Gaussian of the distance, sigma 0.35 in rank units over the 14 features: a near-twin pulls fully, a stranger not at all)
    std::vector<std::array<float, 14>> feat(idx.size());
    for (size_t k = 0; k < idx.size(); k++)
        feat[k] = { rLoud[k], rDyn[k], rCalm[k], rBright[k], rHf[k], rOnsets[k], rPerc[k], rFlux[k], rMinor[k], rBass[k], rBpm[k], rPulse[k], rKick[k],
                    tempoFit(tracks[size_t(idx[k])]->bpm) };
    std::vector<size_t> anchors;
    if (labels)
        for (size_t k = 0; k < idx.size(); k++)
            if (size_t(idx[k]) < labels->size() && (*labels)[size_t(idx[k])].any()) anchors.push_back(k);
    constexpr float kSigma2 = 2 * 0.35f * 0.35f, kPull = 2.5f;
    for (size_t k = 0; k < idx.size(); k++) {
        float z[Count];
        for (int c = 0; c < Count; c++) z[c] = (raw[c][k] - mu[c]) / sd[c];
        int arg0 = 0;
        for (int c = 1; c < Count; c++) if (z[c] > z[arg0]) arg0 = c;
        sound[size_t(idx[k])] = static_cast<signed char>(arg0);
        if (!anchors.empty()) {
            float vote[Count] = {}, total = 1;   // (1: a prior, so one far choice moves little)
            for (size_t a : anchors) {
                if (a == k) continue;
                float d2 = 0;
                for (size_t f = 0; f < 14; f++) { const float d = feat[k][f] - feat[a][f]; d2 += d * d; }
                const float w = std::exp(-d2 / kSigma2);
                if (w < 1e-3f) continue;
                const Label& l = (*labels)[size_t(idx[a])];
                if (l.is >= 0) { vote[int(l.is)] += w; total += w; }
                for (int c = 0; c < Count; c++) if (l.nots & (1u << c)) { vote[c] -= 0.5f * w; total += 0.5f * w; }
            }
            for (int c = 0; c < Count; c++) z[c] += kPull * vote[c] / total;
        }
        // a song's own choice wins; a mood it is not, never
        const Label* own = labels && size_t(idx[k]) < labels->size() ? &(*labels)[size_t(idx[k])] : nullptr;
        int arg = -1;
        for (int c = 0; c < Count; c++) {
            if (own && (own->nots & (1u << c))) continue;
            if (arg < 0 || z[c] > z[arg]) arg = c;
        }
        float zs = arg >= 0 ? z[arg] : 0;
        if (own && own->is >= 0) { arg = own->is; zs = 99; }   // (the strongest of its list: yours)
        best[size_t(idx[k])] = static_cast<signed char>(arg);
        score[size_t(idx[k])] = zs;
    }
}

}  // namespace mood
