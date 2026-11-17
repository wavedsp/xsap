/************************************************************************
 *
 *  XSap - Shared analysis core: whole file spectrogram cache
 *
 *  FILE:      core/SapSpectrogramCache.cpp
 *
 ************************************************************************/

#include "core/SapSpectrogramCache.h"

#include <algorithm>
#include <cmath>
#include <new>

#include "core/SapFft.h"

namespace xsap::core {

namespace {

constexpr double kTwoPi = 6.283185307179586;

int NextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

int Log2Exact(int value)
{
    int stage = 0;
    while ((1 << stage) < value) {
        stage++;
    }
    return stage;
}

/*
 * Per thread scratch: the twiddle tables for a 2^(stage-1) point complex
 * transform, the untangle factors that recover the 2^stage point real
 * spectrum from it, and the Hamming window.
 */
struct Worker {
    std::vector<float> halfCos, halfSin, untangleCos, untangleSin, window, zr, zi;

    void Init(const SpectrogramGeometry& geometry)
    {
        const int half = geometry.fftSize / 2;

        halfCos.resize(static_cast<std::size_t>(half / 2));
        halfSin.resize(static_cast<std::size_t>(half / 2));
        const double halfStep = kTwoPi / static_cast<double>(half);
        for (int j = 0; j < half / 2; j++) {
            halfCos[static_cast<std::size_t>(j)] = static_cast<float>(std::cos(halfStep * j));
            halfSin[static_cast<std::size_t>(j)] = static_cast<float>(std::sin(halfStep * j));
        }

        untangleCos.resize(static_cast<std::size_t>(half) + 1);
        untangleSin.resize(static_cast<std::size_t>(half) + 1);
        const double fullStep = kTwoPi / static_cast<double>(geometry.fftSize);
        for (int k = 0; k <= half; k++) {
            untangleCos[static_cast<std::size_t>(k)] = static_cast<float>(std::cos(fullStep * k));
            untangleSin[static_cast<std::size_t>(k)] = static_cast<float>(std::sin(fullStep * k));
        }

        window.resize(static_cast<std::size_t>(geometry.windowLength));
        const double step = 6.2831853 / static_cast<double>(geometry.windowLength - 1);
        for (int k = 0; k < geometry.windowLength; k++) {
            window[static_cast<std::size_t>(k)] =
                0.54f - 0.46f * static_cast<float>(std::cos(k * step));
        }

        zr.assign(static_cast<std::size_t>(half), 0.0f);
        zi.assign(static_cast<std::size_t>(half), 0.0f);
    }

    // Transforms one column and writes quantised dB. Returns the column peak.
    float Column(const SpectrogramGeometry& geometry,
                 const std::int16_t* src,
                 int usable,
                 std::uint8_t* dest)
    {
        const int half = geometry.fftSize / 2;

        // Pack the real input as half as many complex points: even samples to
        // the real part, odd samples to the imaginary part.
        const int pairs = usable / 2;
        for (int i = 0; i < pairs; i++) {
            zr[static_cast<std::size_t>(i)] =
                static_cast<float>(src[2 * i]) * window[static_cast<std::size_t>(2 * i)];
            zi[static_cast<std::size_t>(i)] =
                static_cast<float>(src[2 * i + 1]) * window[static_cast<std::size_t>(2 * i + 1)];
        }
        int packed = pairs;
        if (usable & 1) {
            zr[static_cast<std::size_t>(packed)] =
                static_cast<float>(src[usable - 1]) * window[static_cast<std::size_t>(usable - 1)];
            zi[static_cast<std::size_t>(packed)] = 0.0f;
            packed++;
        }
        for (int i = packed; i < half; i++) {
            zr[static_cast<std::size_t>(i)] = 0.0f;
            zi[static_cast<std::size_t>(i)] = 0.0f;
        }

        SapFft::FftTabled(zr.data(), zi.data(), geometry.stage - 1, halfCos.data(), halfSin.data());

        const int mask = half - 1;
        float peak = 0.0f;
        for (int k = 0; k < geometry.bins; k++) {
            const int km = (half - k) & mask;

            const float zrk = zr[static_cast<std::size_t>(k)];
            const float zik = zi[static_cast<std::size_t>(k)];
            const float zrm = zr[static_cast<std::size_t>(km)];
            const float zim = zi[static_cast<std::size_t>(km)];

            const float er = 0.5f * (zrk + zrm);
            const float ei = 0.5f * (zik - zim);
            const float dr = zrk - zrm;
            const float di = zik + zim;
            const float orr = 0.5f * di;
            const float oi = -0.5f * dr;

            const float c = untangleCos[static_cast<std::size_t>(k)];
            const float s = untangleSin[static_cast<std::size_t>(k)];

            const float xr = er + (c * orr - s * oi);
            const float xi = ei + (s * orr + c * oi);

            // The +1 keeps an all zero frame finite, as in wsap dispSpectrum.
            float db = 10.0f * std::log10(xr * xr + xi * xi + 1.0f);
            if (db < 0.0f)
                db = 0.0f;
            if (db > kDbQuantMax)
                db = kDbQuantMax;
            if (db > peak)
                peak = db;

            dest[k] = static_cast<std::uint8_t>(db * kDbQuantScale + 0.5f);
        }
        return peak;
    }
};

} // namespace

/*****************************************************************************
 * ChooseSpectrogramGeometry
 *
 * The window is a fixed duration so that the effective bandwidth is the same
 * at every sample rate; the FFT is only as large as that window requires, with
 * a floor so that low rate files still produce enough bins to fill a plot.
 *
 *****************************************************************************/
SpectrogramGeometry ChooseSpectrogramGeometry(int sampleRate,
                                              std::size_t sampleCount,
                                              double windowMs,
                                              double hopMs,
                                              std::size_t memoryBudget)
{
    SpectrogramGeometry geometry{};
    if (sampleRate <= 0 || sampleCount == 0) {
        return geometry;
    }

    geometry.sampleRate = sampleRate;

    int windowLength = static_cast<int>(std::lround(windowMs * sampleRate / 1000.0));
    windowLength = std::max(16, std::min(windowLength, kMaxFftSize));
    geometry.windowLength = windowLength;

    geometry.fftSize = std::max(kMinFftSize, NextPowerOfTwo(windowLength));
    geometry.fftSize = std::min(geometry.fftSize, kMaxFftSize);
    geometry.stage = Log2Exact(geometry.fftSize);
    geometry.bins = geometry.fftSize / 2;

    int hop = static_cast<int>(std::lround(hopMs * sampleRate / 1000.0));
    hop = std::max(1, hop);

    // Widen the hop until the matrix fits the budget. Doubling keeps the
    // columns on a subset of the original grid. A hop wider than the window
    // leaves gaps between analysed frames, but that only happens on files far
    // beyond the budget, where the display is decimating heavily anyway.
    const std::size_t binBytes = static_cast<std::size_t>(geometry.bins);
    while (static_cast<std::size_t>(hop) < sampleCount) {
        const std::size_t columns = sampleCount / static_cast<std::size_t>(hop) + 1u;
        if (columns * binBytes <= memoryBudget) {
            break;
        }
        hop *= 2;
    }
    geometry.hop = hop;

    return geometry;
}

SapSpectrogramCache::~SapSpectrogramCache()
{
    Cancel();
}

void SapSpectrogramCache::Cancel()
{
    m_cancel.store(true, std::memory_order_release);
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_cancel.store(false, std::memory_order_release);
    m_building.store(false, std::memory_order_release);
}

void SapSpectrogramCache::Reset()
{
    Cancel();
    m_ready.store(false, std::memory_order_release);
    m_usable.store(false, std::memory_order_release);
    m_progress.store(0, std::memory_order_relaxed);
    m_generation.store(0, std::memory_order_release);
    m_peakQuant.store(0, std::memory_order_release);
    m_chunksDone.store(0, std::memory_order_release);
    m_focusChunk.store(0, std::memory_order_release);
    m_geometry = SpectrogramGeometry{};
    m_columns = 0;
    m_chunkState.reset();
    m_chunkCount = 0;
    m_ownedSamples.clear();
    m_ownedSamples.shrink_to_fit();
    m_sharedSamples.reset();
    m_matrix.clear();
    m_matrix.shrink_to_fit();
}

void SapSpectrogramCache::SetFocusSample(std::size_t firstSample) const noexcept
{
    if (m_geometry.hop <= 0 || m_chunkCount == 0) {
        return;
    }
    const std::size_t column = firstSample / static_cast<std::size_t>(m_geometry.hop);
    const std::size_t chunk = std::min(column / kSpectrogramChunkColumns, m_chunkCount - 1u);
    m_focusChunk.store(chunk, std::memory_order_release);
}

bool SapSpectrogramCache::ChunkDone(std::size_t chunkIndex) const noexcept
{
    if (m_chunkState == nullptr || chunkIndex >= m_chunkCount) {
        return false;
    }
    return m_chunkState[chunkIndex].load(std::memory_order_acquire) == 2u;
}

/*
 * Claims the unclaimed chunk closest to wherever the user is looking. The scan
 * is linear in the chunk count, which is a few hundred at most, and happens
 * once per chunk rather than once per column.
 */
bool SapSpectrogramCache::ClaimNextChunk(std::size_t& chunkIndex)
{
    const std::lock_guard<std::mutex> guard(m_claimMutex);

    const std::size_t focus = m_focusChunk.load(std::memory_order_acquire);
    std::size_t best = m_chunkCount;
    std::size_t bestDistance = 0;

    for (std::size_t chunk = 0; chunk < m_chunkCount; chunk++) {
        if (m_chunkState[chunk].load(std::memory_order_relaxed) != 0u) {
            continue;
        }
        const std::size_t distance = focus > chunk ? focus - chunk : chunk - focus;
        if (best == m_chunkCount || distance < bestDistance) {
            best = chunk;
            bestDistance = distance;
            if (distance == 0u) {
                break;
            }
        }
    }

    if (best == m_chunkCount) {
        return false;
    }
    m_chunkState[best].store(1u, std::memory_order_relaxed);
    chunkIndex = best;
    return true;
}

void SapSpectrogramCache::RecordPeak(int quantisedDb) noexcept
{
    int current = m_peakQuant.load(std::memory_order_relaxed);
    while (quantisedDb > current &&
           !m_peakQuant.compare_exchange_weak(current, quantisedDb, std::memory_order_release,
                                              std::memory_order_relaxed)) {
    }
}

bool SapSpectrogramCache::Build(const std::int16_t* samples,
                                std::size_t sampleCount,
                                int sampleRate,
                                unsigned threadCount,
                                double windowMs,
                                double hopMs,
                                std::size_t memoryBudget)
{
    Reset();
    m_building.store(true, std::memory_order_release);
    const bool ok = BuildInternal(samples, sampleCount, sampleRate, threadCount,
                                  windowMs, hopMs, memoryBudget);
    m_building.store(false, std::memory_order_release);
    m_ready.store(ok, std::memory_order_release);
    return ok;
}

void SapSpectrogramCache::BuildAsync(const std::int16_t* samples,
                                     std::size_t sampleCount,
                                     int sampleRate,
                                     double windowMs,
                                     double hopMs,
                                     std::size_t memoryBudget)
{
    Reset();
    if (samples == nullptr || sampleCount == 0) {
        return;
    }

    // Own the audio: the track may be closed or reloaded while this runs.
    try {
        m_ownedSamples.assign(samples, samples + sampleCount);
    } catch (const std::bad_alloc&) {
        return;
    }

    m_building.store(true, std::memory_order_release);
    // Thread creation fails with system_error or bad_alloc under resource
    // pressure. Leave the cache idle and not ready rather than terminating;
    // the spectrogram then simply stays blank.
    try {
        m_worker = std::thread([this, sampleCount, sampleRate, windowMs, hopMs, memoryBudget] {
            bool ok = false;
            try {
                ok = BuildInternal(m_ownedSamples.data(), sampleCount, sampleRate, 0,
                                   windowMs, hopMs, memoryBudget);
            } catch (...) {
                // An exception leaving a thread entry point terminates the process
                ok = false;
            }
            m_ready.store(ok, std::memory_order_release);
            m_building.store(false, std::memory_order_release);
        });
    } catch (const std::exception&) {
        m_building.store(false, std::memory_order_release);
        m_ownedSamples.clear();
        m_ownedSamples.shrink_to_fit();
    }
}

void SapSpectrogramCache::BuildAsync(std::shared_ptr<const std::vector<std::int16_t>> samples,
                                     int sampleRate,
                                     double windowMs,
                                     double hopMs,
                                     std::size_t memoryBudget)
{
    Reset();
    if (samples == nullptr || samples->empty()) {
        return;
    }

    // Shared ownership keeps the audio alive for the build without copying it.
    m_sharedSamples = std::move(samples);
    const std::int16_t* data = m_sharedSamples->data();
    const std::size_t sampleCount = m_sharedSamples->size();

    m_building.store(true, std::memory_order_release);
    // See the copying overload: a failed start leaves the cache idle.
    try {
        m_worker = std::thread([this, data, sampleCount, sampleRate, windowMs, hopMs, memoryBudget] {
            bool ok = false;
            try {
                ok = BuildInternal(data, sampleCount, sampleRate, 0,
                                   windowMs, hopMs, memoryBudget);
            } catch (...) {
                ok = false;
            }
            m_ready.store(ok, std::memory_order_release);
            m_building.store(false, std::memory_order_release);
        });
    } catch (const std::exception&) {
        m_building.store(false, std::memory_order_release);
        m_sharedSamples.reset();
    }
}

/*****************************************************************************
 * BuildInternal
 *
 * One transform per hop across the whole file. The column grid is anchored to
 * sample zero, so a view that scrolls does not move the analysis boundaries.
 *
 *****************************************************************************/
bool SapSpectrogramCache::BuildInternal(const std::int16_t* samples,
                                        std::size_t sampleCount,
                                        int sampleRate,
                                        unsigned threadCount,
                                        double windowMs,
                                        double hopMs,
                                        std::size_t memoryBudget)
{
    if (samples == nullptr || sampleCount == 0 || sampleRate <= 0) {
        return false;
    }

    m_geometry = ChooseSpectrogramGeometry(sampleRate, sampleCount, windowMs, hopMs, memoryBudget);
    if (!m_geometry.valid()) {
        return false;
    }

    const std::size_t hop = static_cast<std::size_t>(m_geometry.hop);
    const std::size_t bins = static_cast<std::size_t>(m_geometry.bins);
    m_columns = sampleCount / hop;
    if (m_columns == 0) {
        m_columns = 1;
    }

    try {
        m_matrix.assign(m_columns * bins, 0u);
        m_chunkCount = (m_columns + kSpectrogramChunkColumns - 1u) / kSpectrogramChunkColumns;
        m_chunkState = std::make_unique<std::atomic<std::uint8_t>[]>(m_chunkCount);
        for (std::size_t chunk = 0; chunk < m_chunkCount; chunk++) {
            m_chunkState[chunk].store(0u, std::memory_order_relaxed);
        }
    } catch (const std::bad_alloc&) {
        m_columns = 0;
        m_chunkCount = 0;
        return false;
    }

    // From here Render() can draw whichever chunks have finished.
    m_usable.store(true, std::memory_order_release);

    unsigned threads = threadCount;
    if (threads == 0) {
        threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    }
    if (m_chunkCount < threads) {
        threads = static_cast<unsigned>(std::max<std::size_t>(1u, m_chunkCount));
    }

    const auto run = [&]() {
        try {
            Worker worker;
            worker.Init(m_geometry);

            std::size_t chunk = 0;
            while (ClaimNextChunk(chunk)) {
                if (m_cancel.load(std::memory_order_acquire)) {
                    return;
                }

                const std::size_t begin = chunk * kSpectrogramChunkColumns;
                const std::size_t end = std::min(begin + kSpectrogramChunkColumns, m_columns);
                float peak = 0.0f;

                for (std::size_t column = begin; column < end; column++) {
                    const std::size_t start = column * hop;
                    if (start >= sampleCount) {
                        break;
                    }
                    const std::size_t available =
                        std::min(static_cast<std::size_t>(m_geometry.windowLength),
                                 sampleCount - start);
                    peak = std::max(peak, worker.Column(m_geometry, samples + start,
                                                        static_cast<int>(available),
                                                        m_matrix.data() + column * bins));
                }

                RecordPeak(static_cast<int>(peak * kDbQuantScale + 0.5f));

                // Release: a reader that sees state 2 also sees the columns.
                m_chunkState[chunk].store(2u, std::memory_order_release);
                m_generation.fetch_add(1u, std::memory_order_release);
                const std::size_t done = m_chunksDone.fetch_add(1u, std::memory_order_relaxed) + 1u;
                m_progress.store(static_cast<int>(done * 100u / m_chunkCount),
                                 std::memory_order_relaxed);
            }
        } catch (const std::bad_alloc&) {
            // A worker without scratch space just stops. Any chunk it had
            // claimed stays unfinished, which the check below reports.
        }
    };

    if (threads == 1) {
        run();
    } else {
        // Every started thread is joined even if a later one fails to start,
        // since destroying a joinable std::thread terminates the process.
        // Whatever did start claims every chunk between them, and if none
        // started this thread does the work itself.
        std::vector<std::thread> pool;
        try {
            pool.reserve(threads);
            for (unsigned id = 0; id < threads; id++) {
                pool.emplace_back(run);
            }
        } catch (const std::exception&) {
            // Run with the threads that did start
        }
        if (pool.empty()) {
            run();
        }
        for (auto& thread : pool) {
            thread.join();
        }
    }

    if (m_cancel.load(std::memory_order_acquire) ||
        m_chunksDone.load(std::memory_order_acquire) < m_chunkCount) {
        return false;
    }

    m_progress.store(100, std::memory_order_relaxed);
    return true;
}

/*****************************************************************************
 * Render
 *
 * Reduces the cached matrix straight into the destination rectangle. Both axes
 * reduce by maximum rather than by sampling, so a single loud column or bin
 * survives decimation instead of being dropped between samples.
 *
 * The grey ramp is linear over [peak - dynamicRange, peak] using the whole
 * file peak, so the picture does not change brightness as the view scrolls.
 *
 *****************************************************************************/
bool SapSpectrogramCache::Render(std::size_t firstSample,
                                 std::size_t spanSamples,
                                 int width,
                                 int height,
                                 float dynamicRangeDb,
                                 double lowHz,
                                 double highHz,
                                 SpectrogramView& out) const
{
    out.width = 0;
    out.height = 0;
    out.grey.clear();

    if (!usable() || m_columns == 0 || width <= 0 || height <= 0 || spanSamples == 0) {
        return false;
    }

    // Tell the workers where to analyse next, so scrolling into an unbuilt
    // region pulls the build towards it instead of starting a separate job.
    SetFocusSample(firstSample);

    const std::size_t hop = static_cast<std::size_t>(m_geometry.hop);
    const int bins = m_geometry.bins;

    const double nyquist = m_geometry.sampleRate / 2.0;
    if (highHz <= lowHz) {
        lowHz = 0.0;
        highHz = nyquist;
    }
    lowHz = std::clamp(lowHz, 0.0, nyquist);
    highHz = std::clamp(highHz, 0.0, nyquist);

    int lowBin = static_cast<int>(lowHz * bins / nyquist);
    int highBin = static_cast<int>(std::ceil(highHz * bins / nyquist));
    lowBin = std::clamp(lowBin, 0, bins - 1);
    highBin = std::clamp(highBin, lowBin + 1, bins);
    const int bandBins = highBin - lowBin;

    const std::size_t firstColumn = std::min(firstSample / hop, m_columns - 1u);
    std::size_t lastColumn = (firstSample + spanSamples) / hop;
    lastColumn = std::clamp<std::size_t>(lastColumn, firstColumn + 1u, m_columns);
    const std::size_t spanColumns = lastColumn - firstColumn;

    try {
        out.grey.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 255u);
    } catch (const std::bad_alloc&) {
        return false;
    }

    // dB to grey, precomputed over the quantised dB range.
    if (dynamicRangeDb < 1.0f) {
        dynamicRangeDb = 1.0f;
    }
    std::uint8_t ramp[256];
    const float floorDb = peakDb() - dynamicRangeDb;
    for (int level = 0; level < 256; level++) {
        const float db = static_cast<float>(level) / kDbQuantScale;
        float fraction = (db - floorDb) / dynamicRangeDb;
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        ramp[level] = static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - fraction)));
    }

    // Column reduction first, into a band sized strip, then row reduction.
    // Columns whose chunk has not been analysed yet are skipped; a destination
    // column with nothing behind it is left blank.
    std::vector<std::uint8_t> strip(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(bandBins));
    std::vector<std::uint8_t> stripHasData(static_cast<std::size_t>(width), 0u);

    for (int x = 0; x < width; x++) {
        std::size_t begin = firstColumn +
                            static_cast<std::size_t>(static_cast<double>(x) * spanColumns / width);
        std::size_t end = firstColumn +
                          static_cast<std::size_t>(static_cast<double>(x + 1) * spanColumns / width);
        begin = std::min(begin, m_columns - 1u);
        end = std::clamp<std::size_t>(end, begin + 1u, m_columns);

        std::uint8_t* dest = strip.data() + static_cast<std::size_t>(x) *
                                                static_cast<std::size_t>(bandBins);
        bool first = true;
        for (std::size_t column = begin; column < end; column++) {
            if (!ChunkDone(column / kSpectrogramChunkColumns)) {
                // Skip the whole chunk rather than testing every column in it.
                const std::size_t chunkEnd =
                    (column / kSpectrogramChunkColumns + 1u) * kSpectrogramChunkColumns;
                column = chunkEnd - 1u;
                continue;
            }
            const std::uint8_t* source = m_matrix.data() +
                                         column * static_cast<std::size_t>(bins) +
                                         static_cast<std::size_t>(lowBin);
            if (first) {
                std::copy(source, source + bandBins, dest);
                first = false;
            } else {
                for (int k = 0; k < bandBins; k++) {
                    dest[k] = std::max(dest[k], source[k]);
                }
            }
        }
        stripHasData[static_cast<std::size_t>(x)] = first ? 0u : 1u;
    }

    for (int y = 0; y < height; y++) {
        // Row 0 is the top of the image and carries the highest frequency.
        const int flipped = height - 1 - y;
        int begin = static_cast<int>(static_cast<double>(flipped) * bandBins / height);
        int end = static_cast<int>(static_cast<double>(flipped + 1) * bandBins / height);
        begin = std::clamp(begin, 0, bandBins - 1);
        end = std::clamp(end, begin + 1, bandBins);

        std::uint8_t* row = out.grey.data() + static_cast<std::size_t>(y) *
                                                  static_cast<std::size_t>(width);
        for (int x = 0; x < width; x++) {
            if (stripHasData[static_cast<std::size_t>(x)] == 0u) {
                continue; // left at the blank value the buffer was filled with
            }
            const std::uint8_t* source = strip.data() + static_cast<std::size_t>(x) *
                                                            static_cast<std::size_t>(bandBins);
            std::uint8_t peak = source[begin];
            for (int k = begin + 1; k < end; k++) {
                peak = std::max(peak, source[k]);
            }
            row[x] = ramp[peak];
        }
    }

    out.width = width;
    out.height = height;
    return true;
}

} // namespace xsap::core
