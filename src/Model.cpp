/************************************************************************
 *
 *  XSap - Document Model Implementation
 *
 *  FILE:      Model.cpp
 *
 *  PURPOSE:   Loading, and every analysis a panel can display: waveform
 *             envelopes, spectra, spectrograms and parameter tracks.
 *
 *             Results are memoised against the view parameters that produced
 *             them, so scrolling and redrawing do not repeat work. The
 *             numerics themselves live in core/, transcribed from WinSap, so
 *             that both programs produce the same numbers.
 *
 *  Update:    2026.1
 *             2026.11 Spectrogram cache allocation failure is not fatal;
 *                     .par files are recognised as parameter files;
 *                     parameter frames cached sparsely, no frame limit
 *
 ************************************************************************/

#include "Model.h"

#include "core/SapFft.h"
#include "core/SapParameterFile.h"
#include "core/SapWaveFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace xsap {

namespace {

constexpr size_t kFftSize = static_cast<size_t>(core::kFftLength);
constexpr size_t kSpectrumBins = static_cast<size_t>(core::kFftCutOff);

std::string pathForError(const std::filesystem::path& path)
{
    return path.u8string();
}

/*****************************************************************************
 * clampToInt64
 *
 * Converts filesystem-sized values into the signed sample-index domain,
 * saturating rather than overflowing when a very large file is reported.
 *
 *****************************************************************************/
int64_t clampToInt64(std::uintmax_t value) noexcept
{
    const auto limit = static_cast<std::uintmax_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(value > limit ? limit : value);
}

/*****************************************************************************
 * saturatingAdd
 *
 * Adds signed sample positions with saturation so navigation arithmetic
 * cannot wrap when a view or cursor is pushed beyond representable limits.
 *
 *****************************************************************************/
int64_t saturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right) {
        return std::numeric_limits<int64_t>::min();
    }
    return left + right;
}

/*****************************************************************************
 * saturatingMulToInt64
 *
 * Multiplies frame counts into sample offsets while clamping overflow to
 * the largest signed index the panel state can safely carry.
 *
 *****************************************************************************/
int64_t saturatingMulToInt64(size_t left, size_t right) noexcept
{
    if (left == 0 || right == 0) {
        return 0;
    }

    const auto limit = static_cast<unsigned long long>(std::numeric_limits<int64_t>::max());
    const auto l = static_cast<unsigned long long>(left);
    const auto r = static_cast<unsigned long long>(right);
    if (l > limit / r) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(l * r);
}

/*****************************************************************************
 * clampRangeToSamples
 *
 * Normalises a possibly reversed sample range to the loaded buffer. Keeping
 * the range ordered here lets callers accept drag and scroll endpoints freely.
 *
 *****************************************************************************/
SampleRange clampRangeToSamples(SampleRange range, size_t sampleCount) noexcept
{
    const int64_t maxSample = clampToInt64(sampleCount);
    range.begin = std::clamp<int64_t>(range.begin, 0, maxSample);
    range.end = std::clamp<int64_t>(range.end, 0, maxSample);
    if (range.end < range.begin) {
        std::swap(range.begin, range.end);
    }
    return range;
}

/*****************************************************************************
 * firstFreeSlot
 *
 * Finds the first unused panel slot, returning the slot count as the
 * sentinel when the fixed WinSap-style track array is already full.
 *
 *****************************************************************************/
size_t firstFreeSlot(const std::array<std::optional<Track>, Panel::kMaxTracks>& tracks) noexcept
{
    for (size_t slot = 0; slot < tracks.size(); ++slot) {
        if (!tracks[slot].has_value()) {
            return slot;
        }
    }
    return tracks.size();
}

/*****************************************************************************
 * readS16Le
 *
 * Decodes one little-endian PCM sample explicitly so raw files behave the
 * same way on every host architecture.
 *
 *****************************************************************************/
int16_t readS16Le(const unsigned char* bytes) noexcept
{
    return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                                static_cast<uint16_t>(bytes[1] << 8U));
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

using FileHandle = std::unique_ptr<std::FILE, FileCloser>;

std::FILE* openBinary(const std::filesystem::path& path)
{
    return std::fopen(path.string().c_str(), "rb");
}

// WinSAP-equivalent binary loader.
//
// WinSAP (WinsapApp::OpenUpperFile -> Panel::OpenFile) probes the RIFF magic
// rather than the file extension. A WAV is read in any supported sample
// format and converted to 16-bit, keeping one channel of a stereo file. A
// non-RIFF file is treated as headerless raw PCM at the panel sample rate.
/*****************************************************************************
 * wavFormatTagName
 *
 * A readable name for a WAV format tag, used to say which format a file
 * that cannot be read is in.
 *
 *****************************************************************************/
std::string wavFormatTagName(uint16_t tag)
{
    switch (tag) {
    case 0x0001: return "integer PCM";
    case 0x0002: return "Microsoft ADPCM";
    case 0x0003: return "IEEE float";
    case 0x0006: return "A-law";
    case 0x0007: return "mu-law";
    case 0x0011: return "IMA ADPCM";
    case 0x0031: return "GSM 6.10";
    case 0x0050: return "MPEG";
    case 0x0055: return "MPEG Layer 3";
    case 0x0000: return "unknown";
    default: break;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "format tag 0x%04X", static_cast<unsigned int>(tag));
    return text;
}

/*****************************************************************************
 * readBinaryTrack
 *
 * Loads a WAV or raw PCM track using WinSap's detection rules. The RIFF
 * probe, format and channel checks and min/max scan are kept together so
 * file limits match the samples actually cached.
 *
 *****************************************************************************/
bool readBinaryTrack(const std::filesystem::path& path,
                     uint32_t rawPcmSampleRate,
                     int channel,
                     std::vector<int16_t>& samples,
                     std::optional<Limits>& fileLimits,
                     bool& wavFile,
                     uint32_t& storedSampleRate,
                     long& headerLength,
                     int& channelCount,
                     core::WavSampleFormat& sampleFormat,
                     std::string& error)
{
    const FileHandle file(openBinary(path));
    if (!file) {
        error = "Cannot open file '" + pathForError(path) + "'.";
        return false;
    }

    const long fileLength = core::FileLength(file.get());
    if (fileLength <= 0) {
        error = "File '" + pathForError(path) + "' is empty.";
        return false;
    }

    core::WaveInfo info;
    wavFile = core::ProbeWaveFile(file.get(), info);
    channelCount = 1;
    sampleFormat = core::WavSampleFormat::Pcm16;
    if (wavFile) {
        error = wavFormatError(path, info);
        if (!error.empty()) {
            return false;
        }
    }
    if (wavFile) {
        channelCount = info.channels;
        sampleFormat = info.sampleFormat;
        if (channel < 0 || channel >= channelCount) {
            error = "WAV file '" + pathForError(path) + "' has no channel " +
                    std::to_string(channel + 1) + ".";
            return false;
        }
    }

    long dataOffset = 0;
    long dataLength = fileLength;
    if (wavFile) {
        dataOffset = info.headerLength;
        if (dataOffset < 0 || dataOffset > fileLength) {
            error = "WAV file '" + pathForError(path) + "' has an out-of-range data chunk.";
            return false;
        }
        const long available = fileLength - dataOffset;
        dataLength = info.dataLength > 0U ? static_cast<long>(info.dataLength) : available;
        if (dataLength > available) {
            dataLength = available;
        }
        storedSampleRate = info.samplingRate;
    } else {
        storedSampleRate = rawPcmSampleRate;
    }
    headerLength = dataOffset;

    const size_t frameBytes = wavFile
        ? static_cast<size_t>(core::WavBytesPerSample(sampleFormat)) * static_cast<size_t>(channelCount)
        : sizeof(int16_t);
    const size_t sampleCount = static_cast<size_t>(dataLength) / frameBytes;
    if (sampleCount == 0) {
        error = wavFile ? "File '" + pathForError(path) + "' contains no samples."
                        : "File '" + pathForError(path) + "' contains no 16-bit samples.";
        return false;
    }

    std::vector<unsigned char> bytes;
    try {
        if (!wavFile) {
            bytes.resize(sampleCount * sizeof(int16_t));
        }
        samples.resize(sampleCount);
    } catch (const std::bad_alloc&) {
        error = "Out of memory while loading '" + pathForError(path) + "'.";
        return false;
    }

    if (std::fseek(file.get(), dataOffset, SEEK_SET) != 0) {
        error = "Failed while reading '" + pathForError(path) + "'.";
        return false;
    }
    if (wavFile) {
        bool read = false;
        try {
            read = core::ReadWavChannel(file.get(), sampleCount, channelCount, channel,
                                        sampleFormat, samples.data());
        } catch (const std::bad_alloc&) {
            error = "Out of memory while loading '" + pathForError(path) + "'.";
            return false;
        }
        if (!read) {
            error = "Failed while reading '" + pathForError(path) + "'.";
            return false;
        }
    } else {
        if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
            error = "Failed while reading '" + pathForError(path) + "'.";
            return false;
        }
        for (size_t i = 0; i < sampleCount; ++i) {
            samples[i] = readS16Le(bytes.data() + (i * 2U));
        }
    }

    int16_t minValue = std::numeric_limits<int16_t>::max();
    int16_t maxValue = std::numeric_limits<int16_t>::min();
    for (size_t i = 0; i < sampleCount; ++i) {
        minValue = std::min(minValue, samples[i]);
        maxValue = std::max(maxValue, samples[i]);
    }

    fileLimits = Limits{static_cast<double>(minValue), static_cast<double>(maxValue)};
    return true;
}

// WinSAP-equivalent parameter loader.
//
// WinSAP buffers the whole file (Panel::OpenFile), indexes it with
// BuildParameterIndex, derives the frame count with ParFileTotal, and renders
// frames through DispID. The same three steps are performed here and the
// resulting text is cached per frame.
/*****************************************************************************
 * readParameterFile
 *
 * Loads and indexes a WinSap parameter text file, caching the frame header
 * and display lines that the UI needs without reparsing during redraws.
 *
 *****************************************************************************/
bool readParameterFile(const std::filesystem::path& path,
                       std::vector<ParameterFrame>& frames,
                       size_t& frameTotal,
                       std::string& error)
{
    const FileHandle file(openBinary(path));
    if (!file) {
        error = "Cannot open parameter file '" + pathForError(path) + "'.";
        return false;
    }

    const long fileLength = core::FileLength(file.get());
    if (fileLength <= 0) {
        error = "Parameter file '" + pathForError(path) + "' is empty.";
        return false;
    }

    std::vector<char> buffer;
    try {
        buffer.resize(static_cast<size_t>(fileLength) + 1U);
    } catch (const std::bad_alloc&) {
        error = "Out of memory while loading parameter file '" + pathForError(path) + "'.";
        return false;
    }

    if (std::fread(buffer.data(), 1, static_cast<size_t>(fileLength), file.get()) !=
        static_cast<size_t>(fileLength)) {
        error = "Failed while reading parameter file '" + pathForError(path) + "'.";
        return false;
    }
    buffer[static_cast<size_t>(fileLength)] = '\0';

    std::fseek(file.get(), 0L, SEEK_SET);
    const long totalFrames = core::ParFileTotal(file.get());

    // Only the frames present are cached, so memory follows the FRAME= lines
    // in the file rather than the highest frame number.
    frames.clear();
    try {
        core::ParameterIndex index;
        core::BuildParameterIndex(buffer.data(), index);
        for (const core::ParameterIndexEntry& entry : index) {
            std::string header;
            std::string line1;
            std::string line2;
            if (!core::GetParameterHeader(buffer.data(), index, totalFrames, entry.frame, header)) {
                continue;
            }
            core::GetParameterLines(buffer.data(), index, totalFrames, entry.frame, line1, line2);
            frames.push_back(ParameterFrame{static_cast<size_t>(entry.frame), header, line1, line2});
        }
    } catch (const std::bad_alloc&) {
        frames.clear();
        error = "Out of memory while loading parameter file '" + pathForError(path) + "'.";
        return false;
    }

    if (frames.empty()) {
        error = "Parameter file '" + pathForError(path) + "' does not contain any FRAME= sections.";
        return false;
    }

    frameTotal = static_cast<size_t>(totalFrames);
    return true;
}

} // namespace

/*****************************************************************************
 * isParameterFileName
 *
 * Tests for the .par extension without regard to case, as WinSap does on
 * Windows, so FOO.PAR and foo.par open the same way.
 *
 *****************************************************************************/
bool isParameterFileName(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension == ".par";
}

/*****************************************************************************
 * open
 *
 * Initialises the track identity before delegating the actual file load, so a
 * failed open leaves no stale cached analysis behind.
 *
 *****************************************************************************/
bool Track::open(const std::filesystem::path& path,
                 TrackType type,
                 uint32_t rawPcmSampleRate,
                 std::string& error)
{
    return open(path, type, rawPcmSampleRate, 0, error);
}

bool Track::open(const std::filesystem::path& path,
                 TrackType type,
                 uint32_t rawPcmSampleRate,
                 int channel,
                 std::string& error)
{
    path_ = path;
    type_ = type;
    rawPcmSampleRate_ = rawPcmSampleRate;
    channel_ = channel;
    open_ = false;
    return loadFromDisk(error);
}

/*****************************************************************************
 * reload
 *
 * Reloads an already-open track from its original path, preserving the track
 * configuration while refreshing all data derived from the file.
 *
 *****************************************************************************/
bool Track::reload(std::string& error)
{
    if (!open_) {
        error = "Cannot reload a track that is not open.";
        return false;
    }
    return loadFromDisk(error);
}

/*****************************************************************************
 * close
 *
 * Returns the track to its unopened state and drops every analysis cache.
 * Resetting the shared sample buffer keeps later accessors safe to call.
 *
 *****************************************************************************/
void Track::close() noexcept
{
    path_.clear();
    audioSamples_ = std::make_shared<std::vector<int16_t>>();
    parameterFrames_.clear();
    parameterFrameTotal_ = 0;
    fileLimits_.reset();
    wavFile_ = false;
    sampleRate_ = 0;
    rawPcmSampleRate_ = 16000;
    headerLength_ = 0;
    channel_ = 0;
    channelCount_ = 1;
    sampleFormat_ = core::WavSampleFormat::Pcm16;
    open_ = false;
    invalidateSpectrumCache();
}

bool Track::isOpen() const noexcept
{
    return open_;
}

bool Track::isBinary() const noexcept
{
    return type_ == TrackType::WaveformPcm16 || type_ == TrackType::FftAmplitude;
}

bool Track::isWav() const noexcept
{
    return wavFile_;
}

const std::filesystem::path& Track::path() const noexcept
{
    return path_;
}

TrackType Track::type() const noexcept
{
    return type_;
}

uint32_t Track::sampleRate() const noexcept
{
    return sampleRate_;
}

uint32_t Track::effectiveSampleRate(uint32_t fallbackSampleRate) const noexcept
{
    return sampleRate_ != 0 ? sampleRate_ : fallbackSampleRate;
}

long Track::headerLength() const noexcept
{
    return headerLength_;
}

int Track::channel() const noexcept
{
    return channel_;
}

int Track::channelCount() const noexcept
{
    return channelCount_;
}

bool Track::isStereoChannel() const noexcept
{
    return open_ && wavFile_ && channelCount_ == 2;
}

core::WavSampleFormat Track::sampleFormat() const noexcept
{
    return sampleFormat_;
}

/*****************************************************************************
 * setRawPcmSampleRate
 *
 * Changes the rate only for open raw PCM tracks. The spectrogram cache is
 * rebuilt because its frequency scale and window geometry depend on that rate.
 *
 *****************************************************************************/
bool Track::setRawPcmSampleRate(uint32_t sampleRate, std::string& error)
{
    if (sampleRate == 0U) {
        error = "Raw PCM sample rate must be greater than zero.";
        return false;
    }
    if (!open_ || !isBinary() || wavFile_) {
        error = "Sample rate can only be changed for an open raw PCM track.";
        return false;
    }
    rawPcmSampleRate_ = sampleRate;
    sampleRate_ = sampleRate;
    // The spectrogram geometry is derived from the rate, so re-analyse.
    invalidateSpectrumCache();
    ensureSpectrogramCache();
    return true;
}

const std::vector<int16_t>& Track::audioSamples() const noexcept
{
    return audio();
}

/*****************************************************************************
 * copySamples
 *
 * Returns a clipped copy of the requested binary sample span for playback or
 * export code that needs ownership rather than a view into the track buffer.
 *
 *****************************************************************************/
std::vector<int16_t> Track::copySamples(SampleRange range) const
{
    if (!isBinary()) {
        return {};
    }

    range = clampRangeToSamples(range, audio().size());
    if (range.empty()) {
        return {};
    }

    const size_t begin = static_cast<size_t>(range.begin);
    const size_t end = static_cast<size_t>(range.end);
    return std::vector<int16_t>(audio().begin() + static_cast<std::ptrdiff_t>(begin),
                                audio().begin() + static_cast<std::ptrdiff_t>(end));
}

std::vector<int16_t> Track::copyAllSamples() const
{
    return audio();
}

size_t Track::sampleCount() const noexcept
{
    return audio().size();
}

/*****************************************************************************
 * totalFrames
 *
 * Reports the number of frames available for the current track type, using
 * parameter sections for text tracks and fixed-length chunks for audio.
 *
 *****************************************************************************/
size_t Track::totalFrames(size_t frameLength) const noexcept
{
    if (!open_) {
        return 0;
    }
    if (type_ == TrackType::ParameterText) {
        return parameterFrameTotal_;
    }
    if (frameLength == 0 || audio().empty()) {
        return 0;
    }
    return audio().size() / frameLength;
}

const std::vector<ParameterFrame>& Track::parameterFrames() const noexcept
{
    return parameterFrames_;
}

size_t Track::parameterFrameCount() const noexcept
{
    return parameterFrameTotal_;
}

/*****************************************************************************
 * parameterFrame
 *
 * Fetches one cached parameter frame when it exists and contains displayable
 * text, mirroring WinSap's habit of ignoring empty indexed slots.
 *
 *****************************************************************************/
std::optional<ParameterFrame> Track::parameterFrame(size_t frameIndex) const
{
    if (type_ != TrackType::ParameterText || frameIndex >= parameterFrameTotal_) {
        return std::nullopt;
    }
    const auto frame = std::lower_bound(
        parameterFrames_.begin(), parameterFrames_.end(), frameIndex,
        [](const ParameterFrame& item, size_t index) { return item.frameIndex < index; });
    if (frame == parameterFrames_.end() || frame->frameIndex != frameIndex) {
        return std::nullopt;
    }
    return *frame;
}

std::optional<ParameterFrame> Track::currentParameterFrame(const Panel& panel) const
{
    return parameterFrame(panel.frameBegin());
}

/*****************************************************************************
 * defaultLimits
 *
 * Provides display limits for tracks that did not supply file-derived bounds.
 * FFT amplitude uses the WinSap dB range; PCM uses full signed 16-bit scale.
 *
 *****************************************************************************/
Limits Track::defaultLimits() const noexcept
{
    if (type_ == TrackType::FftAmplitude) {
        return Limits{-120.0, 0.0};
    }
    return Limits{-32768.0, 32768.0};
}

const std::optional<Limits>& Track::fileLimits() const noexcept
{
    return fileLimits_;
}

/*****************************************************************************
 * computeEnvelope
 *
 * Reduces visible audio to one min/max column per pixel. The remainder carry
 * spreads uneven sample counts across columns without dropping edge samples.
 *
 *****************************************************************************/
std::vector<EnvelopeColumn> Track::computeEnvelope(SampleRange visibleSamples,
                                                   size_t pixelWidth,
                                                   std::string& error) const
{
    if (!open_ || !isBinary()) {
        error = "Envelope data is only available for open binary tracks.";
        return {};
    }
    if (pixelWidth == 0) {
        error = "Envelope width must be greater than zero.";
        return {};
    }

    const SampleRange panelSamples = visibleSamples;
    if (panelSamples.empty()) {
        return {};
    }
    visibleSamples = clampRangeToSamples(visibleSamples, audio().size());
    if (visibleSamples.empty()) {
        return {};
    }

    const size_t begin = static_cast<size_t>(visibleSamples.begin);
    const size_t end = static_cast<size_t>(visibleSamples.end);
    const size_t count = end - begin;
    const long double projectedColumns =
        static_cast<long double>(count) * static_cast<long double>(pixelWidth) /
        static_cast<long double>(panelSamples.length());
    const size_t columnCount =
        std::min({pixelWidth, count,
                  std::max<size_t>(1U, static_cast<size_t>(std::ceil(projectedColumns)))});
    std::vector<EnvelopeColumn> columns(columnCount);
    const size_t baseSamplesPerColumn = count / columnCount;
    const size_t extraSamples = count % columnCount;
    size_t nextColumnBegin = begin;
    size_t remainderCarry = 0;

    for (size_t x = 0; x < columnCount; ++x) {
        const size_t columnBegin = nextColumnBegin;
        size_t columnEnd = columnBegin + baseSamplesPerColumn;
        remainderCarry += extraSamples;
        if (remainderCarry >= columnCount) {
            ++columnEnd;
            remainderCarry -= columnCount;
        }
        if (columnEnd <= columnBegin) {
            columnEnd = std::min(end, columnBegin + 1U);
        }
        nextColumnBegin = std::min(columnEnd, end);

        int16_t minValue = audio()[columnBegin];
        int16_t maxValue = audio()[columnBegin];
        for (size_t index = columnBegin + 1U; index < columnEnd; ++index) {
            minValue = std::min(minValue, audio()[index]);
            maxValue = std::max(maxValue, audio()[index]);
        }

        columns[x] = EnvelopeColumn{
            minValue,
            maxValue,
            SampleRange{static_cast<int64_t>(columnBegin), static_cast<int64_t>(columnEnd)}};
    }

    return columns;
}

/*****************************************************************************
 * computeConnectedPoints
 *
 * Builds individual sample points only when the view is sufficiently zoomed
 * in. Wider spans fall back to envelopes to avoid huge point lists.
 *
 *****************************************************************************/
std::vector<ConnectedPoint> Track::computeConnectedPoints(SampleRange visibleSamples,
                                                          size_t pixelWidth,
                                                          size_t maxPoints,
                                                          std::string& error) const
{
    if (!open_ || !isBinary()) {
        error = "Connected sample points are only available for open binary tracks.";
        return {};
    }
    if (pixelWidth == 0) {
        error = "Connected point width must be greater than zero.";
        return {};
    }

    const SampleRange panelSamples = visibleSamples;
    if (panelSamples.empty()) {
        return {};
    }
    visibleSamples = clampRangeToSamples(visibleSamples, audio().size());
    if (visibleSamples.empty()) {
        return {};
    }

    const int64_t visibleCount = panelSamples.length();
    if (visibleCount > static_cast<int64_t>(pixelWidth) * 4) {
        return {};
    }

    const size_t begin = static_cast<size_t>(visibleSamples.begin);
    const size_t end = static_cast<size_t>(visibleSamples.end);
    const size_t pointCount = end - begin;
    if (maxPoints != 0 && pointCount > maxPoints) {
        error = "Visible sample count exceeds the connected-point limit.";
        return {};
    }

    std::vector<ConnectedPoint> points;
    try {
        points.reserve(pointCount);
    } catch (const std::bad_alloc&) {
        error = "Out of memory while preparing connected sample points.";
        return {};
    }

    const double denominator = visibleCount > 1 ? static_cast<double>(visibleCount - 1) : 1.0;
    for (size_t index = begin; index < end; ++index) {
        const double x =
            (static_cast<double>(static_cast<int64_t>(index) - panelSamples.begin) / denominator) *
            static_cast<double>(pixelWidth > 0 ? pixelWidth - 1U : 0U);
        points.push_back(ConnectedPoint{x, audio()[index], static_cast<int64_t>(index)});
    }

    return points;
}

// WinSAP Panel::DispSpectrum, numeric part. The dB values, the kFftCutOff bin
// count and the 0 dB = full-scale-sine reference all come from WinSAP; the
// FFT itself is core::SapFft, a direct transcription of Panel::FFT.
/*****************************************************************************
 * computeSpectrum
 *
 * Computes spectrum bins for one frame using the WinSap FFT scaling and
 * cut-off. Frequencies are attached here so callers do not duplicate that math.
 *
 *****************************************************************************/
std::vector<SpectrumBin> Track::computeSpectrum(size_t frameIndex,
                                                size_t frameLength,
                                                std::string& error) const
{
    if (!open_ || !isBinary()) {
        error = "Spectrum data is only available for open binary tracks.";
        return {};
    }
    if (frameLength == 0) {
        error = "Frame length must be greater than zero.";
        return {};
    }

    std::vector<SpectrumBin> bins;
    std::vector<float> spectrumDb;
    try {
        bins.reserve(kSpectrumBins);
        spectrumDb.assign(kSpectrumBins, core::kFftLowLimitDb);
    } catch (const std::bad_alloc&) {
        error = "Out of memory while preparing spectrum bins.";
        return {};
    }

    const Limits limits = defaultLimits();
    if (!fft_.ComputeSpectrumDbReal(audio().data(),
                                    audio().size(),
                                    frameIndex,
                                    frameLength,
                                    static_cast<float>(limits.upper),
                                    static_cast<float>(limits.lower),
                                    spectrumDb.data())) {
        // WinSAP silently draws nothing when the frame runs past the data.
        return {};
    }

    const double sampleRate = static_cast<double>(effectiveSampleRate());
    for (size_t bin = 0; bin < kSpectrumBins; ++bin) {
        bins.push_back(SpectrumBin{
            bin,
            (sampleRate * static_cast<double>(bin)) / static_cast<double>(kFftSize),
            static_cast<double>(spectrumDb[bin])});
    }

    return bins;
}

/*****************************************************************************
 * invalidateSpectrumCache
 *
 * Invalidates every spectrum and spectrogram view derived from the samples.
 * Dropping the shared cache also stops any background build in progress.
 *
 *****************************************************************************/
void Track::invalidateSpectrumCache() const noexcept
{
    spectrumCacheValid_ = false;
    spectrogramCacheValid_ = false;
    viewValid_ = false;
    // Dropping the reference cancels and joins any build still in flight.
    fileSpectrogram_.reset();
}

// Drawing path: memoised, allocation free after the first call.
/*****************************************************************************
 * spectrumDb
 *
 * Returns a memoised dB spectrum for the drawing path. Reusing the buffer
 * keeps repeated repaints allocation-free while the frame parameters match.
 *
 *****************************************************************************/
const float* Track::spectrumDb(size_t frameIndex, size_t frameLength, size_t frameCount) const
{
    if (!open_ || !isBinary() || frameLength == 0) {
        return nullptr;
    }
    if (frameCount == 0)
        frameCount = 1;

    if (spectrumCacheValid_ && spectrumFrameIndex_ == frameIndex &&
        spectrumFrameLength_ == frameLength && spectrumFrameCount_ == frameCount) {
        return spectrumCache_.data();
    }

    try {
        spectrumCache_.resize(kSpectrumBins);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }

    const Limits limits = defaultLimits();
    if (!fft_.ComputeSpectrumDbReal(audio().data(),
                                    audio().size(),
                                    frameIndex,
                                    frameLength,
                                    static_cast<float>(limits.upper),
                                    static_cast<float>(limits.lower),
                                    spectrumCache_.data(),
                                    frameCount)) {
        spectrumCacheValid_ = false;
        return nullptr;
    }

    spectrumFrameIndex_ = frameIndex;
    spectrumFrameLength_ = frameLength;
    spectrumFrameCount_ = frameCount;
    spectrumCacheValid_ = true;
    return spectrumCache_.data();
}

// Wide-span drawing path. See core/SapSpectrogram.h for why this mode exists.
/*****************************************************************************
 * spectrogram
 *
 * Builds the wide-span spectrogram image used when the view covers more data
 * than the whole-file cache can sensibly render at pixel resolution.
 *
 *****************************************************************************/
const core::SpectrogramImage* Track::spectrogram(size_t firstSample,
                                                 size_t spanSamples,
                                                 int pixelWidth) const
{
    if (!open_ || !isBinary() || spanSamples == 0 || pixelWidth <= 0) {
        return nullptr;
    }

    if (spectrogramCacheValid_ && spectrogramFirstSample_ == firstSample &&
        spectrogramSpan_ == spanSamples && spectrogramWidth_ == pixelWidth) {
        return &spectrogramCache_;
    }

    if (!spectrogramEngine_.Build(audio().data(),
                                  audio().size(),
                                  firstSample,
                                  spanSamples,
                                  pixelWidth,
                                  spectrogramCache_)) {
        spectrogramCacheValid_ = false;
        return nullptr;
    }

    spectrogramFirstSample_ = firstSample;
    spectrogramSpan_ = spanSamples;
    spectrogramWidth_ = pixelWidth;
    spectrogramCacheValid_ = true;
    return &spectrogramCache_;
}

/*
 * Whole file spectrogram.
 *
 * Started as soon as the file is read rather than on the first spectrogram
 * repaint, so the analysis overlaps with the user looking at the waveform and
 * the first switch into spectrum mode does not stall.
 */
/*****************************************************************************
 * ensureSpectrogramCache
 *
 * Starts whole-file spectrogram analysis as soon as audio is available.
 * Doing the work in the background hides the first spectrum-mode cost.
 *
 *****************************************************************************/
void Track::ensureSpectrogramCache() const
{
    if (!open_ || !isBinary() || audio().empty()) {
        return;
    }
    if (fileSpectrogram_) {
        return;
    }

    const unsigned int rate = sampleRate_ != 0 ? sampleRate_ : rawPcmSampleRate_;
    if (rate == 0) {
        return;
    }

    // Analysis is optional: if the cache cannot even be allocated the views
    // simply stay on the per view fallback instead of terminating a repaint.
    try {
        fileSpectrogram_ = std::make_shared<core::SapSpectrogramCache>();
    } catch (const std::bad_alloc&) {
        return;
    }
    fileSpectrogram_->BuildAsync(audioSamples_, static_cast<int>(rate));
}

bool Track::spectrogramCacheReady() const noexcept
{
    return fileSpectrogram_ && fileSpectrogram_->ready();
}

bool Track::spectrogramCacheBuilding() const noexcept
{
    return fileSpectrogram_ && fileSpectrogram_->building();
}

int Track::spectrogramCacheProgress() const noexcept
{
    return fileSpectrogram_ ? fileSpectrogram_->progress() : 0;
}

uint64_t Track::spectrogramCacheGeneration() const noexcept
{
    return fileSpectrogram_ ? fileSpectrogram_->generation() : 0U;
}

/*****************************************************************************
 * spectrogramView
 *
 * Renders a filtered view from the whole-file spectrogram cache, reusing it
 * until either the requested geometry or the cache generation changes.
 *
 *****************************************************************************/
const core::SpectrogramView* Track::spectrogramView(size_t firstSample,
                                                    size_t spanSamples,
                                                    int width,
                                                    int height,
                                                    float dynamicRangeDb,
                                                    double lowHz,
                                                    double highHz) const
{
    ensureSpectrogramCache();
    if (!fileSpectrogram_ || !fileSpectrogram_->usable()) {
        return nullptr;
    }

    // The generation changes as chunks land, so a partially built view is
    // re-rendered until the analysis catches up with it.
    const uint64_t generation = fileSpectrogram_->generation();
    if (viewValid_ && viewGeneration_ == generation && viewFirstSample_ == firstSample &&
        viewSpan_ == spanSamples && viewWidth_ == width && viewHeight_ == height &&
        viewRangeDb_ == dynamicRangeDb && viewLowHz_ == lowHz && viewHighHz_ == highHz) {
        return &spectrogramView_;
    }

    if (!fileSpectrogram_->Render(firstSample, spanSamples, width, height, dynamicRangeDb,
                                  lowHz, highHz, spectrogramView_)) {
        viewValid_ = false;
        return nullptr;
    }

    viewGeneration_ = generation;
    viewFirstSample_ = firstSample;
    viewSpan_ = spanSamples;
    viewWidth_ = width;
    viewHeight_ = height;
    viewRangeDb_ = dynamicRangeDb;
    viewLowHz_ = lowHz;
    viewHighHz_ = highHz;
    viewValid_ = true;
    return &spectrogramView_;
}

/*****************************************************************************
 * loadFromDisk
 *
 * Refreshes the track from disk and clears all derived state before reading.
 * The loader follows WinSap's type-specific paths for binary and parameter data.
 *
 *****************************************************************************/
bool Track::loadFromDisk(std::string& error)
{
    invalidateSpectrumCache();
    audioSamples_ = std::make_shared<std::vector<int16_t>>();
    parameterFrames_.clear();
    parameterFrameTotal_ = 0;
    fileLimits_.reset();
    wavFile_ = false;
    sampleRate_ = 0;
    headerLength_ = 0;
    channelCount_ = 1;
    sampleFormat_ = core::WavSampleFormat::Pcm16;

    if (path_.empty()) {
        error = "Track path is empty.";
        return false;
    }

    std::error_code existsError;
    if (!std::filesystem::exists(path_, existsError)) {
        error = "Track file '" + pathForError(path_) + "' does not exist.";
        return false;
    }
    if (existsError) {
        error = "Cannot access '" + pathForError(path_) + "': " + existsError.message();
        return false;
    }

    bool ok = false;
    if (type_ == TrackType::ParameterText) {
        ok = readParameterFile(path_, parameterFrames_, parameterFrameTotal_, error);
        if (!ok) {
            open_ = false;
            return false;
        }
    } else {
        // WinSAP decides on the RIFF magic, not on the file extension.
        ok = readBinaryTrack(path_,
                             rawPcmSampleRate_,
                             channel_,
                             audio(),
                             fileLimits_,
                             wavFile_,
                             sampleRate_,
                             headerLength_,
                             channelCount_,
                             sampleFormat_,
                             error);
        if (!ok) {
            open_ = false;
            return false;
        }
    }

    open_ = true;

    // Analyse the whole file now, on a background thread, so that switching
    // into spectrum mode later does not have to wait for it.
    ensureSpectrogramCache();
    return true;
}

/*****************************************************************************
 * Panel
 *
 * Creates one panel model and records whether it represents the upper display
 * area, matching the two-panel structure inherited from WinSap.
 *
 *****************************************************************************/
Panel::Panel(bool upper) noexcept
    : upper_(upper) {}

bool Panel::isUpper() const noexcept
{
    return upper_;
}

bool Panel::isActive() const noexcept
{
    return openTrackCount() != 0U;
}

size_t Panel::frameBegin() const noexcept
{
    return frameBegin_;
}

size_t Panel::visibleFrameCount() const noexcept
{
    return visibleFrameCount_;
}

size_t Panel::frameLength() const noexcept
{
    return frameLength_;
}

size_t Panel::frameSkip() const noexcept
{
    return frameSkip_;
}

double Panel::lowerLimit() const noexcept
{
    return lowerLimit_;
}

double Panel::upperLimit() const noexcept
{
    return upperLimit_;
}

Limits Panel::amplitudeLimits() const noexcept
{
    return Limits{lowerLimit_, upperLimit_};
}

/*****************************************************************************
 * setFrameBegin
 *
 * Moves the frame window to an explicit start and then clamps it to the data
 * currently loaded in the panel.
 *
 *****************************************************************************/
bool Panel::setFrameBegin(size_t frameBegin, std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot set frame begin without any open tracks.";
        return false;
    }
    frameBegin_ = frameBegin;
    clampFrameWindow();
    return true;
}

bool Panel::setFrameBeginFrom(const Panel& source, std::string& error)
{
    if (!source.hasNavigableFrames()) {
        error = "Cannot copy frame begin from an empty panel.";
        return false;
    }
    return setFrameBegin(source.frameBegin(), error);
}

bool Panel::gotoFrame(size_t frameIndex, std::string& error)
{
    return setFrameBegin(frameIndex, error);
}

/*****************************************************************************
 * moveOneFrame
 *
 * Nudges the view by a signed frame count, using saturated arithmetic so
 * large command repeats cannot wrap the frame index.
 *
 *****************************************************************************/
bool Panel::moveOneFrame(int direction, std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot move frames without any open tracks.";
        return false;
    }

    const int64_t current = static_cast<int64_t>(frameBegin_);
    const int64_t target = saturatingAdd(current, static_cast<int64_t>(direction));
    frameBegin_ = static_cast<size_t>(std::max<int64_t>(0, target));
    clampFrameWindow();
    return true;
}

/*****************************************************************************
 * moveBySkip
 *
 * Moves the view by multiples of the configured frame skip, preserving the
 * WinSap distinction between single-frame and skip navigation.
 *
 *****************************************************************************/
bool Panel::moveBySkip(int direction, std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot move by frame skip without any open tracks.";
        return false;
    }

    const int64_t step = static_cast<int64_t>(frameSkip_);
    const int64_t delta = direction >= 0 ? step * static_cast<int64_t>(direction)
                                         : -step * static_cast<int64_t>(-direction);
    const int64_t current = static_cast<int64_t>(frameBegin_);
    frameBegin_ = static_cast<size_t>(std::max<int64_t>(0, saturatingAdd(current, delta)));
    clampFrameWindow();
    return true;
}

/*****************************************************************************
 * zoomIn
 *
 * Halves the visible frame count around the current window, then clamps the
 * result so zooming near the end of the file stays in range.
 *
 *****************************************************************************/
bool Panel::zoomIn(std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot zoom without any open tracks.";
        return false;
    }
    if (visibleFrameCount_ <= 1U) {
        return true;
    }
    const size_t newFrameCount = std::max<size_t>(1U, visibleFrameCount_ / 2U);
    frameBegin_ += newFrameCount / 2U;
    visibleFrameCount_ = newFrameCount;
    clampFrameWindow();
    return true;
}

/*****************************************************************************
 * zoomOut
 *
 * Expands the visible frame count and shifts the start backwards to keep the
 * old focus roughly centred while respecting the available data.
 *
 *****************************************************************************/
bool Panel::zoomOut(std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot zoom without any open tracks.";
        return false;
    }
    const size_t totalFrames = totalFrameCount();
    const size_t newFrameCount =
        std::min(totalFrames,
                 std::max<size_t>(visibleFrameCount_ + 1U, visibleFrameCount_ * 2U));
    const size_t shift = newFrameCount / 4U;
    frameBegin_ = frameBegin_ > shift ? frameBegin_ - shift : 0U;
    visibleFrameCount_ = newFrameCount;
    clampFrameWindow();
    return true;
}

/*****************************************************************************
 * viewAll
 *
 * Resets the window to cover every frame currently available in the panel.
 *
 *****************************************************************************/
bool Panel::viewAll(std::string& error)
{
    if (!hasNavigableFrames()) {
        error = "Cannot switch to view-all without any open tracks.";
        return false;
    }
    frameBegin_ = 0;
    visibleFrameCount_ = totalFrameCount();
    return true;
}

/*****************************************************************************
 * setVisibleFrameCount
 *
 * Stores a requested window size, immediately constraining it when tracks are
 * open and otherwise keeping it as the default for later loads.
 *
 *****************************************************************************/
bool Panel::setVisibleFrameCount(size_t visibleFrameCount, std::string& error)
{
    if (visibleFrameCount == 0U) {
        error = "Visible frame count must be greater than zero.";
        return false;
    }
    if (hasNavigableFrames()) {
        visibleFrameCount_ = std::min(visibleFrameCount, totalFrameCount());
        clampFrameWindow();
    } else {
        visibleFrameCount_ = visibleFrameCount;
    }
    return true;
}

/*****************************************************************************
 * setFrameLength
 *
 * Changes the frame size used for audio-derived views and reclamps cursor and
 * selection state because the sample window may have moved.
 *
 *****************************************************************************/
bool Panel::setFrameLength(size_t frameLength, std::string& error)
{
    if (frameLength == 0U) {
        error = "Frame length must be greater than zero.";
        return false;
    }
    frameLength_ = frameLength;
    clampFrameWindow();
    clampSampleState();
    return true;
}

/*****************************************************************************
 * setFrameSkip
 *
 * Updates the skip distance used by navigation commands after rejecting the
 * zero step that would make movement appear to succeed without moving.
 *
 *****************************************************************************/
bool Panel::setFrameSkip(size_t frameSkip, std::string& error)
{
    if (frameSkip == 0U) {
        error = "Frame skip must be greater than zero.";
        return false;
    }
    frameSkip_ = frameSkip;
    return true;
}

/*****************************************************************************
 * setAmplitudeLimits
 *
 * Validates and stores the display amplitude range. Keeping the invariant here
 * lets zoom commands compose through the same path.
 *
 *****************************************************************************/
bool Panel::setAmplitudeLimits(double lower, double upper, std::string& error)
{
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower == upper) {
        error = "Amplitude limits must be finite and different.";
        return false;
    }
    if (lower > upper) {
        std::swap(lower, upper);
    }
    lowerLimit_ = lower;
    upperLimit_ = upper;
    return true;
}

/*****************************************************************************
 * expandAmplitude
 *
 * Narrows the amplitude span around its current centre, leaving validation to
 * the shared limit setter.
 *
 *****************************************************************************/
bool Panel::expandAmplitude(double factor, std::string& error)
{
    if (!(factor > 0.0 && factor < 1.0)) {
        error = "Expand factor must be greater than 0 and less than 1.";
        return false;
    }
    const double center = (lowerLimit_ + upperLimit_) / 2.0;
    const double halfSpan = ((upperLimit_ - lowerLimit_) / 2.0) * factor;
    return setAmplitudeLimits(center - halfSpan, center + halfSpan, error);
}

/*****************************************************************************
 * compressAmplitude
 *
 * Widens the amplitude span around its current centre, using the same setter
 * as manual limit changes so invalid ranges are rejected consistently.
 *
 *****************************************************************************/
bool Panel::compressAmplitude(double factor, std::string& error)
{
    if (factor <= 1.0) {
        error = "Compress factor must be greater than 1.";
        return false;
    }
    const double center = (lowerLimit_ + upperLimit_) / 2.0;
    const double halfSpan = ((upperLimit_ - lowerLimit_) / 2.0) * factor;
    return setAmplitudeLimits(center - halfSpan, center + halfSpan, error);
}

/*****************************************************************************
 * openTrack
 *
 * Chooses the requested slot or the first free one, then opens the track there
 * so callers can support both explicit and automatic placement.
 *
 *****************************************************************************/
bool Panel::openTrack(const std::filesystem::path& path,
                      TrackType type,
                      const TrackOpenOptions& options,
                      size_t& openedSlot,
                      std::string& error)
{
    const size_t slot = options.slot.value_or(firstFreeSlot(tracks_));
    if (slot >= kMaxTracks) {
        error = "No free track slot is available.";
        return false;
    }
    if (!openTrackAt(slot, path, type, options, error)) {
        return false;
    }
    openedSlot = slot;
    return true;
}

/*****************************************************************************
 * openTrackAt
 *
 * Opens a track in a specific slot after validating the fixed slot array.
 * A track that extends the panel duration switches to View All; shorter
 * additions keep the zoom span but align the shared window at frame zero.
 *
 *****************************************************************************/
bool Panel::openTrackAt(size_t slot,
                        const std::filesystem::path& path,
                        TrackType type,
                        const TrackOpenOptions& options,
                        std::string& error)
{
    if (slot >= kMaxTracks) {
        error = "Track slot " + std::to_string(slot) + " is outside the valid range 0.." +
                std::to_string(kMaxTracks - 1U) + ".";
        return false;
    }
    if (tracks_[slot].has_value()) {
        error = "Track slot " + std::to_string(slot) + " is already occupied.";
        return false;
    }

    const size_t previousTotalFrames = totalFrameCount();
    Track track;
    if (!track.open(path, type, options.rawPcmSampleRate, options.channel, error)) {
        return false;
    }

    tracks_[slot] = std::move(track);
    const size_t newTotalFrames = totalFrameCount();
    frameBegin_ = 0;
    if (newTotalFrames > previousTotalFrames) {
        visibleFrameCount_ = newTotalFrames;
    } else {
        clampFrameWindow();
    }
    clampSampleState();
    return true;
}

/*****************************************************************************
 * closeTrack
 *
 * Removes one track slot and resets navigation state when the panel becomes
 * empty; otherwise surviving tracks define the new valid ranges.
 *
 *****************************************************************************/
bool Panel::closeTrack(size_t slot, std::string& error)
{
    if (slot >= kMaxTracks) {
        error = "Track slot " + std::to_string(slot) + " is outside the valid range 0.." +
                std::to_string(kMaxTracks - 1U) + ".";
        return false;
    }
    if (!tracks_[slot].has_value()) {
        error = "Track slot " + std::to_string(slot) + " is already empty.";
        return false;
    }

    tracks_[slot].reset();
    if (!hasNavigableFrames()) {
        frameBegin_ = 0;
        visibleFrameCount_ = 1;
        clearPlaybackCursor();
        clearMark();
        clearListenRange();
    } else {
        clampFrameWindow();
        clampSampleState();
    }
    return true;
}

/*****************************************************************************
 * closeAll
 *
 * Clears every track and restores the panel to the small default frame window
 * with no cursor, mark or listen range.
 *
 *****************************************************************************/
void Panel::closeAll() noexcept
{
    for (auto& track : tracks_) {
        track.reset();
    }
    frameBegin_ = 0;
    visibleFrameCount_ = 1;
    clearPlaybackCursor();
    clearMark();
    clearListenRange();
}

/*****************************************************************************
 * reloadAll
 *
 * Reloads every open track, one slot at a time. Files that cannot be opened for
 * reading keep their in-memory copy. If a readable file fails format/open
 * validation, its slot is emptied rather than retaining stale invalid data.
 *
 *****************************************************************************/
std::size_t Panel::reloadAll(std::vector<std::string>& preservedFailures,
                             std::vector<std::string>& emptiedFailures)
{
    std::size_t reloadedCount = 0;
    for (size_t slot = 0; slot < tracks_.size(); ++slot) {
        if (!tracks_[slot].has_value()) {
            continue;
        }

        const std::filesystem::path path = tracks_[slot]->path();
        std::ifstream probe(path, std::ios::binary);
        if (!probe.is_open()) {
            preservedFailures.push_back(path.string() + ": file is unavailable or unreadable");
            continue;
        }
        probe.close();

        std::string error;
        Track refreshed = *tracks_[slot];
        if (!refreshed.reload(error)) {
            emptiedFailures.push_back(path.string() + ": " + error);
            tracks_[slot].reset();
            continue;
        }
        tracks_[slot] = std::move(refreshed);
        ++reloadedCount;
    }

    if (hasNavigableFrames()) {
        clampFrameWindow();
        clampSampleState();
    } else {
        frameBegin_ = 0;
        visibleFrameCount_ = 1;
    }
    return reloadedCount;
}

/*****************************************************************************
 * openTrackCount
 *
 * Counts occupied track slots in the fixed panel array.
 *
 *****************************************************************************/
size_t Panel::openTrackCount() const noexcept
{
    size_t count = 0;
    for (const auto& track : tracks_) {
        count += track.has_value() ? 1U : 0U;
    }
    return count;
}

/*****************************************************************************
 * totalFrameCount
 *
 * Returns the largest frame count among open tracks, which keeps the shared
 * panel window long enough for the longest loaded display.
 *
 *****************************************************************************/
size_t Panel::totalFrameCount() const noexcept
{
    // While a stereo file is open only stereo channels set the length, so both
    // panels share one range; longer mono data beyond it is not shown.
    bool stereoOnly = false;
    for (const auto& track : tracks_) {
        if (track.has_value() && track->isStereoChannel()) {
            stereoOnly = true;
            break;
        }
    }
    size_t total = 0;
    for (const auto& track : tracks_) {
        if (!track.has_value() || (stereoOnly && !track->isStereoChannel())) {
            continue;
        }
        total = std::max(total, track->totalFrames(frameLength_));
    }
    return total;
}

/*****************************************************************************
 * totalFrameCountForSlot
 *
 * Reports the frame count for one slot, treating invalid or empty slots as
 * zero so callers can probe the fixed array safely.
 *
 *****************************************************************************/
size_t Panel::totalFrameCountForSlot(size_t slot) const noexcept
{
    if (slot >= kMaxTracks || !tracks_[slot].has_value()) {
        return 0;
    }
    return tracks_[slot]->totalFrames(frameLength_);
}

/*****************************************************************************
 * maxBinarySampleCount
 *
 * Finds the longest binary track in the panel. Sample-based cursors and
 * selections use that span because parameter tracks have no sample timeline.
 *
 *****************************************************************************/
size_t Panel::maxBinarySampleCount() const noexcept
{
    size_t total = 0;
    for (const auto& track : tracks_) {
        if (!track.has_value() || !track->isBinary()) {
            continue;
        }
        total = std::max(total, track->sampleCount());
    }
    return total;
}

/*****************************************************************************
 * track
 *
 * Overload returning a const raw pointer for one slot, or null when the slot is
 * outside the fixed array or currently empty.
 *
 *****************************************************************************/
const Track* Panel::track(size_t slot) const noexcept
{
    if (slot >= kMaxTracks || !tracks_[slot].has_value()) {
        return nullptr;
    }
    return &*tracks_[slot];
}

/*****************************************************************************
 * track
 *
 * Overload returning a mutable raw pointer for one slot, with the same null
 * sentinel as the const form for invalid or empty slots.
 *
 *****************************************************************************/
Track* Panel::track(size_t slot) noexcept
{
    if (slot >= kMaxTracks || !tracks_[slot].has_value()) {
        return nullptr;
    }
    return &*tracks_[slot];
}

const std::array<std::optional<Track>, Panel::kMaxTracks>& Panel::tracks() const noexcept
{
    return tracks_;
}

/*****************************************************************************
 * visibleSampleRange
 *
 * Converts the frame window into a sample range using saturating arithmetic,
 * so very large frame settings cannot overflow the panel state.
 *
 *****************************************************************************/
SampleRange Panel::visibleSampleRange() const noexcept
{
    const int64_t start = saturatingMulToInt64(frameBegin_, frameLength_);
    const int64_t span = saturatingMulToInt64(visibleFrameCount_, frameLength_);
    return SampleRange{start, saturatingAdd(start, span)};
}

/*****************************************************************************
 * sampleIndexAtPixel
 *
 * Maps a pixel column back to a sample index in the current visible range,
 * returning no value when the panel has no meaningful sample geometry.
 *
 *****************************************************************************/
std::optional<int64_t> Panel::sampleIndexAtPixel(size_t pixelX, size_t pixelWidth) const noexcept
{
    if (pixelWidth == 0U || pixelX >= pixelWidth) {
        return std::nullopt;
    }
    const SampleRange visible = visibleSampleRange();
    if (visible.empty()) {
        return std::nullopt;
    }
    const int64_t offset = (visible.length() * static_cast<int64_t>(pixelX)) /
                           static_cast<int64_t>(pixelWidth);
    return visible.begin + offset;
}

/*****************************************************************************
 * pixelOffsetForSample
 *
 * Maps a sample index into the current pixel coordinate system for cursor and
 * selection drawing, rejecting samples outside the visible range.
 *
 *****************************************************************************/
std::optional<double> Panel::pixelOffsetForSample(int64_t sampleIndex, size_t pixelWidth) const noexcept
{
    if (pixelWidth == 0U) {
        return std::nullopt;
    }
    const SampleRange visible = visibleSampleRange();
    if (visible.empty() || sampleIndex < visible.begin || sampleIndex >= visible.end) {
        return std::nullopt;
    }
    const double offset = static_cast<double>(sampleIndex - visible.begin) /
                          static_cast<double>(visible.length());
    return offset * static_cast<double>(pixelWidth);
}

/*****************************************************************************
 * setPlaybackCursor
 *
 * Stores the playback cursor after validating it against the binary sample
 * timeline; unlike marks, the cursor must point at an actual sample.
 *
 *****************************************************************************/
bool Panel::setPlaybackCursor(int64_t sampleIndex, std::string& error)
{
    if (!validateSamplePosition(sampleIndex, false, error)) {
        return false;
    }
    playbackCursor_ = sampleIndex;
    return true;
}

/*****************************************************************************
 * clearPlaybackCursor
 *
 * Removes the playback cursor without changing any selection or listen range.
 *
 *****************************************************************************/
void Panel::clearPlaybackCursor() noexcept
{
    playbackCursor_.reset();
}

std::optional<int64_t> Panel::playbackCursor() const noexcept
{
    return playbackCursor_;
}

/*****************************************************************************
 * setMarkStart
 *
 * Stores the start endpoint of the marked range after allowing the file-end
 * sentinel used by drag selections.
 *
 *****************************************************************************/
bool Panel::setMarkStart(int64_t sampleIndex, std::string& error)
{
    if (!validateSamplePosition(sampleIndex, true, error)) {
        return false;
    }
    markStart_ = sampleIndex;
    return true;
}

/*****************************************************************************
 * setMarkEnd
 *
 * Stores the end endpoint of the marked range after applying the same bounds
 * rules as the start endpoint.
 *
 *****************************************************************************/
bool Panel::setMarkEnd(int64_t sampleIndex, std::string& error)
{
    if (!validateSamplePosition(sampleIndex, true, error)) {
        return false;
    }
    markEnd_ = sampleIndex;
    return true;
}

/*****************************************************************************
 * clearMark
 *
 * Clears both mark endpoints together so callers never observe a half-selected
 * range.
 *
 *****************************************************************************/
void Panel::clearMark() noexcept
{
    markStart_.reset();
    markEnd_.reset();
}

/*****************************************************************************
 * selectedSampleRange
 *
 * Returns the marked range in ascending order once both endpoints are present,
 * letting drag direction remain a UI concern.
 *
 *****************************************************************************/
std::optional<SampleRange> Panel::selectedSampleRange() const noexcept
{
    if (!markStart_.has_value() || !markEnd_.has_value()) {
        return std::nullopt;
    }
    SampleRange selected{std::min(*markStart_, *markEnd_), std::max(*markStart_, *markEnd_)};
    return selected;
}

/*****************************************************************************
 * markedAreaHasData
 *
 * WinSap MarkedAreaHasData: the mark is complete, not empty, and starts
 * inside the file in `slot`, so playing it would produce sound.
 *
 *****************************************************************************/
bool Panel::markedAreaHasData(size_t slot) const noexcept
{
    const Track* opened = track(slot);
    const std::optional<SampleRange> range = selectedSampleRange();
    if (opened == nullptr || !opened->isOpen() || !range.has_value()) {
        return false;
    }
    return range->end > range->begin && range->begin >= 0 &&
           range->begin < clampToInt64(opened->sampleCount());
}

/*****************************************************************************
 * firstMarkedDataSlot
 *
 * The first playable file (waveform or FFT) with data in the marked area,
 * which is what Play Up / Play Down play (WinSap PlayPanelMark).
 *
 *****************************************************************************/
std::optional<size_t> Panel::firstMarkedDataSlot() const noexcept
{
    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        const Track* opened = track(slot);
        if (opened != nullptr && opened->isBinary() && markedAreaHasData(slot)) {
            return slot;
        }
    }
    return std::nullopt;
}

/*****************************************************************************
 * setListenRange
 *
 * Stores an explicit listen range after validating both endpoints and keeping
 * the begin-before-end invariant required by playback.
 *
 *****************************************************************************/
bool Panel::setListenRange(int64_t begin, int64_t end, std::string& error)
{
    if (!validateSamplePosition(begin, true, error) || !validateSamplePosition(end, true, error)) {
        return false;
    }
    if (end < begin) {
        error = "Listen range end must not be before the begin position.";
        return false;
    }
    listenRange_ = SampleRange{begin, end};
    return true;
}

/*****************************************************************************
 * clearListenRange
 *
 * Clears the listen range while leaving marks and playback cursor untouched.
 *
 *****************************************************************************/
void Panel::clearListenRange() noexcept
{
    listenRange_.reset();
}

std::optional<SampleRange> Panel::listenRange() const noexcept
{
    return listenRange_;
}

/*****************************************************************************
 * clampFrameWindow
 *
 * Constrains the frame window to the currently loaded tracks, preserving at
 * least one visible frame whenever any frame data exists.
 *
 *****************************************************************************/
void Panel::clampFrameWindow() noexcept
{
    const size_t totalFrames = totalFrameCount();
    if (totalFrames == 0U) {
        frameBegin_ = 0;
        return;
    }

    visibleFrameCount_ = std::max<size_t>(1U, std::min(visibleFrameCount_, totalFrames));
    const size_t maxBegin = totalFrames > visibleFrameCount_ ? (totalFrames - visibleFrameCount_) : 0U;
    frameBegin_ = std::min(frameBegin_, maxBegin);
}

/*****************************************************************************
 * clampSampleState
 *
 * Keeps cursor, mark and listen endpoints inside the available binary sample
 * span after tracks or frame geometry change.
 *
 *****************************************************************************/
void Panel::clampSampleState() noexcept
{
    const size_t maxSamples = maxBinarySampleCount();
    if (maxSamples == 0U) {
        clearPlaybackCursor();
        clearMark();
        clearListenRange();
        return;
    }

    const int64_t sampleCount = clampToInt64(maxSamples);
    const int64_t lastSample = sampleCount - 1;

    if (playbackCursor_.has_value()) {
        *playbackCursor_ = std::clamp(*playbackCursor_, int64_t{0}, lastSample);
    }
    if (markStart_.has_value()) {
        *markStart_ = std::clamp(*markStart_, int64_t{0}, sampleCount);
    }
    if (markEnd_.has_value()) {
        *markEnd_ = std::clamp(*markEnd_, int64_t{0}, sampleCount);
    }
    if (listenRange_.has_value()) {
        listenRange_->begin = std::clamp(listenRange_->begin, int64_t{0}, sampleCount);
        listenRange_->end = std::clamp(listenRange_->end, int64_t{0}, sampleCount);
        if (listenRange_->end < listenRange_->begin) {
            std::swap(listenRange_->begin, listenRange_->end);
        }
    }
}

bool Panel::hasNavigableFrames() const noexcept
{
    return totalFrameCount() != 0U;
}

/*****************************************************************************
 * validateSamplePosition
 *
 * Checks a sample position against the panel's binary timeline, optionally
 * allowing the one-past-end value used for range endpoints.
 *
 *****************************************************************************/
bool Panel::validateSamplePosition(int64_t sampleIndex, bool allowEnd, std::string& error) const
{
    if (sampleIndex < 0) {
        error = "Sample index must not be negative.";
        return false;
    }

    const size_t maxSamples = maxBinarySampleCount();
    if (maxSamples == 0U) {
        error = "No binary track is available for sample-based operations.";
        return false;
    }

    const int64_t maxIndex = clampToInt64(maxSamples);
    const int64_t upperBound = allowEnd ? maxIndex : maxIndex - 1;
    if (sampleIndex > upperBound) {
        error = "Sample index " + std::to_string(sampleIndex) + " is outside the valid range 0.." +
                std::to_string(std::max<int64_t>(0, upperBound)) + ".";
        return false;
    }
    return true;
}

/*****************************************************************************
 * probeWavFile
 *
 * The 'fmt ' and 'data' details of a WAV file, or nothing when the file is
 * not a WAV or cannot be read. Lets the UI route a stereo file to both
 * panels, and refuse an unreadable format, before opening it.
 *
 *****************************************************************************/
std::optional<core::WaveInfo> probeWavFile(const std::filesystem::path& path)
{
    const FileHandle file(openBinary(path));
    if (!file) {
        return std::nullopt;
    }
    core::WaveInfo info;
    if (!core::ProbeWaveFile(file.get(), info)) {
        return std::nullopt;
    }
    return info;
}

/*****************************************************************************
 * wavFormatError
 *
 * Why a WAV cannot be loaded, naming its format, or empty when it can.
 *
 *****************************************************************************/
std::string wavFormatError(const std::filesystem::path& path, const core::WaveInfo& info)
{
    if (info.channels > 2U) {
        return "Only mono and stereo WAV files are supported.";
    }
    if (info.sampleFormat == core::WavSampleFormat::Unsupported) {
        return "'" + pathForError(path) + "' uses a WAV sample format that is not supported (" +
               wavFormatTagName(info.formatTag) + ", " + std::to_string(info.bitsPerSample) +
               "-bit).\nSupported formats are 8, 16, 24 and 32-bit integer PCM and 32 or 64-bit float.";
    }
    return {};
}

} // namespace xsap
