/************************************************************************
 *
 *  XSap - Shared analysis core: whole file spectrogram cache
 *
 *  FILE:      core/SapSpectrogramCache.h
 *
 *  PURPOSE:   Analyses an entire file once, on a sample grid anchored to the
 *             file rather than to the view, and keeps the result in memory so
 *             that scrolling and zooming are pure reductions of cached data.
 *
 *             The per view spectrogram in SapSpectrogram re-derives its frame
 *             boundaries from the visible span, so advancing the cursor by a
 *             single frame re-cuts every column and re-runs every transform.
 *             Measured on a 400 frame view that is 1.5 to 10 ms of work per
 *             cursor move, and the picture shimmers because the analysis grid
 *             slides underneath it. Reducing cached columns instead is 0.02 ms
 *             and the grid is stable.
 *
 *             Analysis geometry is chosen from the file's sample rate so that
 *             the effective bandwidth is the same at every rate:
 *
 *                 window = kDefaultWindowMs of audio, in samples
 *                 fft    = max(kMinFftSize, next power of two >= window)
 *                 hop    = kDefaultHopMs of audio, in samples
 *
 *             Columns hold quantised dB, not grey levels, so that the display
 *             dynamic range can be changed without re-analysing.
 *
 ************************************************************************/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xsap::core {

// 20 ms gives roughly a 200 Hz effective bandwidth with a Hamming window,
// between a wideband spectrogram (formants and glottal striations) and a
// narrowband one (resolved harmonics).
inline constexpr double kDefaultWindowMs = 20.0;
inline constexpr double kDefaultHopMs = 5.0;

// A 20 ms window is only 160 samples at 8 kHz; zero padding up to this size
// keeps enough bins to fill a plot without the display looking blocky.
inline constexpr int kMinFftSize = 512;
inline constexpr int kMaxFftSize = 8192;

// dB are stored quantised to this many steps per dB.
inline constexpr float kDbQuantScale = 2.0f;                 // 0.5 dB per step
inline constexpr float kDbQuantMax = 255.0f / kDbQuantScale; // 127.5 dB

// Default ceiling on the cached matrix. The hop is widened until the file fits.
inline constexpr std::size_t kDefaultMemoryBudget = 256u * 1024u * 1024u;

// Default number of dB below the file peak that the grey ramp spans.
inline constexpr float kDefaultDynamicRangeDb = 60.0f;

// Analysis is claimed a chunk at a time, nearest to the view first, so the
// columns the user is looking at are finished long before the whole file is.
inline constexpr std::size_t kSpectrogramChunkColumns = 1024;

struct SpectrogramGeometry {
    int sampleRate = 0;
    int windowLength = 0; // analysis window, samples
    int fftSize = 0;
    int stage = 0; // log2(fftSize)
    int bins = 0;  // fftSize / 2
    int hop = 0;   // samples between columns

    bool valid() const noexcept { return bins > 0 && hop > 0 && windowLength > 0; }
    double windowMs() const noexcept { return 1000.0 * windowLength / sampleRate; }
    double hopMs() const noexcept { return 1000.0 * hop / sampleRate; }
    // Hamming main lobe width, the resolution the picture actually has.
    double bandwidthHz() const noexcept { return 4.0 * sampleRate / windowLength; }
};

// Chooses the analysis geometry for a file, widening the hop if the cached
// matrix would otherwise exceed `memoryBudget`.
SpectrogramGeometry ChooseSpectrogramGeometry(int sampleRate,
                                              std::size_t sampleCount,
                                              double windowMs = kDefaultWindowMs,
                                              double hopMs = kDefaultHopMs,
                                              std::size_t memoryBudget = kDefaultMemoryBudget);

// A grey level image sized exactly to the destination rectangle, so the caller
// blits it one pixel to one pixel. Row 0 is the top, i.e. the highest frequency.
struct SpectrogramView {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> grey; // width * height, indexed [row * width + column]

    bool empty() const noexcept { return width <= 0 || height <= 0 || grey.empty(); }
};

class SapSpectrogramCache {
public:
    SapSpectrogramCache() = default;
    ~SapSpectrogramCache();

    SapSpectrogramCache(const SapSpectrogramCache&) = delete;
    SapSpectrogramCache& operator=(const SapSpectrogramCache&) = delete;

    // Analyses the whole file on this thread, using `threadCount` workers
    // (0 selects a sensible default). `samples` must outlive the call.
    bool Build(const std::int16_t* samples,
               std::size_t sampleCount,
               int sampleRate,
               unsigned threadCount = 0,
               double windowMs = kDefaultWindowMs,
               double hopMs = kDefaultHopMs,
               std::size_t memoryBudget = kDefaultMemoryBudget);

    // Starts Build() on a background thread. The samples are copied, so the
    // caller is free to close or reload the track while the build runs.
    void BuildAsync(const std::int16_t* samples,
                    std::size_t sampleCount,
                    int sampleRate,
                    double windowMs = kDefaultWindowMs,
                    double hopMs = kDefaultHopMs,
                    std::size_t memoryBudget = kDefaultMemoryBudget);

    // Preferred overload. Shares ownership of the caller's samples instead of
    // duplicating them, which on a long file saves both a large allocation and
    // the copy that goes with it, and still cannot dangle if the track closes.
    void BuildAsync(std::shared_ptr<const std::vector<std::int16_t>> samples,
                    int sampleRate,
                    double windowMs = kDefaultWindowMs,
                    double hopMs = kDefaultHopMs,
                    std::size_t memoryBudget = kDefaultMemoryBudget);

    // Asks a background build to stop and waits for it.
    void Cancel();

    void Reset();

    bool building() const noexcept { return m_building.load(std::memory_order_acquire); }
    bool ready() const noexcept { return m_ready.load(std::memory_order_acquire); }
    // True once the geometry is known and the matrix exists, which happens
    // almost immediately. Render() works from this point on, drawing whichever
    // chunks have finished and leaving the rest blank.
    bool usable() const noexcept { return m_usable.load(std::memory_order_acquire); }
    // Bumped every time a chunk completes, so callers can invalidate a
    // memoised view instead of re-rendering on every repaint.
    std::uint64_t generation() const noexcept
    {
        return m_generation.load(std::memory_order_acquire);
    }
    // Tells the workers which part of the file the user is looking at.
    // Outstanding chunks are claimed nearest to this point first.
    void SetFocusSample(std::size_t firstSample) const noexcept;
    // 0 to 100, meaningful while building() is true.
    int progress() const noexcept { return m_progress.load(std::memory_order_relaxed); }

    const SpectrogramGeometry& geometry() const noexcept { return m_geometry; }
    std::size_t columns() const noexcept { return m_columns; }
    int bins() const noexcept { return m_geometry.bins; }
    float peakDb() const noexcept
    {
        return static_cast<float>(m_peakQuant.load(std::memory_order_acquire)) / kDbQuantScale;
    }
    std::size_t memoryBytes() const noexcept { return m_matrix.size(); }

    // Reduces the cached matrix over [firstSample, firstSample + spanSamples)
    // and the frequency band [lowHz, highHz) into a `width` by `height` grey
    // image. Reduction is by maximum, so a narrow event is never lost to
    // decimation. Returns false if the cache is not ready or the request is
    // degenerate.
    bool Render(std::size_t firstSample,
                std::size_t spanSamples,
                int width,
                int height,
                float dynamicRangeDb,
                double lowHz,
                double highHz,
                SpectrogramView& out) const;

private:
    bool BuildInternal(const std::int16_t* samples,
                       std::size_t sampleCount,
                       int sampleRate,
                       unsigned threadCount,
                       double windowMs,
                       double hopMs,
                       std::size_t memoryBudget);

    // Claims the outstanding chunk nearest the focus point, or returns false
    // when none is left. Chunks write disjoint ranges of the matrix.
    bool ClaimNextChunk(std::size_t& chunkIndex);
    bool ChunkDone(std::size_t chunkIndex) const noexcept;
    void RecordPeak(int quantisedDb) noexcept;

    SpectrogramGeometry m_geometry{};
    std::size_t m_columns = 0;
    std::vector<std::int16_t> m_ownedSamples;                         // populated by BuildAsync only
    std::shared_ptr<const std::vector<std::int16_t>> m_sharedSamples; // ditto
    std::vector<std::uint8_t> m_matrix;                               // columns * bins, [column * bins + bin]

    // 0 pending, 1 claimed, 2 done. Indexed by column / kSpectrogramChunkColumns.
    std::unique_ptr<std::atomic<std::uint8_t>[]> m_chunkState;
    std::size_t m_chunkCount = 0;
    std::mutex m_claimMutex;
    mutable std::atomic<std::size_t> m_focusChunk{0};
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<int> m_peakQuant{0};
    std::atomic<std::size_t> m_chunksDone{0};

    std::thread m_worker;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_usable{false};
    std::atomic<bool> m_building{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<int> m_progress{0};
};

} // namespace xsap::core
