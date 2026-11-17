/************************************************************************
 *
 *  XSap - Shared analysis core: spectrogram
 *
 *  FILE:      core/SapSpectrogram.h
 *
 *  PURPOSE:   The grey scale spectrogram that SAP shows when the visible
 *             span is wider than one transform's worth of samples.
 *
 *             This mode exists in the original C implementation
 *             (wsap DRAWAVE.CPP, dispSpectrum, the `datTp - datHp > 1024`
 *             branch) but was dropped in both the WinSap rewrite and
 *             wsap2025, which kept only the single frame line plot.
 *
 *             The analysis parameters are transcribed from that branch:
 *             one column every kColumnWidth pixels, a frame step of
 *             span/columns capped at kMaxFrame, an analysis window of at
 *             least kMinWindow samples, a 512 point real transform, 256
 *             bins, 10*log10(power + 1) clamped to [0, 128] dB, and the
 *             quadratic dB-to-grey mapping of transColor().
 *
 *             The rendering is not transcribed. The original selected a pen
 *             and issued MoveTo/LineTo for every pixel row of every column,
 *             and created and destroyed 256 pens per repaint; this builds a
 *             grey level image that the caller blits once.
 *
 ************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xsap::core {

// Transcribed from wsap DRAWAVE.CPP dispSpectrum / transColor.
inline constexpr int kSpectrogramColumnWidth = 3; // frameNum = len / 3
inline constexpr int kSpectrogramStage = 9;       // realFFT(ar, 9): 512 points
inline constexpr int kSpectrogramSize = 1 << kSpectrogramStage;
inline constexpr int kSpectrogramBins = kSpectrogramSize / 2; // 256
inline constexpr int kSpectrogramMinWindow = 128;
inline constexpr int kSpectrogramMaxFrame = 512;
inline constexpr float kSpectrogramUpLimitDb = 128.0f;
inline constexpr float kSpectrogramLowLimitDb = 0.0f;

// The span above which SAP switches from the line plot to the spectrogram.
inline constexpr std::size_t kSpectrogramThreshold = 1024;

// One grey level per (column, bin). Bin 0 is DC; the caller is responsible
// for flipping the axis so that low frequencies are drawn at the bottom.
struct SpectrogramImage {
    int columns = 0;
    int bins = kSpectrogramBins;
    std::vector<std::uint8_t> grey; // columns * bins, indexed [col * bins + bin]

    bool empty() const noexcept { return columns <= 0 || grey.empty(); }
};

class SapSpectrogram {
public:
    SapSpectrogram();

    // Analyses `spanSamples` samples starting at `firstSample` into a
    // spectrogram wide enough for a plot of `pixelWidth` pixels.
    //
    // Returns false when the request is degenerate or runs past the data, in
    // which case `out` is left empty.
    bool Build(const std::int16_t* samples,
               std::size_t sampleCount,
               std::size_t firstSample,
               std::size_t spanSamples,
               int pixelWidth,
               SpectrogramImage& out);

    // wsap transColor: quadratic, inverted, so that silence is white and a
    // full scale bin is black. The original clamped to 256 and then indexed a
    // 256 entry pen array, which is one past the end; this clamps to 255.
    static std::uint8_t TransColor(float db) noexcept;

private:
    void EnsureTables();
    void EnsureWindow(int windowLength);

    // Tables for the 512 point real transform (a 256 point complex FFT).
    bool m_tablesReady = false;
    std::vector<float> m_halfCos;
    std::vector<float> m_halfSin;
    std::vector<float> m_untangleCos;
    std::vector<float> m_untangleSin;

    int m_windowLength = 0;
    std::vector<float> m_window;

    std::vector<float> m_zr;
    std::vector<float> m_zi;
};

} // namespace xsap::core
