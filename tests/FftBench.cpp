/************************************************************************
 *
 *  XSap - FFT benchmark / parity harness
 *
 *  FILE:      tests/FftBench.cpp
 *
 *  PURPOSE:   Compare three 2048-point spectrum implementations:
 *
 *               1. winsap   - Panel::FFT as shipped by WinSAP, with its
 *                             per-stage cos()/sin() recurrence.
 *               2. xsap-old - the std::complex<double> radix-2 routine XSap
 *                             used before the port.
 *               3. fast     - the same WinSAP algorithm with a precomputed
 *                             float twiddle table (core::SapFft::FftFast).
 *
 *             Reports throughput plus the worst-case dB deviation of the
 *             fast path from the WinSAP reference, so the optimisation can
 *             be judged on both speed and accuracy.
 *
 ************************************************************************/

#include "core/SapFft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kLength = xsap::core::kFftLength;
constexpr int kStage = xsap::core::kFftStage;
constexpr int kCutOff = xsap::core::kFftCutOff;
constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> makeSignal(std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i);
        const double value = 12000.0 * std::sin(2.0 * kPi * 440.0 * t / 16000.0) +
                             6000.0 * std::sin(2.0 * kPi * 1700.0 * t / 16000.0) +
                             2500.0 * std::sin(2.0 * kPi * 3100.0 * t / 16000.0);
        samples[i] = static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
    }
    return samples;
}

// The pre-port XSap routine, kept verbatim for comparison.
void fftRadix2(std::vector<std::complex<double>>& values)
{
    const std::size_t count = values.size();
    std::size_t bitCount = 0;
    while ((static_cast<std::size_t>(1) << bitCount) < count) {
        ++bitCount;
    }

    for (std::size_t i = 0; i < count; ++i) {
        std::size_t reversed = 0;
        for (std::size_t bit = 0; bit < bitCount; ++bit) {
            reversed = (reversed << 1U) | ((i >> bit) & 1U);
        }
        if (reversed > i) {
            std::swap(values[i], values[reversed]);
        }
    }

    for (std::size_t size = 2; size <= count; size <<= 1U) {
        const double angle = -2.0 * kPi / static_cast<double>(size);
        const std::complex<double> wStep(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += size) {
            std::complex<double> w(1.0, 0.0);
            const std::size_t half = size / 2U;
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> even = values[offset + k];
                const std::complex<double> odd = w * values[offset + k + half];
                values[offset + k] = even + odd;
                values[offset + k + half] = even - odd;
                w *= wStep;
            }
        }
    }
}

double elapsedMs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

int main()
{
    constexpr int kIterations = 2000;
    constexpr std::size_t kFrameLength = 512;

    const auto samples = makeSignal(static_cast<std::size_t>(kLength) * 4U);
    xsap::core::SapFft fft;

    std::vector<float> reference(kCutOff, 0.0f);
    std::vector<float> fast(kCutOff, 0.0f);

    // ---- 1. WinSAP reference ------------------------------------------------
    double sink = 0.0;
    auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        fft.ComputeSpectrumDb(samples.data(), samples.size(), 0, kFrameLength,
                              xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                              reference.data());
        sink += reference[10];
    }
    const double winsapMs = elapsedMs(start);

    // ---- 2. Pre-port XSap routine ------------------------------------------
    start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        std::vector<std::complex<double>> values(kLength, std::complex<double>(0.0, 0.0));
        for (std::size_t i = 0; i < kFrameLength; ++i) {
            const double window =
                0.54 - 0.46 * std::cos((2.0 * kPi * static_cast<double>(i)) /
                                       static_cast<double>(kFrameLength - 1U));
            values[i] = std::complex<double>(
                (static_cast<double>(samples[i]) / 32768.0) * window, 0.0);
        }
        fftRadix2(values);
        sink += std::abs(values[10]);
    }
    const double oldXsapMs = elapsedMs(start);

    // ---- 3. Table-driven WinSAP algorithm -----------------------------------
    start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        fft.ComputeSpectrumDbFast(samples.data(), samples.size(), 0, kFrameLength,
                                  xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                                  fast.data());
        sink += fast[10];
    }
    const double fastMs = elapsedMs(start);

    // ---- 4. Real-input transform (half length complex FFT + untangle) -------
    std::vector<float> real(kCutOff, 0.0f);
    start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        fft.ComputeSpectrumDbReal(samples.data(), samples.size(), 0, kFrameLength,
                                  xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                                  real.data());
        sink += real[10];
    }
    const double realMs = elapsedMs(start);

    // ---- Accuracy ------------------------------------------------------------
    // Both float paths are scored against an exact double-precision DFT of the
    // same windowed frame, so "closest to winsap" is not mistaken for "correct".
    std::vector<std::complex<double>> exact(kLength, std::complex<double>(0.0, 0.0));
    for (std::size_t i = 0; i < kFrameLength; ++i) {
        const double window = 0.54 - 0.46 * std::cos((2.0 * kPi * static_cast<double>(i)) /
                                                     static_cast<double>(kFrameLength - 1U));
        exact[i] = std::complex<double>(static_cast<double>(samples[i]) * window, 0.0);
    }
    fftRadix2(exact);

    const double refMag = (static_cast<double>(kLength) / 2.0) * 32768.0;
    const double refPower = refMag * refMag;
    double winsapErr = 0.0;
    double fastErr = 0.0;
    double realErr = 0.0;
    double maxDeviationDb = 0.0;
    double maxRealDeviationDb = 0.0;
    for (int bin = 0; bin < kCutOff; ++bin) {
        const double mag2 = std::norm(exact[static_cast<std::size_t>(bin)]);
        double truth = 10.0 * std::log10(mag2 / refPower + 1e-20);
        truth = std::clamp(truth, static_cast<double>(xsap::core::kFftLowLimitDb),
                           static_cast<double>(xsap::core::kFftUpLimitDb));
        winsapErr = std::max(winsapErr, std::fabs(truth - static_cast<double>(reference[bin])));
        fastErr = std::max(fastErr, std::fabs(truth - static_cast<double>(fast[bin])));
        realErr = std::max(realErr, std::fabs(truth - static_cast<double>(real[bin])));
        maxDeviationDb = std::max(maxDeviationDb,
                                  std::fabs(static_cast<double>(reference[bin]) -
                                            static_cast<double>(fast[bin])));
        maxRealDeviationDb = std::max(maxRealDeviationDb,
                                      std::fabs(static_cast<double>(reference[bin]) -
                                                static_cast<double>(real[bin])));
    }

    std::printf("2048-point spectrum, %d iterations, frame length %zu\n",
                kIterations, kFrameLength);
    std::printf("  winsap Panel::FFT (ported) : %8.2f ms  (%7.1f us/frame)\n",
                winsapMs, (winsapMs * 1000.0) / kIterations);
    std::printf("  xsap complex<double> (old) : %8.2f ms  (%7.1f us/frame)  %5.2fx vs winsap\n",
                oldXsapMs, (oldXsapMs * 1000.0) / kIterations, oldXsapMs / winsapMs);
    std::printf("  table-driven (SapFft fast) : %8.2f ms  (%7.1f us/frame)  %5.2fx faster\n",
                fastMs, (fastMs * 1000.0) / kIterations, winsapMs / fastMs);
    std::printf("  real-input (SapFft real)   : %8.2f ms  (%7.1f us/frame)  %5.2fx faster\n",
                realMs, (realMs * 1000.0) / kIterations, winsapMs / realMs);
    std::printf("  real vs table-driven       : %5.2fx faster\n", fastMs / realMs);
    std::printf("  max |fast - winsap| over %d bins : %.6f dB\n", kCutOff, maxDeviationDb);
    std::printf("  max |real - winsap| over %d bins : %.6f dB\n", kCutOff, maxRealDeviationDb);
    std::printf("  worst error vs exact double DFT  : winsap %.6f dB,  fast %.6f dB,  real %.6f dB\n",
                winsapErr, fastErr, realErr);
    std::printf("  (checksum %.3f)\n", sink);

    return 0;
}
