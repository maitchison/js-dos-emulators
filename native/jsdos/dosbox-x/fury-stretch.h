/* fury (fury-0du3s): the mixer's output stretched in time, so an emulator
 * that falls behind real time is heard as a moment played a little slower --
 * same pitch -- rather than a gap, and one that catches up as a moment a
 * little faster rather than a drop.
 *
 * Everything the emulator renders is pushed in; the audio device pulls what
 * real time asks for. While the queue between them sits near its target the
 * samples pass straight through, untouched. Away from it the output is built
 * by WSOLA: windowed grains of the input, overlap-added half a window apart,
 * each taken from wherever within +-DELTA of its due place joins the last
 * one best, and due places spaced by the hop times the read rate. The target
 * grows when a dip nearly empties the queue and eases back after a quiet
 * spell, so nothing needs setting. A stall longer than the queue fades out
 * at the end of the last grain and comes back once the queue has refilled.
 *
 * Mono floats: what js-dos's worklet is sent. */
#ifndef FURY_STRETCH_H
#define FURY_STRETCH_H

#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

class FuryStretch {
public:
    static constexpr int W = 512;       /* grain, samples (11.6 ms at 44.1k) */
    static constexpr int HS = W / 2;    /* output hop: grains overlap by half */
    static constexpr int DELTA = 128;   /* how far a grain may move to join well */

    explicit FuryStretch(int rate) { reset(rate); }

    void reset(int rate) {
        rateHz = rate;
        minTarget = rate * 46 / 1000;
        maxTarget = rate * 250 / 1000;
        target = minTarget;
        win.resize(W);
        for (int i = 0; i < W; i++) win[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / W);
        in.clear(); acc.assign(W, 0.f); fifo.clear(); fhead = 0;
        nominal = 0; prevNat = -1; started = false; fillAvg = 0;
        sinceBump = 0; bumped = false; gaps = stretched = squeezed = 0;
    }

    void push(float s) { in.push_back(s); }

    /* n samples of output, always */
    void pull(float *out, int n) {
        while ((int)(fifo.size() - fhead) < n) hop();
        for (int i = 0; i < n; i++) out[i] = fifo[fhead + i];
        fhead += n;
        if (fhead > 8192) { fifo.erase(fifo.begin(), fifo.begin() + fhead); fhead = 0; }
    }

    /* queued input not yet read, beyond the grain and search reserve a hop
       needs anyway: the cushion, which is what the target is held against */
    long fill() const { return (long)in.size() - (long)nominal - (W + DELTA); }
    long targetSamples() const { return target; }
    unsigned long gaps, stretched, squeezed;

private:
    int rateHz;
    long minTarget, maxTarget, target;
    std::vector<float> win, in, acc, fifo;
    size_t fhead;
    double nominal;     /* where the next grain is due in the input */
    long prevNat;       /* where the last grain's natural continuation starts, -1 none */
    bool started;
    long sinceBump;
    bool bumped;
    double fillAvg;     /* the fill averaged over ~100 ms: pushes and pulls come in lumps */

    void emit() {
        fifo.insert(fifo.end(), acc.begin(), acc.begin() + HS);
        std::copy(acc.begin() + HS, acc.end(), acc.begin());
        std::fill(acc.end() - HS, acc.end(), 0.f);
    }

    void addGrain(long at) {
        for (int i = 0; i < W; i++) acc[i] += win[i] * in[at + i];
    }

    /* one dip, one step: a stall asks on every hop it lasts */
    void grow() {
        if (sinceBump < rateHz / 2 && bumped) return;
        target = std::min(maxTarget, target * 5 / 4);
        sinceBump = 0;
        bumped = true;
    }

    void hop() {
        long f = fill();
        if (!started) {
            if (f >= target) { started = true; prevNat = -1; fillAvg = f; }
            else { emit(); return; }                      /* silence while it fills */
        }
        fillAvg += (f - fillAvg) * ((double)HS / (0.1 * rateHz));
        /* the queue is near empty: say so, and keep more from now on */
        if (f < target / 4) grow();
        sinceBump += HS;
        if (sinceBump > (long)rateHz * 20) { target = std::max(minTarget, target * 9 / 10); sinceBump = 0; }

        /* a long stall's backlog: skip back to the target, the grain search crossfades */
        if (f > target * 4 + rateHz / 2) nominal += (double)(f - target);

        /* the read rate, from the smoothed fill: 1 inside the dead band, so
           ordinary lumpiness never touches the sound */
        double e = (fillAvg - target) / (double)target;
        double r = 1.0;
        if (e > 0.5) r = std::min(1.6, 1.0 + 0.8 * (e - 0.5));
        else if (e < -0.4) r = std::max(0.5, 1.0 + 1.0 * (e + 0.4));

        long due = (long)std::floor(nominal);
        if (r == 1.0 && prevNat >= 0) {
            /* passthrough: the grain is the natural continuation, so the
               overlap-add gives back the input sample for sample */
            if (prevNat + W > (long)in.size()) { stall(); return; }
            addGrain(prevNat);
            nominal = (double)(prevNat + HS);
            prevNat += HS;
        } else {
            if (due + W + DELTA > (long)in.size()) { stall(); return; }
            long at = due;
            if (prevNat >= 0 && prevNat + W <= (long)in.size()) at = bestJoin(due);
            addGrain(at);
            prevNat = at + HS;
            nominal += HS * r;
            if (r < 1.0) stretched++; else if (r > 1.0) squeezed++;
        }
        emit();
        trim();
    }

    /* the start within +-DELTA of DUE whose first half matches the last
       grain's natural continuation best (normalised cross-correlation) */
    long bestJoin(long due) {
        long lo = std::max(0L, due - DELTA), hi = due + DELTA;
        if (hi + W > (long)in.size()) hi = (long)in.size() - W;
        const float *ref = &in[prevNat];
        double best = -2.0; long at = due;
        for (long k = lo; k <= hi; k += 2) {
            const float *c = &in[k];
            double xy = 0, yy = 0;
            for (int i = 0; i < HS; i += 2) { xy += ref[i] * c[i]; yy += c[i] * c[i]; }
            double s = xy / std::sqrt(yy + 1e-9);
            if (s > best) { best = s; at = k; }
        }
        return at;
    }

    /* nothing to read: the last grain's tail fades out, silence until the
       queue holds its target again, and more queue kept from now on */
    void stall() {
        gaps++;
        grow();
        started = false;
        emit();
    }

    void trim() {
        long keep = std::min((long)nominal, prevNat >= 0 ? prevNat : (long)nominal) - DELTA - W;
        if (keep > 16384) {
            in.erase(in.begin(), in.begin() + keep);
            nominal -= keep;
            if (prevNat >= 0) prevNat -= keep;
        }
    }
};

#endif
