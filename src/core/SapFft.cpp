/************************************************************************
 *
 *  XSap - Shared analysis core: FFT
 *
 *  FILE:      core/SapFft.cpp
 *
 *  PURPOSE:   Transcription of the WinSAP FFT engine (WinSAP
 *             src/x64/Panel.cpp). Loop structure, operation order and the
 *             `float` working precision are preserved verbatim so that XSap
 *             and WinSAP compute the same spectrum values.
 *
 ************************************************************************/

#include "core/SapFft.h"

#include <cmath>

namespace xsap::core {

SapFft::SapFft()
{
    // Initialize FFT buffers (WinSAP Panel::Panel)
    for (int i = 0; i < kFftLength; i++) {
        m_ar[i] = 0.0f;
        m_ai[i] = 0.0f;
    }
    for (int i = 0; i < kFftLengthHalf; i++) {
        m_zr[i] = 0.0f;
        m_zi[i] = 0.0f;
    }
}

/*****************************************************************************
 * Fft
 *
 * Main FFT calculation using Cooley-Tukey algorithm
 * Parameters: z (real part), y (imaginary part), n (FFT stage/power-of-2)
 *
 *****************************************************************************/
void SapFft::Fft(float* z, float* y, int n)
{
    int i, j, k, p;
    int n1, n2;
    float c, s, a, t1, t2;

    // Bit reversal
    j = 0;
    n2 = 1 << (n - 1);
    for (i = 0; i < (1 << n) - 1; i++) {
        if (i < j) {
            t1 = z[j];
            z[j] = z[i];
            z[i] = t1;
            t1 = y[j];
            y[j] = y[i];
            y[i] = t1;
        }
        k = n2;
        while (k <= j) {
            j = j - k;
            k = k >> 1;
        }
        j = j + k;
    }

    // FFT computation
    n1 = 0;
    n2 = 1;
    for (i = 0; i < n; i++) {
        n1 = n2;
        n2 = n2 + n2;
        a = 0.0f;
        for (j = 0; j < n1; j++) {
            c = (float)cos((double)a);
            s = (float)sin((double)a);
            a = a + 6.283185307f / (float)n2;
            for (k = j; k < (1 << n); k = k + n2) {
                p = k + n1;
                t1 = c * z[p] - s * y[p];
                t2 = s * z[p] + c * y[p];
                z[p] = z[k] - t1;
                y[p] = y[k] - t2;
                z[k] = z[k] + t1;
                y[k] = y[k] + t2;
            }
        }
    }
}

/*****************************************************************************
 * EnsureTwiddles
 *
 * Build the per-stage twiddle table used by FftFast. WinSAP regenerates the
 * same values on every transform with cos()/sin() over an accumulating float
 * phase; the table is computed once in double precision instead.
 *
 *****************************************************************************/
void SapFft::EnsureTwiddles(int stage)
{
    if (m_twiddleStage == stage && !m_twiddleCos.empty()) {
        return;
    }

    const int size = 1 << stage;
    const int half = size >> 1;
    m_twiddleCos.resize(static_cast<std::size_t>(half));
    m_twiddleSin.resize(static_cast<std::size_t>(half));

    // WinSAP's inner loop uses angle = j * 2*pi / n2 for stage size n2. The
    // union over all stages is the set { j * 2*pi / size : 0 <= j < size/2 }
    // sampled with a stride, so one half-length table serves every stage.
    const double step = 6.283185307179586 / static_cast<double>(size);
    for (int j = 0; j < half; j++) {
        const double angle = step * static_cast<double>(j);
        m_twiddleCos[static_cast<std::size_t>(j)] = static_cast<float>(cos(angle));
        m_twiddleSin[static_cast<std::size_t>(j)] = static_cast<float>(sin(angle));
    }
    m_twiddleStage = stage;
}

/*****************************************************************************
 * EnsureWindow
 *
 * Cache the WinSAP Hamming window. WinSAP recomputes cos() for every sample
 * of every frame it draws.
 *
 *****************************************************************************/
void SapFft::EnsureWindow(int dataLen)
{
    if (m_windowLength == dataLen && !m_window.empty()) {
        return;
    }

    m_window.resize(static_cast<std::size_t>(dataLen));
    const double doubleConst = 6.2831853 / (double)(dataLen - 1);
    for (int k = 0; k < dataLen; k++) {
        m_window[static_cast<std::size_t>(k)] =
            0.54f - 0.46f * static_cast<float>(cos(static_cast<double>(k) * doubleConst));
    }
    m_windowLength = dataLen;
}

/*****************************************************************************
 * FftFast
 *
 * Structurally identical to Fft(): same decimation, same bit reversal, same
 * butterfly. Only the twiddle source differs.
 *
 *****************************************************************************/
void SapFft::FftFast(float* z, float* y, int n)
{
    EnsureTwiddles(n);
    FftTabled(z, y, n, m_twiddleCos.data(), m_twiddleSin.data());
}

/*****************************************************************************
 * FftTabled
 *
 * The transform kernel. `cosTab`/`sinTab` must hold 2^(n-1) entries sampling
 * exp(i * 2*pi * j / 2^n) for j = 0 .. 2^(n-1) - 1.
 *
 *****************************************************************************/
void SapFft::FftTabled(float* z, float* y, int n, const float* cosTab, const float* sinTab)
{
    int i, j, k, p;
    int n1, n2;
    float c, s, t1, t2;

    const int size = 1 << n;

    // Bit reversal
    j = 0;
    n2 = 1 << (n - 1);
    for (i = 0; i < size - 1; i++) {
        if (i < j) {
            t1 = z[j];
            z[j] = z[i];
            z[i] = t1;
            t1 = y[j];
            y[j] = y[i];
            y[i] = t1;
        }
        k = n2;
        while (k <= j) {
            j = j - k;
            k = k >> 1;
        }
        j = j + k;
    }

    // FFT computation
    n2 = 1;
    for (i = 0; i < n; i++) {
        n1 = n2;
        n2 = n2 + n2;
        // Stage angle step is 2*pi/n2, i.e. every (size/n2)-th table entry.
        const int stride = size / n2;
        for (j = 0; j < n1; j++) {
            const int slot = j * stride;
            c = cosTab[slot];
            s = sinTab[slot];
            for (k = j; k < size; k = k + n2) {
                p = k + n1;
                t1 = c * z[p] - s * y[p];
                t2 = s * z[p] + c * y[p];
                z[p] = z[k] - t1;
                y[p] = y[k] - t2;
                z[k] = z[k] + t1;
                y[k] = y[k] + t2;
            }
        }
    }
}

/*****************************************************************************
 * EnsureRealTables
 *
 * Tables for the real-input path: twiddles for the half length complex
 * transform, plus the exp(i*2*pi*k/kFftLength) factors used by the untangle.
 *
 *****************************************************************************/
void SapFft::EnsureRealTables()
{
    if (m_realTablesReady) {
        return;
    }

    const int halfSize = kFftLength / 2; // points in the complex FFT
    m_halfCos.resize(static_cast<std::size_t>(halfSize / 2));
    m_halfSin.resize(static_cast<std::size_t>(halfSize / 2));
    const double halfStep = 6.283185307179586 / static_cast<double>(halfSize);
    for (int j = 0; j < halfSize / 2; j++) {
        const double angle = halfStep * static_cast<double>(j);
        m_halfCos[static_cast<std::size_t>(j)] = static_cast<float>(cos(angle));
        m_halfSin[static_cast<std::size_t>(j)] = static_cast<float>(sin(angle));
    }

    m_untangleCos.resize(static_cast<std::size_t>(halfSize) + 1);
    m_untangleSin.resize(static_cast<std::size_t>(halfSize) + 1);
    const double fullStep = 6.283185307179586 / static_cast<double>(kFftLength);
    for (int k = 0; k <= halfSize; k++) {
        const double angle = fullStep * static_cast<double>(k);
        m_untangleCos[static_cast<std::size_t>(k)] = static_cast<float>(cos(angle));
        m_untangleSin[static_cast<std::size_t>(k)] = static_cast<float>(sin(angle));
    }

    m_realTablesReady = true;
}

/*****************************************************************************
 * ComputeSpectrumDb
 *
 * Numeric half of WinSAP Panel::DispSpectrum. WinSAP indexes its window with
 * the raw frame length; the only deviation here is that the frame length is
 * capped at kFftLength so an oversized frame cannot run past the buffers.
 *
 *****************************************************************************/
bool SapFft::ComputeSpectrumDb(const int16_t* samples,
                               std::size_t sampleCount,
                               std::size_t frameBegin,
                               std::size_t frameLength,
                               float upLimit,
                               float lowLimit,
                               float* spectrumDb)
{
    if (samples == nullptr || spectrumDb == nullptr || sampleCount == 0 || frameLength == 0) {
        return false;
    }

    // Calculate sample range
    const std::size_t datHp = frameBegin * frameLength;
    const std::size_t datTp = datHp + frameLength;

    // Validate data availability
    if (datHp > sampleCount || datTp > sampleCount)
        return false;

    const int dataLen = static_cast<int>(
        frameLength > static_cast<std::size_t>(kFftLength) ? static_cast<std::size_t>(kFftLength)
                                                           : frameLength);
    if (dataLen < 2)
        return false;

    // Apply Hamming window and prepare FFT input
    double doubleConst = 6.2831853 / (double)(dataLen - 1);
    for (int k = 0; k < dataLen; k++) {
        m_ar[k] = static_cast<float>(samples[datHp + static_cast<std::size_t>(k)]) *
                  (0.54f - 0.46f * static_cast<float>(cos(static_cast<double>(k) * doubleConst)));
        m_ai[k] = 0.0f;
    }

    // Zero-padding to kFftLength
    for (int k = dataLen; k < kFftLength; k++) {
        m_ar[k] = 0.0f;
        m_ai[k] = 0.0f;
    }

    // Compute FFT
    Fft(m_ar, m_ai, kFftStage);

    // Calculate power spectrum in normalized dB (0 dB = full scale sine wave)
    const float refPower = kFftReferenceMagnitude * kFftReferenceMagnitude;

    for (int k = 0; k < kFftCutOff; k++) {
        float mag2 = m_ar[k] * m_ar[k] + m_ai[k] * m_ai[k];     // Magnitude squared
        float value = 10.0f * log10f(mag2 / refPower + 1e-20f); // Normalized dB
        if (value > upLimit)
            value = upLimit;
        if (value < lowLimit)
            value = lowLimit;
        spectrumDb[k] = value;
    }

    return true;
}

/*****************************************************************************
 * ComputeSpectrumDbFast
 *
 * Same contract and same output as ComputeSpectrumDb(), using the cached
 * window and the table-driven transform.
 *
 *****************************************************************************/
bool SapFft::ComputeSpectrumDbFast(const int16_t* samples,
                                   std::size_t sampleCount,
                                   std::size_t frameBegin,
                                   std::size_t frameLength,
                                   float upLimit,
                                   float lowLimit,
                                   float* spectrumDb)
{
    if (samples == nullptr || spectrumDb == nullptr || sampleCount == 0 || frameLength == 0) {
        return false;
    }

    const std::size_t datHp = frameBegin * frameLength;
    const std::size_t datTp = datHp + frameLength;
    if (datHp > sampleCount || datTp > sampleCount)
        return false;

    const int dataLen = static_cast<int>(
        frameLength > static_cast<std::size_t>(kFftLength) ? static_cast<std::size_t>(kFftLength)
                                                           : frameLength);
    if (dataLen < 2)
        return false;

    EnsureWindow(dataLen);

    for (int k = 0; k < dataLen; k++) {
        m_ar[k] = static_cast<float>(samples[datHp + static_cast<std::size_t>(k)]) *
                  m_window[static_cast<std::size_t>(k)];
        m_ai[k] = 0.0f;
    }
    for (int k = dataLen; k < kFftLength; k++) {
        m_ar[k] = 0.0f;
        m_ai[k] = 0.0f;
    }

    FftFast(m_ar, m_ai, kFftStage);

    const float refPower = kFftReferenceMagnitude * kFftReferenceMagnitude;
    for (int k = 0; k < kFftCutOff; k++) {
        float mag2 = m_ar[k] * m_ar[k] + m_ai[k] * m_ai[k];
        float value = 10.0f * log10f(mag2 / refPower + 1e-20f);
        if (value > upLimit)
            value = upLimit;
        if (value < lowLimit)
            value = lowLimit;
        spectrumDb[k] = value;
    }

    return true;
}

/*****************************************************************************
 * ComputeSpectrumDbReal
 *
 * Same contract and same output as ComputeSpectrumDb(), computed through a
 * half length complex transform.
 *
 * With x[] the windowed, zero padded real input of length N = kFftLength and
 * M = N/2, pack z[n] = x[2n] + i*x[2n+1] and transform z with the same kernel,
 * which uses the exp(+i) sign convention. Writing E and O for the transforms
 * of the even and odd samples,
 *
 *     Z[k]             = E[k] + i*O[k]
 *     conj(Z[M-k])     = E[k] - i*O[k]
 *
 * so E[k] = (Z[k] + conj(Z[M-k])) / 2, O[k] = (Z[k] - conj(Z[M-k])) / (2i),
 * and X[k] = E[k] + exp(i*2*pi*k/N) * O[k].
 *
 * Only bins 0 .. kFftCutOff-1 are needed and kFftCutOff <= M, so k never
 * reaches M and the wrap-around case never arises. The index km = (M - k) & mask
 * stays inside the half-size buffers for every k in range, including k = 0,
 * which maps to km = 0 and yields the correct DC term.
 *
 *****************************************************************************/
bool SapFft::ComputeSpectrumDbReal(const int16_t* samples,
                                   std::size_t sampleCount,
                                   std::size_t frameBegin,
                                   std::size_t frameLength,
                                   float upLimit,
                                   float lowLimit,
                                   float* spectrumDb,
                                   std::size_t frameCount)
{
    if (samples == nullptr || spectrumDb == nullptr || sampleCount == 0 || frameLength == 0) {
        return false;
    }
    if (frameCount == 0)
        frameCount = 1;

    const std::size_t datHp = frameBegin * frameLength;
    const std::size_t datTp = datHp + frameCount * frameLength;
    if (datHp > sampleCount || datTp > sampleCount)
        return false;

    const std::size_t span = datTp - datHp;
    const int dataLen = static_cast<int>(
        span > static_cast<std::size_t>(kFftLength) ? static_cast<std::size_t>(kFftLength)
                                                    : span);
    if (dataLen < 2)
        return false;

    EnsureWindow(dataLen);
    EnsureRealTables();

    const int halfSize = kFftLength / 2;
    const int16_t* src = samples + datHp;
    const float* win = m_window.data();

    // Window and pack: even samples into the real part, odd into the imaginary.
    const int fullPairs = dataLen / 2;
    for (int n = 0; n < fullPairs; n++) {
        m_zr[n] = static_cast<float>(src[2 * n]) * win[2 * n];
        m_zi[n] = static_cast<float>(src[2 * n + 1]) * win[2 * n + 1];
    }
    int packed = fullPairs;
    if (dataLen & 1) {
        m_zr[packed] = static_cast<float>(src[dataLen - 1]) * win[dataLen - 1];
        m_zi[packed] = 0.0f;
        packed++;
    }
    for (int n = packed; n < halfSize; n++) {
        m_zr[n] = 0.0f;
        m_zi[n] = 0.0f;
    }

    FftTabled(m_zr, m_zi, kFftStage - 1, m_halfCos.data(), m_halfSin.data());

    const float refPower = kFftReferenceMagnitude * kFftReferenceMagnitude;
    const int mask = halfSize - 1;

    for (int k = 0; k < kFftCutOff; k++) {
        const int km = (halfSize - k) & mask;

        const float zrk = m_zr[k];
        const float zik = m_zi[k];
        const float zrm = m_zr[km];
        const float zim = m_zi[km];

        const float er = 0.5f * (zrk + zrm);
        const float ei = 0.5f * (zik - zim);

        // O = (Z[k] - conj(Z[M-k])) * (-i/2)
        const float dr = zrk - zrm;
        const float di = zik + zim;
        const float orr = 0.5f * di;
        const float oi = -0.5f * dr;

        const float c = m_untangleCos[static_cast<std::size_t>(k)];
        const float s = m_untangleSin[static_cast<std::size_t>(k)];

        const float xr = er + (c * orr - s * oi);
        const float xi = ei + (s * orr + c * oi);

        float mag2 = xr * xr + xi * xi;
        float value = 10.0f * log10f(mag2 / refPower + 1e-20f);
        if (value > upLimit)
            value = upLimit;
        if (value < lowLimit)
            value = lowLimit;
        spectrumDb[k] = value;
    }

    return true;
}

} // namespace xsap::core
