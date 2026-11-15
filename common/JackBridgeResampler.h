/*
Copyright (C) 2026 jack2-nspa

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

#ifndef __JackBridgeResampler__
#define __JackBridgeResampler__

#include <stddef.h>
#include <stdint.h>

namespace Jack
{

/*!
\brief Variable-ratio resampler joining an independently clocked sound card
 to the graph.

 Polyphase Kaiser-windowed sinc, cutoff at the Nyquist frequency of the lower
 rate. The coefficient row of the exact phase is linearly interpolated between
 the two nearest table phases once per output frame and shared by all
 channels (one dot product per channel). AVX2 + FMA when the CPU has it,
 else SSE2 on x86-64, else portable C++; an AVX-512 path exists for
 measurement (ForceVariant()).

 quality  taps  phases  band edge (x lower rate)  worst SNR in band, 48 kHz drift
    0       32    256    0.42 (20 kHz at 48k)      77 dB
    1       56   1024    0.42                     123 dB
    2       96   2048    0.45 (20 kHz at 44.1k)   129 dB
 (taps at ratios <= 1; downsampling stretches the kernel and the taps by the
 ratio). Content between the band edge and the lower Nyquist is attenuated,
 and its images or aliases land below (lower rate - band edge): ultrasonic at
 44.1 and 48 kHz.

 Positions are absolute input frames: a 64-bit frame count plus a 56-bit
 fraction. Each output is made at the current centre, then the ratio r moves
 towards the target t by floor(|t - r| * k / 2^32) (k = SmoothingCoefficient(),
 snapping to t once that is 0, all in units of 2^-56), then the centre
 advances by r. All of it is integer arithmetic, so Center() is the exact sum
 of the ratios applied, Pull and NeededFor agree bit for bit and nothing
 drifts, however long it runs.

 Output k is the band-limited value of the input at Center() at the time it
 is produced: zero group delay relative to Center(). It needs the input
 frames up to floor(Center()) + HalfLength(). At a ratio of exactly 1 and an
 integer Center() the filter is the identity (the output is a bit-exact copy
 of the input); there is no separate copy path, so nothing jumps when the
 ratio leaves 1.

 Init() allocates; everything else is realtime-safe (no allocation, lock,
 system call or logging). Not thread-safe: Push and Pull must be serialized
 by the caller.
*/
class JackBridgeResampler
{
    public:

        enum { kFracBits = 56 };  //!< fractional bits of positions and ratios

        //! Implementations of the inner loop (ForceVariant()).
        enum Variant { kPortable = 0, kSse2 = 1, kAvx2 = 2, kAvx512 = 3 };

        JackBridgeResampler();
        ~JackBridgeResampler();

        /*!
         Non-RT. nominal_ratio is input frames per output frame (about 1 for
         drift only, 48000/44100 = 1.088 for 48k to 44.1k), within [0.2, 5].
         quality: 0 low, 1 normal (default), 2 high. max_in/max_out: the
         largest Push/Pull block. Returns false on bad arguments or when out
         of memory. Can be called again to reconfigure.
        */
        bool Init(int channels, double nominal_ratio, int quality, int max_in, int max_out);

        //! Empty history, positions to 0, ratio = nominal.
        void Reset();

        //! Time constant of the ratio smoothing in output samples (default 100, <= 0: none).
        void SetRatioSmoothing(int samples);

        //! Append input (copied). Returns the frames taken: all, unless the buffer is full.
        int Push(const float* const* in, int frames);

        /*!
         Produce exactly n outputs (n <= max_out) at the target ratio if
         enough input is pushed and return n; otherwise return 0 and change
         nothing. The target is clamped to nominal * [0.8, 1.25].
        */
        int Pull(float* const* out, int n, double target_ratio);

        //! Produce as many outputs as the pushed input allows, up to max_n (<= max_out).
        int PullAvailable(float* const* out, int max_n, double target_ratio);

        //! Total input frames pushed since Reset().
        int64_t Pushed() const { return fPushed; }

        //! Absolute input position (frames) of the centre of the next output.
        double Center() const;

        //! Center() exactly: frames + frac / 2^kFracBits.
        void CenterExact(int64_t* frames, uint64_t* frac) const
        {
            *frames = fIndex;
            *frac = fFrac;
        }

        //! Taps on each side, in input frames: from the centre to the newest input needed.
        int HalfLength() const { return fHalf; }

        //! Pushed() needed for Pull(n, ratio) to succeed now.
        int64_t NeededFor(int n, double ratio) const;

        //! The smoothed ratio applied to the last output (nominal after Reset).
        double CurrentRatio() const;

        //! The smoothed ratio exactly, in units of 2^-kFracBits.
        uint64_t CurrentRatioExact() const { return fRatio; }

        //! Exact fixed-point value Pull uses for a target ratio (after clamping).
        uint64_t TargetExact(double ratio) const;

        //! k of the smoothing step above, round((1 - exp(-1 / samples)) * 2^32); 0: no smoothing.
        uint32_t SmoothingCoefficient() const { return fSmoothK; }

        int Taps() const { return fTaps; }          //!< filter length (2 * HalfLength())
        int Phases() const { return 1 << fPhaseBits; }
        bool UsesSimd() const { return fSimd; }

        //! Testing: use the portable path even if the CPU has a SIMD path.
        void ForceScalar(bool scalar);

        /*!
         Measurement: make `variant` the SIMD path (the one ForceScalar(false)
         selects) if this CPU has it, kPortable for none; returns false and
         changes nothing otherwise. Init() sets the default (kAvx2 with AVX2 +
         FMA, else kSse2 on x86-64, else kPortable), or the variant named by the
         environment variable JACK_BRIDGE_RESAMPLER (portable, sse2, avx2,
         avx512) if set and supported.
        */
        bool ForceVariant(int variant);

        //! The variant Pull uses now.
        int ActiveVariant() const { return fSimd ? fSimdVariant : (int)kPortable; }

        static const char* VariantName(int variant);

    private:

        int fChannels;
        int fTaps;              // N = 2 * fHalf, a multiple of 8
        int fHalf;
        int fPhaseBits;         // log2 of the number of table phases
        int fCapacity;          // frames per channel in the history buffer
        size_t fStride;         // floats between channels in the history buffer
        float* fTable;          // (phases + 1) rows of fTaps coefficients, 32-byte aligned
        float* fHistory;        // fChannels rows of fCapacity frames, 32-byte aligned
        void* fTableMem;
        void* fHistoryMem;

        int64_t fPushed;        // frames pushed since Reset
        int64_t fBase;          // absolute frame stored in fHistory[0]
        int64_t fIndex;         // integer part of the centre
        uint64_t fFrac;         // fractional part of the centre, 2^-56 units
        uint64_t fRatio;        // smoothed ratio, 2^-56 units
        uint64_t fNominal;
        uint64_t fMinRatio;
        uint64_t fMaxRatio;
        uint32_t fSmoothK;      // 1 - exp(-1 / tau), 2^-32 units; 0: no smoothing
        bool fSimd;             // fSimdVariant in use (else the portable path)
        int fSimdVariant;       // Variant; kPortable: no SIMD path

        int64_t Needed(int n, uint64_t target) const;
        int Produce(float* const* out, int n, uint64_t target);
        void Release();

        JackBridgeResampler(const JackBridgeResampler&);
        JackBridgeResampler& operator=(const JackBridgeResampler&);
};

} // end of namespace

#endif
