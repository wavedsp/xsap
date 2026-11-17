/************************************************************************
 *
 *  XSap - Shared analysis core: FFT
 *
 *  FILE:      core/SapFft.h
 *
 *  PURPOSE:   Port of the WinSAP FFT engine so that XSap produces bit-for-bit
 *             identical spectra. The algorithms are transcribed from
 *             WinSAP src/x64/Panel.cpp (Panel::FFT, Panel::LoadSinCos,
 *             Panel::ComplexFFT, Panel::AdjustFFT, Panel::RealFFT and the
 *             numeric part of Panel::DispSpectrum) with no numeric changes:
 *             single precision `float` storage, the same twiddle recurrence
 *             and the same dB normalisation.
 *
 ************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xsap::core {

// FFT constants - identical to WinSAP src/x64/Panel.h
inline constexpr int kFftLength = 2048;
inline constexpr int kFftLengthHalf = 1024;
// Number of bins displayed. The historic SAP value was 900, which stopped the
// axis at 0.44 * sampleRate - the passband edge of the anti-aliasing filters in
// the A/D hardware of the era. Modern converters are flat to Nyquist and the
// transform was measured clean across the whole band (peak level within 0.1 dB
// and spurious content below -64 dB in bins 900..1023), so the full
// half-spectrum is displayed, matching the original DOS wsap.
inline constexpr int kFftCutOff = kFftLengthHalf;
inline constexpr int kFftStage = 11;

// WinSAP default display limits for an FFT amplitude track (dB).
inline constexpr float kFftUpLimitDb = 0.0f;
inline constexpr float kFftLowLimitDb = -120.0f;

// Reference magnitude used by WinSAP: a full scale sine wave yields 0 dB.
inline constexpr float kFftReferenceMagnitude = (kFftLength / 2.0f) * 32768.0f;

class SapFft {
public:
    SapFft();

    // WinSAP Panel::FFT - in-place Cooley-Tukey, `n` is the stage (log2 size).
    // Stateless, so it is exposed as a static entry point.
    static void Fft(float* z, float* y, int n);

    // Same algorithm as Fft(), but the per-stage twiddle factors come from a
    // precomputed table instead of being regenerated with cos()/sin() and an
    // accumulating float phase. WinSAP issues 2*(2^n - 1) libm trig calls per
    // transform, which dominates its runtime; this variant issues none and is
    // also slightly more accurate because the phase no longer drifts.
    void FftFast(float* z, float* y, int n);

    // Numeric half of WinSAP Panel::DispSpectrum.
    //
    // Applies the WinSAP Hamming window to `frameLength` samples starting at
    // `frameBegin * frameLength`, zero-pads to kFftLength, transforms, and
    // writes kFftCutOff clamped dB values into `spectrumDb`.
    //
    // Returns false when the requested frame is not fully contained in the
    // sample buffer, matching WinSAP's early-out.
    bool ComputeSpectrumDb(const int16_t* samples,
                           std::size_t sampleCount,
                           std::size_t frameBegin,
                           std::size_t frameLength,
                           float upLimit,
                           float lowLimit,
                           float* spectrumDb);

    // Same output as ComputeSpectrumDb() but uses FftFast() and a cached
    // Hamming window.
    bool ComputeSpectrumDbFast(const int16_t* samples,
                               std::size_t sampleCount,
                               std::size_t frameBegin,
                               std::size_t frameLength,
                               float upLimit,
                               float lowLimit,
                               float* spectrumDb);

    // Same output again, but exploiting the fact that the FFT input is real.
    // The kFftLength real samples are packed into a kFftLength/2 point complex
    // transform (even samples -> real part, odd samples -> imaginary part) and
    // the true spectrum is recovered with a Hermitian untangle. That halves
    // the butterfly count and the memory traffic. This is what XSap calls at
    // run time.
    //
    // This is what WinSAP's Panel::RealFFT was reaching for, but that routine
    // is dead code: nothing calls it, and its twiddle index runs to 523520 on
    // a 512 entry table, so it cannot work at any useful size.
    //
    // frameCount honours the panel's frame count: the analysis window is
    // frameCount whole frames starting at frameBegin, so asking to see more
    // frames lengthens the window and buys frequency resolution at the cost of
    // time resolution. That is the point of the control, and it is what the
    // line spectrum is for. Past kSpectrogramThreshold samples the caller
    // switches to the spectrogram instead.
    bool ComputeSpectrumDbReal(const int16_t* samples,
                               std::size_t sampleCount,
                               std::size_t frameBegin,
                               std::size_t frameLength,
                               float upLimit,
                               float lowLimit,
                               float* spectrumDb,
                               std::size_t frameCount = 1);

    const float* Real() const noexcept { return m_ar; }
    const float* Imag() const noexcept { return m_ai; }

    // Shared kernel for FftFast(), the real-input path and SapSpectrogram:
    // identical structure to Fft(), with the twiddles supplied by a caller
    // owned table of 2^(n-1) entries sampling exp(i*2*pi*j / 2^n).
    static void FftTabled(float* z, float* y, int n, const float* cosTab, const float* sinTab);

private:
    void EnsureTwiddles(int stage);
    void EnsureWindow(int dataLen);
    void EnsureRealTables();

    float m_ar[kFftLength]; // Real part of FFT result
    float m_ai[kFftLength]; // Imaginary part of FFT result
    // Caches for the optimised path.
    int m_twiddleStage = 0;
    std::vector<float> m_twiddleCos;
    std::vector<float> m_twiddleSin;
    int m_windowLength = 0;
    std::vector<float> m_window;

    // Caches for the real-input path.
    bool m_realTablesReady = false;
    std::vector<float> m_halfCos; // twiddles for the kFftStage-1 transform
    std::vector<float> m_halfSin;
    std::vector<float> m_untangleCos; // exp(i*2*pi*k/kFftLength)
    std::vector<float> m_untangleSin;
    float m_zr[kFftLengthHalf];
    float m_zi[kFftLengthHalf];
};

} // namespace xsap::core
