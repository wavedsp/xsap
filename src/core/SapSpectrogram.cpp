/************************************************************************
 *
 *  XSap - Shared analysis core: spectrogram
 *
 *  FILE:      core/SapSpectrogram.cpp
 *
 ************************************************************************/

#include "core/SapSpectrogram.h"

#include <algorithm>
#include <cmath>

#include "core/SapFft.h"

namespace xsap::core {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

SapSpectrogram::SapSpectrogram()
{
    m_zr.assign(static_cast<std::size_t>(kSpectrogramBins), 0.0f);
    m_zi.assign(static_cast<std::size_t>(kSpectrogramBins), 0.0f);
}

/*****************************************************************************
 * TransColor
 *
 * wsap DRAWAVE.CPP transColor(). Quadratic in dB and inverted, so a silent
 * bin is white and a full scale bin is black.
 *
 *****************************************************************************/
std::uint8_t SapSpectrogram::TransColor(float db) noexcept
{
    const float threshold = kSpectrogramUpLimitDb * kSpectrogramUpLimitDb;
    const float scaled = (db * db) * 256.0f / threshold;
    int value = 256 - static_cast<int>(scaled);
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255; // wsap clamped to 256 and overran its pen array
    return static_cast<std::uint8_t>(value);
}

/*****************************************************************************
 * EnsureTables
 *
 * Twiddles for the 256 point complex transform plus the untangle factors for
 * the 512 point real transform.
 *
 *****************************************************************************/
void SapSpectrogram::EnsureTables()
{
    if (m_tablesReady) {
        return;
    }

    const int halfSize = kSpectrogramSize / 2; // 256 complex points
    m_halfCos.resize(static_cast<std::size_t>(halfSize / 2));
    m_halfSin.resize(static_cast<std::size_t>(halfSize / 2));
    const double halfStep = kTwoPi / static_cast<double>(halfSize);
    for (int j = 0; j < halfSize / 2; j++) {
        const double angle = halfStep * static_cast<double>(j);
        m_halfCos[static_cast<std::size_t>(j)] = static_cast<float>(std::cos(angle));
        m_halfSin[static_cast<std::size_t>(j)] = static_cast<float>(std::sin(angle));
    }

    m_untangleCos.resize(static_cast<std::size_t>(halfSize) + 1);
    m_untangleSin.resize(static_cast<std::size_t>(halfSize) + 1);
    const double fullStep = kTwoPi / static_cast<double>(kSpectrogramSize);
    for (int k = 0; k <= halfSize; k++) {
        const double angle = fullStep * static_cast<double>(k);
        m_untangleCos[static_cast<std::size_t>(k)] = static_cast<float>(std::cos(angle));
        m_untangleSin[static_cast<std::size_t>(k)] = static_cast<float>(std::sin(angle));
    }

    m_tablesReady = true;
}

/*****************************************************************************
 * EnsureWindow
 *
 * Hamming window over the analysis length. wsap rebuilt this with cos() for
 * every column of every repaint.
 *
 *****************************************************************************/
void SapSpectrogram::EnsureWindow(int windowLength)
{
    if (m_windowLength == windowLength && !m_window.empty()) {
        return;
    }

    m_window.resize(static_cast<std::size_t>(windowLength));
    const double doubleConst = 6.2831853 / static_cast<double>(windowLength - 1);
    for (int k = 0; k < windowLength; k++) {
        m_window[static_cast<std::size_t>(k)] =
            0.54f - 0.46f * static_cast<float>(std::cos(static_cast<double>(k) * doubleConst));
    }
    m_windowLength = windowLength;
}

/*****************************************************************************
 * Build
 *
 * Column geometry follows wsap DRAWAVE.CPP dispSpectrum:
 *
 *     frameNum  = pixelWidth / 3
 *     frameSize = span / frameNum          (capped at 512, frameNum recomputed)
 *     windowLen = max(frameSize, 128)
 *
 * Each column transforms `windowLen` samples starting at the column's frame
 * boundary, so consecutive windows overlap whenever windowLen > frameSize.
 *
 *****************************************************************************/
bool SapSpectrogram::Build(const std::int16_t* samples,
                           std::size_t sampleCount,
                           std::size_t firstSample,
                           std::size_t spanSamples,
                           int pixelWidth,
                           SpectrogramImage& out)
{
    out.columns = 0;
    out.bins = kSpectrogramBins;
    out.grey.clear();

    if (samples == nullptr || sampleCount == 0 || spanSamples == 0 || pixelWidth <= 0) {
        return false;
    }
    if (firstSample >= sampleCount) {
        return false;
    }

    // Do not analyse past the end of the data.
    spanSamples = std::min(spanSamples, sampleCount - firstSample);
    if (spanSamples == 0) {
        return false;
    }

    int columns = pixelWidth / kSpectrogramColumnWidth;
    if (columns < 1)
        columns = 1;

    std::size_t frameSize = spanSamples / static_cast<std::size_t>(columns);
    if (frameSize < 1)
        frameSize = 1;
    if (frameSize > static_cast<std::size_t>(kSpectrogramMaxFrame)) {
        frameSize = static_cast<std::size_t>(kSpectrogramMaxFrame);
        columns = static_cast<int>(spanSamples / frameSize);
        if (columns < 1)
            columns = 1;
    }

    int windowLength = static_cast<int>(frameSize);
    if (windowLength < kSpectrogramMinWindow)
        windowLength = kSpectrogramMinWindow;
    if (windowLength > kSpectrogramSize)
        windowLength = kSpectrogramSize;

    EnsureTables();
    EnsureWindow(windowLength);

    try {
        out.grey.assign(static_cast<std::size_t>(columns) *
                            static_cast<std::size_t>(kSpectrogramBins),
                        TransColor(kSpectrogramLowLimitDb));
    } catch (const std::bad_alloc&) {
        return false;
    }

    const int halfSize = kSpectrogramSize / 2;
    const int mask = halfSize - 1;
    const float* window = m_window.data();

    for (int column = 0; column < columns; column++) {
        const std::size_t begin = firstSample + static_cast<std::size_t>(column) * frameSize;
        if (begin >= sampleCount) {
            break;
        }

        // Window and pack: even samples to the real part, odd to the imaginary.
        const std::size_t available = std::min(static_cast<std::size_t>(windowLength),
                                               sampleCount - begin);
        const int usable = static_cast<int>(available);
        const std::int16_t* src = samples + begin;

        const int pairs = usable / 2;
        for (int i = 0; i < pairs; i++) {
            m_zr[static_cast<std::size_t>(i)] =
                static_cast<float>(src[2 * i]) * window[2 * i];
            m_zi[static_cast<std::size_t>(i)] =
                static_cast<float>(src[2 * i + 1]) * window[2 * i + 1];
        }
        int packed = pairs;
        if (usable & 1) {
            m_zr[static_cast<std::size_t>(packed)] =
                static_cast<float>(src[usable - 1]) * window[usable - 1];
            m_zi[static_cast<std::size_t>(packed)] = 0.0f;
            packed++;
        }
        for (int i = packed; i < halfSize; i++) {
            m_zr[static_cast<std::size_t>(i)] = 0.0f;
            m_zi[static_cast<std::size_t>(i)] = 0.0f;
        }

        SapFft::FftTabled(m_zr.data(), m_zi.data(), kSpectrogramStage - 1,
                          m_halfCos.data(), m_halfSin.data());

        std::uint8_t* dest = out.grey.data() +
                             static_cast<std::size_t>(column) *
                                 static_cast<std::size_t>(kSpectrogramBins);

        for (int k = 0; k < kSpectrogramBins; k++) {
            const int km = (halfSize - k) & mask;

            const float zrk = m_zr[static_cast<std::size_t>(k)];
            const float zik = m_zi[static_cast<std::size_t>(k)];
            const float zrm = m_zr[static_cast<std::size_t>(km)];
            const float zim = m_zi[static_cast<std::size_t>(km)];

            const float er = 0.5f * (zrk + zrm);
            const float ei = 0.5f * (zik - zim);

            const float dr = zrk - zrm;
            const float di = zik + zim;
            const float orr = 0.5f * di;
            const float oi = -0.5f * dr;

            const float c = m_untangleCos[static_cast<std::size_t>(k)];
            const float s = m_untangleSin[static_cast<std::size_t>(k)];

            const float xr = er + (c * orr - s * oi);
            const float xi = ei + (s * orr + c * oi);

            const float power = xr * xr + xi * xi;
            // wsap: 10*log10(power + 1.0), the +1 keeps all-zero data finite.
            float db = 10.0f * std::log10(power + 1.0f);
            if (db > kSpectrogramUpLimitDb)
                db = kSpectrogramUpLimitDb;
            if (db < kSpectrogramLowLimitDb)
                db = kSpectrogramLowLimitDb;

            dest[k] = TransColor(db);
        }
    }

    out.columns = columns;
    return true;
}

} // namespace xsap::core
