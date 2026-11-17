/************************************************************************
 *
 *  XSap - Document Model Header
 *
 *  FILE:      Model.h
 *
 *  PURPOSE:   The UI independent half of the program: Track, which owns one
 *             loaded file and everything derived from it, and Panel, which
 *             holds the view state of one display area.
 *
 *             Counterpart of the data members of WinSap's Panel class. The
 *             split exists because GTKMM drawing lives in XsapApp.cpp, so
 *             every analysis result a panel needs is produced here and the
 *             drawing code only reduces it to pixels.
 *
 *  Update:    2026.1
 *             2026.11 .par files are recognised as parameter files
 *
 ************************************************************************/

#pragma once

#include "core/SapFft.h"
#include "core/SapSpectrogram.h"
#include "core/SapSpectrogramCache.h"
#include "core/SapWaveFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xsap {

class Panel;

enum class TrackType {
    WaveformPcm16,
    ParameterText,
    FftAmplitude,
};

// Parameter mode and the .par extension are tied together: a .par file that
// is not a WAV file always opens as parameter text. Case-insensitive.
bool isParameterFileName(const std::filesystem::path& path);

struct Limits {
    double lower = 0.0;
    double upper = 0.0;
};

struct SampleRange {
    int64_t begin = 0;
    int64_t end = 0; // Half-open: [begin, end).

    bool empty() const noexcept { return end <= begin; }
    int64_t length() const noexcept { return empty() ? 0 : (end - begin); }
};

struct ParameterFrame {
    size_t frameIndex = 0;
    std::string header;
    std::string line1;
    std::string line2;
};

struct EnvelopeColumn {
    int16_t minimum = 0;
    int16_t maximum = 0;
    SampleRange source;
};

struct ConnectedPoint {
    double x = 0.0;
    int16_t sample = 0;
    int64_t sampleIndex = 0;
};

struct SpectrumBin {
    size_t bin = 0;
    double frequencyHz = 0.0;
    double valueDb = -120.0;
};

struct TrackOpenOptions {
    std::optional<size_t> slot;
    uint32_t rawPcmSampleRate = 16000;
    // Channel of a WAV file to load: 0 (left, or the only one) or 1 (right).
    int channel = 0;
};

// The header details of a WAV file, nothing when it is not a WAV or cannot be
// read; and why such a WAV cannot be loaded, naming its format (empty if it can).
std::optional<core::WaveInfo> probeWavFile(const std::filesystem::path& path);
std::string wavFormatError(const std::filesystem::path& path, const core::WaveInfo& info);

class Track {
public:
    Track() = default;

    bool open(const std::filesystem::path& path,
              TrackType type,
              uint32_t rawPcmSampleRate,
              std::string& error);
    bool open(const std::filesystem::path& path,
              TrackType type,
              uint32_t rawPcmSampleRate,
              int channel,
              std::string& error);
    bool reload(std::string& error);
    void close() noexcept;

    bool isOpen() const noexcept;
    bool isBinary() const noexcept;
    bool isWav() const noexcept;

    const std::filesystem::path& path() const noexcept;
    TrackType type() const noexcept;

    uint32_t sampleRate() const noexcept;
    uint32_t effectiveSampleRate(uint32_t fallbackSampleRate = 16000) const noexcept;
    bool setRawPcmSampleRate(uint32_t sampleRate, std::string& error);

    // Byte offset of the PCM payload, as reported by WinSAP's ReadWavFile.
    long headerLength() const noexcept;

    // WAV channel layout: the channel loaded, how many the file has, and
    // whether this track is one channel of a stereo file.
    int channel() const noexcept;
    int channelCount() const noexcept;
    bool isStereoChannel() const noexcept;
    // Sample format of the WAV data on disk (Pcm16 for raw PCM).
    core::WavSampleFormat sampleFormat() const noexcept;

    const std::vector<int16_t>& audioSamples() const noexcept;
    std::vector<int16_t> copySamples(SampleRange range) const;
    std::vector<int16_t> copyAllSamples() const;

    size_t sampleCount() const noexcept;
    size_t totalFrames(size_t frameLength) const noexcept;

    const std::vector<ParameterFrame>& parameterFrames() const noexcept;
    size_t parameterFrameCount() const noexcept;
    std::optional<ParameterFrame> parameterFrame(size_t frameIndex) const;
    std::optional<ParameterFrame> currentParameterFrame(const Panel& panel) const;

    Limits defaultLimits() const noexcept;
    const std::optional<Limits>& fileLimits() const noexcept;

    std::vector<EnvelopeColumn> computeEnvelope(SampleRange visibleSamples,
                                                size_t pixelWidth,
                                                std::string& error) const;
    std::vector<ConnectedPoint> computeConnectedPoints(SampleRange visibleSamples,
                                                       size_t pixelWidth,
                                                       size_t maxPoints,
                                                       std::string& error) const;
    std::vector<SpectrumBin> computeSpectrum(size_t frameIndex,
                                             size_t frameLength,
                                             std::string& error) const;

    // Drawing path. Returns a pointer to kFftCutOff dB values owned by the
    // track, or nullptr when the frame runs past the data. The result is
    // memoised on (frameIndex, frameLength), so repaints that do not move the
    // frame cost nothing, and no allocation happens per repaint.
    const float* spectrumDb(size_t frameIndex, size_t frameLength, size_t frameCount = 1) const;

    // Grey scale spectrogram for a span wider than one transform, memoised on
    // (firstSample, spanSamples, pixelWidth). Returns nullptr when the span is
    // degenerate. See core/SapSpectrogram.h.
    //
    // This is the per view fallback, used only until the whole file cache
    // below has finished building.
    const core::SpectrogramImage* spectrogram(size_t firstSample,
                                              size_t spanSamples,
                                              int pixelWidth) const;

    // Whole file spectrogram cache. The analysis grid is anchored to the file,
    // so scrolling reduces cached columns instead of re-running transforms.
    // See core/SapSpectrogramCache.h.
    void ensureSpectrogramCache() const;
    bool spectrogramCacheReady() const noexcept;
    bool spectrogramCacheBuilding() const noexcept;
    int spectrogramCacheProgress() const noexcept;
    // Changes every time a chunk of the background analysis lands.
    uint64_t spectrogramCacheGeneration() const noexcept;

    // A grey image sized exactly to the destination rectangle, memoised on the
    // whole request. Returns nullptr while the cache is still building.
    const core::SpectrogramView* spectrogramView(size_t firstSample,
                                                 size_t spanSamples,
                                                 int width,
                                                 int height,
                                                 float dynamicRangeDb,
                                                 double lowHz,
                                                 double highHz) const;

private:
    bool loadFromDisk(std::string& error);
    void invalidateSpectrumCache() const noexcept;

    // Held by shared_ptr so that copying a Track (Panel::reloadAll does) and
    // handing the audio to the background spectrogram build are both free.
    // Every mutation re-seats the pointer, so sharing never leaks across copies.
    std::vector<int16_t>& audio() noexcept { return *audioSamples_; }
    const std::vector<int16_t>& audio() const noexcept { return *audioSamples_; }

    std::filesystem::path path_;
    TrackType type_ = TrackType::WaveformPcm16;
    std::shared_ptr<std::vector<int16_t>> audioSamples_ =
        std::make_shared<std::vector<int16_t>>();
    // Only the frames present in the file, sorted by frameIndex; the
    // navigable count can be far larger when the numbering has gaps.
    std::vector<ParameterFrame> parameterFrames_;
    size_t parameterFrameTotal_ = 0;
    std::optional<Limits> fileLimits_;
    bool open_ = false;
    bool wavFile_ = false;
    uint32_t sampleRate_ = 0;
    uint32_t rawPcmSampleRate_ = 16000;
    long headerLength_ = 0;
    int channel_ = 0;
    int channelCount_ = 1;
    core::WavSampleFormat sampleFormat_ = core::WavSampleFormat::Pcm16;

    // WinSAP keeps its FFT scratch buffers inside the Panel; XSap keeps them
    // per track so computeSpectrum() allocates nothing per repaint.
    mutable core::SapFft fft_;

    // Memoised spectrum for the drawing path.
    mutable std::vector<float> spectrumCache_;
    mutable size_t spectrumFrameIndex_ = 0;
    mutable size_t spectrumFrameLength_ = 0;
    mutable size_t spectrumFrameCount_ = 0;
    mutable bool spectrumCacheValid_ = false;

    // Memoised spectrogram for the wide-span drawing path.
    mutable core::SapSpectrogram spectrogramEngine_;
    mutable core::SpectrogramImage spectrogramCache_;
    mutable size_t spectrogramFirstSample_ = 0;
    mutable size_t spectrogramSpan_ = 0;
    mutable int spectrogramWidth_ = 0;
    mutable bool spectrogramCacheValid_ = false;

    // Whole file spectrogram. Held behind a shared pointer because Track must
    // stay copyable (Panel::reloadAll copies it) and the cache owns a thread.
    mutable std::shared_ptr<core::SapSpectrogramCache> fileSpectrogram_;

    // Memoised reduction of the above into the last requested rectangle.
    mutable core::SpectrogramView spectrogramView_;
    mutable uint64_t viewGeneration_ = 0;
    mutable size_t viewFirstSample_ = 0;
    mutable size_t viewSpan_ = 0;
    mutable int viewWidth_ = 0;
    mutable int viewHeight_ = 0;
    mutable float viewRangeDb_ = 0.0f;
    mutable double viewLowHz_ = 0.0;
    mutable double viewHighHz_ = 0.0;
    mutable bool viewValid_ = false;
};

class Panel {
public:
    static constexpr size_t kMaxTracks = 6;

    explicit Panel(bool upper) noexcept;

    bool isUpper() const noexcept;
    bool isActive() const noexcept;

    size_t frameBegin() const noexcept;
    size_t visibleFrameCount() const noexcept;
    size_t frameLength() const noexcept;
    size_t frameSkip() const noexcept;

    double lowerLimit() const noexcept;
    double upperLimit() const noexcept;
    Limits amplitudeLimits() const noexcept;

    bool setFrameBegin(size_t frameBegin, std::string& error);
    bool setFrameBeginFrom(const Panel& source, std::string& error);
    bool gotoFrame(size_t frameIndex, std::string& error);
    bool moveOneFrame(int direction, std::string& error);
    bool moveBySkip(int direction, std::string& error);
    bool zoomIn(std::string& error);
    bool zoomOut(std::string& error);
    bool viewAll(std::string& error);

    bool setVisibleFrameCount(size_t visibleFrameCount, std::string& error);
    bool setFrameLength(size_t frameLength, std::string& error);
    bool setFrameSkip(size_t frameSkip, std::string& error);

    bool setAmplitudeLimits(double lower, double upper, std::string& error);
    bool expandAmplitude(double factor, std::string& error);
    bool compressAmplitude(double factor, std::string& error);

    bool openTrack(const std::filesystem::path& path,
                   TrackType type,
                   const TrackOpenOptions& options,
                   size_t& openedSlot,
                   std::string& error);
    bool openTrackAt(size_t slot,
                     const std::filesystem::path& path,
                     TrackType type,
                     const TrackOpenOptions& options,
                     std::string& error);
    bool closeTrack(size_t slot, std::string& error);
    void closeAll() noexcept;
    // Unavailable files retain their in-memory data. Readable files that fail
    // validation are removed so stale invalid data is not left on screen.
    std::size_t reloadAll(std::vector<std::string>& preservedFailures,
                          std::vector<std::string>& emptiedFailures);

    size_t openTrackCount() const noexcept;
    size_t totalFrameCount() const noexcept;
    size_t totalFrameCountForSlot(size_t slot) const noexcept;
    size_t maxBinarySampleCount() const noexcept;

    const Track* track(size_t slot) const noexcept;
    Track* track(size_t slot) noexcept;
    const std::array<std::optional<Track>, kMaxTracks>& tracks() const noexcept;

    SampleRange visibleSampleRange() const noexcept;
    std::optional<int64_t> sampleIndexAtPixel(size_t pixelX, size_t pixelWidth) const noexcept;
    std::optional<double> pixelOffsetForSample(int64_t sampleIndex, size_t pixelWidth) const noexcept;

    bool setPlaybackCursor(int64_t sampleIndex, std::string& error);
    void clearPlaybackCursor() noexcept;
    std::optional<int64_t> playbackCursor() const noexcept;

    bool setMarkStart(int64_t sampleIndex, std::string& error);
    bool setMarkEnd(int64_t sampleIndex, std::string& error);
    void clearMark() noexcept;
    std::optional<SampleRange> selectedSampleRange() const noexcept;
    // WinSap MarkedAreaHasData / PlayPanelMark: whether the complete mark
    // covers data of the file in `slot`, and the first playable such file.
    bool markedAreaHasData(size_t slot) const noexcept;
    std::optional<size_t> firstMarkedDataSlot() const noexcept;

    bool setListenRange(int64_t begin, int64_t end, std::string& error);
    void clearListenRange() noexcept;
    std::optional<SampleRange> listenRange() const noexcept;

private:
    void clampFrameWindow() noexcept;
    void clampSampleState() noexcept;
    bool hasNavigableFrames() const noexcept;
    bool validateSamplePosition(int64_t sampleIndex, bool allowEnd, std::string& error) const;

    bool upper_ = false;
    size_t frameBegin_ = 0;
    size_t visibleFrameCount_ = 1;
    size_t frameLength_ = 160;
    size_t frameSkip_ = 10;
    double lowerLimit_ = -32768.0;
    double upperLimit_ = 32768.0;
    std::optional<int64_t> playbackCursor_;
    std::optional<int64_t> markStart_;
    std::optional<int64_t> markEnd_;
    std::optional<SampleRange> listenRange_;
    std::array<std::optional<Track>, kMaxTracks> tracks_;
};

} // namespace xsap
