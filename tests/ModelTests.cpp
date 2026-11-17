/************************************************************************
 *
 *  XSap - Model and Analysis Tests
 *
 *  FILE:      tests/ModelTests.cpp
 *
 *  PURPOSE:   Regression tests for the parts of the program that produce
 *             numbers rather than pixels: file loading, the FFT paths, the
 *             spectrum and spectrogram geometry, and the parameter tracks.
 *
 *             Several of these exist to pin down behaviour that has been got
 *             wrong before, so a test that stops failing when its assertion
 *             is inverted is a broken test, not a passing one.
 *
 *  Update:    2026.1
 *             2026.11 .par recognition, bounded parameter frames, atomic save
 *
 ************************************************************************/

#include "Model.h"

#include "core/SapFft.h"
#include "core/SapParameterFile.h"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using xsap::Panel;
using xsap::SampleRange;
using xsap::Track;
using xsap::TrackOpenOptions;
using xsap::TrackType;

/*****************************************************************************
 * require
 *
 * The whole assertion mechanism. Throws on failure so that main can report
 * the message and return non-zero; there is no assert here because the tests
 * must fail the same way in a release build.
 *
 *****************************************************************************/
void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/*****************************************************************************
 * writeU16
 *
 * Writes a 16 bit field little endian by hand rather than dumping the raw
 * bytes, so the fixtures are the same on any host.
 *
 *****************************************************************************/
void writeU16(std::ofstream& output, std::uint16_t value)
{
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
}

/*****************************************************************************
 * writeU32
 *
 * Writes a 32 bit field little endian, as the RIFF format requires.
 *
 *****************************************************************************/
void writeU32(std::ofstream& output, std::uint32_t value)
{
    writeU16(output, static_cast<std::uint16_t>(value & 0xffffU));
    writeU16(output, static_cast<std::uint16_t>(value >> 16U));
}

/*****************************************************************************
 * writeSamples
 *
 * Writes PCM samples little endian, whatever the host's own byte order.
 *
 *****************************************************************************/
void writeSamples(std::ofstream& output, const std::vector<std::int16_t>& samples)
{
    for (const auto sample : samples) {
        writeU16(output, static_cast<std::uint16_t>(sample));
    }
}

/*****************************************************************************
 * writeWave
 *
 * Writes a minimal but valid mono 16 bit WAV fixture: RIFF header, fmt chunk
 * and data chunk. Deliberately hand written rather than produced by the
 * program's own writer, so that a fault in the reader cannot be masked by a
 * matching fault in the writer.
 *
 *****************************************************************************/
void writeWave(const std::filesystem::path& path,
               const std::vector<std::int16_t>& samples,
               std::uint32_t sampleRate)
{
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2U);
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    writeU32(output, 4U + 12U + 8U + 16U + 8U + dataBytes);
    output.write("WAVE", 4);
    output.write("JUNK", 4);
    writeU32(output, 3);
    output.write("abc", 3);
    output.put('\0');
    output.write("fmt ", 4);
    writeU32(output, 16);
    writeU16(output, 1);
    writeU16(output, 1);
    writeU32(output, sampleRate);
    writeU32(output, sampleRate * 2U);
    writeU16(output, 2);
    writeU16(output, 16);
    output.write("data", 4);
    writeU32(output, dataBytes);
    writeSamples(output, samples);
    require(static_cast<bool>(output), "failed to write WAV fixture");
}

/*****************************************************************************
 * testRawAndPanel
 *
 * Opens a headerless PCM file and exercises the panel state built on it:
 * sample addressing, the waveform envelope, and the marked range.
 *
 *****************************************************************************/
void testRawAndPanel(const std::filesystem::path& directory)
{
    const auto path = directory / "samples.raw";
    std::vector<std::int16_t> samples(1605);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(static_cast<int>(i) - 160);
    }
    {
        std::ofstream output(path, std::ios::binary);
        writeSamples(output, samples);
    }

    Panel panel(true);
    require(!panel.isActive(), "an empty panel should not participate");
    std::size_t slot = 99;
    std::string error;
    TrackOpenOptions options;
    options.rawPcmSampleRate = 8000;
    require(panel.openTrack(path, TrackType::WaveformPcm16, options, slot, error), error);
    require(panel.isActive(), "a panel with an open track should participate automatically");
    require(slot == 0, "first track did not use slot zero");
    require(panel.totalFrameCount() == 10, "binary frame count does not match WinSAP");
    require(panel.setVisibleFrameCount(4, error), error);
    require(panel.gotoFrame(99, error), error);
    require(panel.frameBegin() == 6, "frame window did not clamp to the final full view");
    const auto visible = panel.visibleSampleRange();
    require(visible.begin == 960 && visible.end == 1600,
            "visible sample range uses wrong frame semantics");
    require(panel.gotoFrame(4, error), error);
    require(panel.zoomIn(error), error);
    require(panel.frameBegin() == 5 && panel.visibleFrameCount() == 2,
            "zoom in did not preserve the center of the WinSAP view");
    require(panel.zoomOut(error), error);
    require(panel.frameBegin() == 4 && panel.visibleFrameCount() == 4,
            "zoom out did not preserve the center of the WinSAP view");
    require(panel.setFrameSkip(1, error), error);
    require(panel.moveBySkip(-1, error) && panel.frameBegin() == 3, "frame skip navigation failed");
    require(panel.setAmplitudeLimits(20.0, -10.0, error), error);
    require(panel.lowerLimit() == -10.0 && panel.upperLimit() == 20.0,
            "reversed amplitude limits were not normalized");

    const Track* track = panel.track(0);
    require(track != nullptr && track->sampleRate() == 8000, "raw PCM sample rate was not retained");
    const auto envelope = track->computeEnvelope(SampleRange{0, 3}, 20, error);
    require(envelope.size() == 3, "narrow envelope should not create invalid empty columns");
    require(envelope.front().minimum == -160 && envelope.back().maximum == -158,
            "envelope values are incorrect");
    require(panel.setMarkStart(10, error) && panel.setMarkEnd(20, error), error);
    const auto marked = panel.selectedSampleRange();
    require(marked.has_value() && marked->begin == 10 && marked->end == 20,
            "marked range was not retained");
    panel.closeAll();
    require(!panel.isActive(), "a panel should stop participating after its tracks are closed");
}

void testLockedFrameBeginUsesUpperResult(const std::filesystem::path& directory)
{
    const auto upperPath = directory / "lock-upper.raw";
    const auto lowerPath = directory / "lock-lower.raw";
    {
        std::ofstream upper(upperPath, std::ios::binary);
        writeSamples(upper, std::vector<std::int16_t>(3200, 1));
        std::ofstream lower(lowerPath, std::ios::binary);
        writeSamples(lower, std::vector<std::int16_t>(800, 1));
    }

    Panel upper(true);
    Panel lower(false);
    TrackOpenOptions options;
    std::size_t slot = 0;
    std::string error;
    require(upper.openTrack(upperPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(lower.openTrack(lowerPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(upper.setVisibleFrameCount(2, error) && lower.setVisibleFrameCount(2, error), error);
    require(upper.gotoFrame(12, error), error);
    require(lower.setFrameBeginFrom(upper, error), error);
    require(upper.frameBegin() == 12, "upper test position was not retained");
    require(lower.frameBegin() == 3,
            "locked lower start must use and clamp the resulting upper start");

    require(lower.gotoFrame(0, error), error);
    require(upper.frameBegin() == 12, "lower navigation must not drive upper");
}

/*****************************************************************************
 * testOpenExpandsOnlyForLongerTrack
 *
 * The first file opens in View All. Later files expand the panel only when
 * they are longer than every track already present. Shorter additions preserve
 * the zoom span but return the shared frame window to zero.
 *
 *****************************************************************************/
void testOpenExpandsOnlyForLongerTrack(const std::filesystem::path& directory)
{
    const auto shortPath = directory / "open-short.raw";
    const auto shorterPath = directory / "open-shorter.raw";
    const auto longPath = directory / "open-long.raw";
    const auto mediumPath = directory / "open-medium.raw";
    {
        std::ofstream output(shortPath, std::ios::binary);
        std::vector<std::int16_t> samples(300U * 160U, 0);
        samples[150U * 160U] = 1000;
        writeSamples(output, samples);
    }
    {
        std::ofstream output(shorterPath, std::ios::binary);
        writeSamples(output, std::vector<std::int16_t>(160, 1));
    }
    {
        std::ofstream output(longPath, std::ios::binary);
        writeSamples(output, std::vector<std::int16_t>(3200U * 160U, 1));
    }
    {
        std::ofstream output(mediumPath, std::ios::binary);
        writeSamples(output, std::vector<std::int16_t>(800U * 160U, 1));
    }

    Panel panel(true);
    TrackOpenOptions options;
    std::size_t slot = 99;
    std::string error;
    require(panel.openTrack(shortPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(panel.frameBegin() == 0 && panel.visibleFrameCount() == 300,
            "the first opened track did not start in View All");

    require(panel.setVisibleFrameCount(1, error) && panel.gotoFrame(1, error), error);
    require(panel.openTrack(shorterPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(panel.frameBegin() == 0 && panel.visibleFrameCount() == 1,
            "a shorter track did not align the panel at frame zero");

    require(panel.openTrack(longPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(panel.frameBegin() == 0 && panel.visibleFrameCount() == 3200,
            "a longer track did not expand the panel to View All");
    const Track* shortTrack = panel.track(0);
    require(shortTrack != nullptr, "the short alignment track was not retained");
    const auto aligned = shortTrack->computeEnvelope(panel.visibleSampleRange(), 3200, error);
    require(aligned.size() == 300,
            "the short track was stretched across the longer shared timeline");
    require(aligned[150].maximum == 1000,
            "the short-track pulse did not remain at its shared frame position");

    require(panel.setVisibleFrameCount(2, error) && panel.gotoFrame(4, error), error);
    require(panel.openTrack(mediumPath, TrackType::WaveformPcm16, options, slot, error), error);
    require(panel.frameBegin() == 0 && panel.visibleFrameCount() == 2,
            "a later shorter track did not preserve zoom from frame zero");
}

/*****************************************************************************
 * testWaveAndSpectrum
 *
 * Reads a WAV fixture and checks the spectrum it produces.
 *
 * The dB scale is absolute, referenced to a full scale sine, so halving the
 * amplitude must move the peak by 6 dB. An earlier version normalised each
 * frame against its own peak, which made every frame read 0 dB whatever its
 * level; the 6 dB check is what catches a return to that.
 *
 *****************************************************************************/
void testWaveAndSpectrum(const std::filesystem::path& directory)
{
    constexpr std::uint32_t sampleRate = 16000;
    constexpr std::size_t sampleCount = 2048;
    constexpr double amplitude = 20000.0;
    std::vector<std::int16_t> samples(sampleCount);
    std::vector<std::int16_t> quiet(sampleCount);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 1000.0 *
                             static_cast<double>(i) / sampleRate;
        samples[i] = static_cast<std::int16_t>(std::sin(phase) * amplitude);
        quiet[i] = static_cast<std::int16_t>(std::sin(phase) * (amplitude / 2.0));
    }

    const auto path = directory / "tone.wav";
    writeWave(path, samples, sampleRate);

    Track track;
    std::string error;
    require(track.open(path, TrackType::FftAmplitude, 8000, error), error);
    require(track.isWav() && track.sampleRate() == sampleRate, "WAV metadata was not parsed");
    require(track.headerLength() > 0, "WAV data offset was not reported");
    const auto waveformLimits = Track{}.defaultLimits();
    require(waveformLimits.lower == -32768.0 && waveformLimits.upper == 32768.0,
            "16-bit waveform limits do not match WinSAP");
    const auto fftLimits = track.defaultLimits();
    require(fftLimits.lower == -120.0 && fftLimits.upper == 0.0,
            "FFT amplitude limits do not match WinSAP");

    const auto spectrum = track.computeSpectrum(0, sampleCount, error);
    require(spectrum.size() == static_cast<std::size_t>(xsap::core::kFftCutOff),
            "spectrum bin count is incorrect");
    const auto peak = std::max_element(
        spectrum.begin(), spectrum.end(),
        [](const auto& left, const auto& right) { return left.valueDb < right.valueDb; });
    require(peak != spectrum.end(), "spectrum is empty");
    require(std::abs(peak->frequencyHz - 1000.0) < 10.0, "spectrum peak frequency is incorrect");

    // WinSAP references 0 dB to a full scale sine wave, so the peak of a
    // Hamming-windowed sine of amplitude A lands at 20*log10(0.54*A/32768).
    const double expectedPeakDb = 20.0 * std::log10(0.54 * amplitude / 32768.0);
    require(std::abs(peak->valueDb - expectedPeakDb) < 0.5,
            "spectrum peak is not referenced to a full scale sine wave");

    // Halving the signal must lower the peak by ~6 dB. A peak-relative
    // normalisation would report 0 dB for both.
    const auto quietPath = directory / "tone-quiet.wav";
    writeWave(quietPath, quiet, sampleRate);
    Track quietTrack;
    require(quietTrack.open(quietPath, TrackType::FftAmplitude, 8000,
                            error),
            error);
    const auto quietSpectrum = quietTrack.computeSpectrum(0, sampleCount, error);
    const auto quietPeak = std::max_element(
        quietSpectrum.begin(), quietSpectrum.end(),
        [](const auto& left, const auto& right) { return left.valueDb < right.valueDb; });
    require(quietPeak != quietSpectrum.end(), "quiet spectrum is empty");
    require(std::abs((peak->valueDb - quietPeak->valueDb) - 6.0206) < 0.2,
            "spectrum dB scale is not absolute");

    // WinSAP draws nothing when the requested frame runs past the data.
    require(track.computeSpectrum(4, sampleCount, error).empty(),
            "out-of-range frame should produce no spectrum");
}

/*****************************************************************************
 * testRawIsDetectedByMagicNotExtension
 *
 * A file is treated as WAV because it carries a RIFF header, not because of
 * its name. A headerless file called .wav must therefore fall back to raw PCM
 * at the rate the user supplied, rather than reading its first 44 samples as a
 * header.
 *
 *****************************************************************************/
void testRawIsDetectedByMagicNotExtension(const std::filesystem::path& directory)
{
    // WinSAP probes the RIFF magic, so a WAV keeps its header even when the
    // extension lies, and a headerless file is raw PCM whatever it is called.
    constexpr std::uint32_t sampleRate = 16000;
    std::vector<std::int16_t> samples(512);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(i);
    }

    const auto disguised = directory / "actually-a-wav.pcm";
    writeWave(disguised, samples, sampleRate);

    Track wavTrack;
    std::string error;
    require(wavTrack.open(disguised, TrackType::WaveformPcm16, 8000, error),
            error);
    require(wavTrack.isWav(), "RIFF file with a .pcm extension was not detected as WAV");
    require(wavTrack.sampleRate() == sampleRate, "disguised WAV sample rate was not parsed");
    require(wavTrack.sampleCount() == samples.size(), "disguised WAV payload length is wrong");
    require(wavTrack.audioSamples().front() == 0 && wavTrack.audioSamples().back() == 511,
            "disguised WAV payload was read from the wrong offset");

    const auto rawNamedWav = directory / "actually-raw.wav";
    {
        std::ofstream output(rawNamedWav, std::ios::binary);
        writeSamples(output, samples);
    }
    Track rawTrack;
    require(rawTrack.open(rawNamedWav, TrackType::WaveformPcm16, 8000,
                          error),
            error);
    require(!rawTrack.isWav(), "headerless file with a .wav extension was treated as WAV");
    require(rawTrack.sampleRate() == 8000, "raw PCM fallback sample rate was not applied");
    require(rawTrack.headerLength() == 0, "raw PCM should have no header");
}

/*****************************************************************************
 * testParameters
 *
 * Parameter text files must be indexed exactly as WinSap indexes them, since
 * the two programs are expected to read the same files.
 *
 * WinSap matches "FRAME=" only at the start of a line and only in upper case,
 * and derives the frame count from the highest number it finds, so gaps in the
 * numbering leave empty frames rather than renumbering the rest.
 *
 *****************************************************************************/
void testParameters(const std::filesystem::path& directory)
{
    // WinSAP matches "FRAME=" only at the start of a line and only in upper
    // case (Panel::BuildParameterIndex uses strncmp), and derives the frame
    // count from the highest number in the trailing 1000 bytes.
    const auto path = directory / "frames.par";
    {
        std::ofstream output(path);
        output << "FRAME=0\n"
               << "first line\n"
               << "second line\n"
               << "FRAME=3\n"
               << "third line\n"
               << "fourth line\n";
    }

    Track track;
    std::string error;
    require(track.open(path, TrackType::ParameterText, 16000, error), error);
    require(track.parameterFrameCount() == 4, "parameter frame numbers were not indexed");
    require(!track.parameterFrame(1).has_value(), "missing parameter frame should remain empty");
    const auto frame = track.parameterFrame(3);
    require(frame.has_value() && frame->line1 == "third line" && frame->line2 == "fourth line",
            "parameter frame text is incorrect");
    const auto first = track.parameterFrame(0);
    require(first.has_value() && first->header == "FRAME=0" && first->line1 == "first line",
            "first parameter frame text is incorrect");

    // An indented or lower case tag is not a frame for WinSAP.
    const auto looseTags = directory / "loose.par";
    {
        std::ofstream output(looseTags);
        output << "  frame=0\n"
               << "ignored one\n"
               << "ignored two\n"
               << "FRAME=1\n"
               << "real one\n"
               << "real two\n";
    }
    Track looseTrack;
    require(looseTrack.open(looseTags, TrackType::ParameterText, 16000,
                            error),
            error);
    require(looseTrack.parameterFrameCount() == 2, "loose parameter frame count is wrong");
    const auto loose = looseTrack.parameterFrame(1);
    require(loose.has_value() && loose->line1 == "real one",
            "upper case FRAME= tag was not indexed");
}

/*****************************************************************************
 * testFftParity
 *
 * The optimised transforms must agree with the plain WinSap one.
 *
 * Three paths exist for speed: the original complex FFT, a table driven
 * version of it, and a real input version that transforms half as many points
 * and untangles the result. They are only worth having if they produce the
 * same numbers, so all three are compared bin for bin within 0.5 dB.
 *
 *****************************************************************************/
void testFftParity()
{
    // The optimised transform must stay interchangeable with the verbatim
    // WinSAP one, otherwise the port has drifted.
    constexpr std::size_t frameLength = 512;
    std::vector<std::int16_t> samples(frameLength * 4U);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i);
        const double value = 11000.0 * std::sin(2.0 * 3.14159265358979323846 * 313.0 * t / 16000.0) +
                             4200.0 * std::sin(2.0 * 3.14159265358979323846 * 2570.0 * t / 16000.0);
        samples[i] = static_cast<std::int16_t>(std::max(-32768.0, std::min(32767.0, value)));
    }

    xsap::core::SapFft fft;
    std::vector<float> reference(xsap::core::kFftCutOff, 0.0f);
    std::vector<float> fast(xsap::core::kFftCutOff, 0.0f);

    require(fft.ComputeSpectrumDb(samples.data(), samples.size(), 1, frameLength,
                                  xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                                  reference.data()),
            "reference spectrum failed");
    require(fft.ComputeSpectrumDbFast(samples.data(), samples.size(), 1, frameLength,
                                      xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                                      fast.data()),
            "fast spectrum failed");

    std::vector<float> real(xsap::core::kFftCutOff, 0.0f);
    require(fft.ComputeSpectrumDbReal(samples.data(), samples.size(), 1, frameLength,
                                      xsap::core::kFftUpLimitDb, xsap::core::kFftLowLimitDb,
                                      real.data()),
            "real spectrum failed");

    double worstReal = 0.0;
    for (int bin = 0; bin < xsap::core::kFftCutOff; ++bin) {
        worstReal = std::max(worstReal, std::abs(static_cast<double>(reference[bin]) -
                                                 static_cast<double>(real[bin])));
    }
    require(worstReal < 0.5, "real-input transform diverged from the WinSAP transform");

    double worst = 0.0;
    for (int bin = 0; bin < xsap::core::kFftCutOff; ++bin) {
        worst = std::max(worst, std::abs(static_cast<double>(reference[bin]) -
                                         static_cast<double>(fast[bin])));
    }
    require(worst < 0.5, "optimised FFT diverged from the WinSAP transform");
}

/*****************************************************************************
 * testSpectrumHonoursFrameCount
 *
 * The frame count control must reach the transform. It used to be ignored, so
 * a 16 kHz file with 160 sample frames drew exactly the same spectrum whether
 * the panel showed one frame or six, which is what the control is for.
 *
 * Asking only that the output changes would pass on a wrong window too, so the
 * test also requires the tone's main lobe to narrow: more frames must buy
 * frequency resolution, not merely a different picture.
 *
 *****************************************************************************/
void testSpectrumHonoursFrameCount()
{
    constexpr unsigned rate = 16000;
    constexpr std::size_t frameLength = 160;
    constexpr double pi = 3.14159265358979323846;

    std::vector<std::int16_t> samples(rate);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double value = 12000.0 * std::sin(2.0 * pi * 1000.0 *
                                                static_cast<double>(i) / rate);
        samples[i] = static_cast<std::int16_t>(value);
    }

    xsap::core::SapFft fft;
    auto analyse = [&](std::size_t frameCount, std::vector<float>& out) {
        out.assign(xsap::core::kFftCutOff, 0.0f);
        require(fft.ComputeSpectrumDbReal(samples.data(), samples.size(), 1, frameLength,
                                          xsap::core::kFftUpLimitDb,
                                          xsap::core::kFftLowLimitDb,
                                          out.data(), frameCount),
                "spectrum failed");
    };

    std::vector<float> one;
    std::vector<float> five;
    analyse(1, one);
    analyse(5, five);

    bool differs = false;
    for (int bin = 0; bin < xsap::core::kFftCutOff && !differs; ++bin) {
        differs = std::abs(one[bin] - five[bin]) > 0.5f;
    }
    require(differs, "the frame count must change the analysis window");

    // A longer window is a narrower filter, so the tone's main lobe must cover
    // fewer bins. Hamming gives about 4*rate/window Hz, so 400 Hz at one frame
    // against 80 Hz at five.
    auto mainLobeBins = [](const std::vector<float>& spectrum) {
        float peak = spectrum[0];
        for (float value : spectrum)
            peak = std::max(peak, value);
        int count = 0;
        for (float value : spectrum) {
            if (value > peak - 3.0f)
                ++count;
        }
        return count;
    };

    const int wide = mainLobeBins(one);
    const int narrow = mainLobeBins(five);
    require(narrow < wide,
            "more frames must buy frequency resolution, not just redraw");
}
/*****************************************************************************
 * testSpectrogramGeometry
 *
 * The analysis window is a fixed duration rather than a fixed sample count, so
 * the effective bandwidth must come out the same at every sample rate and the
 * FFT must always be large enough to hold the window. A file too large for the
 * memory budget must widen the hop rather than refuse to build.
 *
 *****************************************************************************/
void testSpectrogramGeometry()
{
    // The window is a fixed duration, so the effective bandwidth must come out
    // the same at every sample rate, and the FFT must always be able to hold it.
    const int rates[] = {8000, 11025, 16000, 22050, 44100, 48000, 96000};
    for (const int rate : rates) {
        const auto geometry =
            xsap::core::ChooseSpectrogramGeometry(rate, static_cast<std::size_t>(rate) * 10U);
        require(geometry.valid(), "geometry invalid");
        require(geometry.fftSize >= geometry.windowLength,
                "FFT too small to hold the analysis window");
        require(geometry.fftSize >= xsap::core::kMinFftSize, "FFT below the display floor");
        require((geometry.fftSize & (geometry.fftSize - 1)) == 0, "FFT size not a power of two");
        require(geometry.bins == geometry.fftSize / 2, "bin count inconsistent");
        require(std::abs(geometry.windowMs() - xsap::core::kDefaultWindowMs) < 0.5,
                "analysis window drifted from the requested duration");
        require(std::abs(geometry.bandwidthHz() - 200.0) < 10.0,
                "effective bandwidth is not the same at every rate");
    }

    // A file too large for the budget must widen the hop rather than fail.
    const auto huge = xsap::core::ChooseSpectrogramGeometry(16000, 16000U * 3600U * 4U,
                                                            xsap::core::kDefaultWindowMs,
                                                            xsap::core::kDefaultHopMs,
                                                            8U * 1024U * 1024U);
    require(huge.valid(), "budgeted geometry invalid");
    const std::size_t hugeColumns = (16000U * 3600U * 4U) / static_cast<std::size_t>(huge.hop);
    require(hugeColumns * static_cast<std::size_t>(huge.bins) <= 8U * 1024U * 1024U,
            "hop was not widened to respect the memory budget");
}

/*****************************************************************************
 * testSpectrogramCache
 *
 * Builds a cache over half silence and half tone, then checks the properties
 * a reduction of cached dB has to have: the tone half is darker, repeating a
 * render is bit for bit identical because the analysis grid is anchored to the
 * file rather than the view, narrowing the dynamic range only ever lightens
 * the picture, and band limiting keeps or drops the tone according to whether
 * the band contains it.
 *
 *****************************************************************************/
void testSpectrogramCache()
{
    constexpr int rate = 16000;
    constexpr std::size_t count = static_cast<std::size_t>(rate) * 2U;

    // Silence for the first half, a loud tone for the second.
    std::vector<std::int16_t> samples(count, 0);
    for (std::size_t i = count / 2U; i < count; ++i) {
        const double t = static_cast<double>(i);
        samples[i] = static_cast<std::int16_t>(
            12000.0 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t / rate));
    }

    xsap::core::SapSpectrogramCache cache;
    require(cache.Build(samples.data(), samples.size(), rate, 1), "cache build failed");
    require(cache.ready(), "cache did not become ready");
    require(cache.columns() > 0U, "cache produced no columns");
    require(cache.memoryBytes() == cache.columns() * static_cast<std::size_t>(cache.bins()),
            "cache matrix size inconsistent");
    require(cache.peakDb() > 60.0f, "a full scale tone should register a high peak");

    xsap::core::SpectrogramView view;
    require(cache.Render(0, count, 200, 100, 60.0f, 0.0, 0.0, view), "render failed");
    require(view.width == 200 && view.height == 100, "render did not fill the rectangle");
    require(view.grey.size() == 200U * 100U, "render produced the wrong pixel count");

    // The silent half must be lighter than the tone half.
    long silent = 0;
    long loud = 0;
    for (int y = 0; y < view.height; ++y) {
        const std::uint8_t* row = view.grey.data() + static_cast<std::size_t>(y) * view.width;
        for (int x = 0; x < 100; ++x)
            silent += row[x];
        for (int x = 100; x < 200; ++x)
            loud += row[x];
    }
    require(loud < silent, "the tone half should be darker than the silent half");

    // Rendering the same view twice must be identical; the grid is anchored to
    // the file, so a shifted view must not change what the overlap shows.
    xsap::core::SpectrogramView again;
    require(cache.Render(0, count, 200, 100, 60.0f, 0.0, 0.0, again), "second render failed");
    require(again.grey == view.grey, "render is not deterministic");

    // A narrower dynamic range must never produce a darker picture.
    xsap::core::SpectrogramView narrow;
    require(cache.Render(0, count, 200, 100, 30.0f, 0.0, 0.0, narrow), "narrow render failed");
    for (std::size_t i = 0; i < narrow.grey.size(); ++i) {
        require(narrow.grey[i] >= view.grey[i],
                "narrowing the dynamic range must only lighten the picture");
    }

    // Band limiting must keep the tone when the band contains it and drop it
    // when the band sits above it.
    xsap::core::SpectrogramView band;
    require(cache.Render(0, count, 200, 100, 60.0f, 3000.0, 8000.0, band), "band render failed");
    long above = 0;
    for (const std::uint8_t level : band.grey)
        above += level;
    require(above > loud + silent, "a band above the tone should be much lighter");
}

/*****************************************************************************
 * testSpectrogramCacheChunking
 *
 * The cache is built a chunk at a time on a background thread, prioritised
 * around wherever the view currently sits, so that a long file becomes
 * renderable before it is finished.
 *
 * What matters is that the prioritisation cannot change the result: however
 * the chunks are ordered, the finished matrix must match a straight
 * synchronous build.
 *
 *****************************************************************************/
void testSpectrogramCacheChunking()
{
    constexpr int rate = 16000;
    constexpr std::size_t count = static_cast<std::size_t>(rate) * 40U; // many chunks

    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i);
        const double sweep = 300.0 + 2000.0 * (t / static_cast<double>(count));
        samples[i] = static_cast<std::int16_t>(
            9000.0 * std::sin(2.0 * 3.14159265358979323846 * sweep * t / rate));
    }

    // A single threaded build and a multi threaded, chunk claimed build must
    // produce the same picture, otherwise the chunking has introduced a race
    // or an off-by-one at a chunk boundary.
    xsap::core::SapSpectrogramCache serial;
    require(serial.Build(samples.data(), samples.size(), rate, 1), "serial build failed");

    xsap::core::SapSpectrogramCache parallel;
    require(parallel.Build(samples.data(), samples.size(), rate, 8), "parallel build failed");

    require(serial.columns() == parallel.columns(), "chunking changed the column count");
    require(std::abs(serial.peakDb() - parallel.peakDb()) < 0.01f,
            "chunking changed the file peak");

    xsap::core::SpectrogramView serialView;
    xsap::core::SpectrogramView parallelView;
    require(serial.Render(0, count, 400, 120, 60.0f, 0.0, 0.0, serialView), "serial render failed");
    require(parallel.Render(0, count, 400, 120, 60.0f, 0.0, 0.0, parallelView),
            "parallel render failed");
    require(serialView.grey == parallelView.grey,
            "multi threaded chunk claiming produced a different picture");

    // A background build must be renderable long before it is complete, and
    // must converge on exactly the same result.
    xsap::core::SapSpectrogramCache async;
    async.BuildAsync(samples.data(), samples.size(), rate);
    while (async.building() && !async.usable()) {
        // Wait only for the matrix to exist, not for the analysis to finish.
    }
    require(async.usable(), "a background build must become renderable immediately");

    while (!async.ready() && async.building()) {
        // Spin until the background analysis finishes.
    }
    require(async.ready(), "background build did not complete");

    xsap::core::SpectrogramView asyncView;
    require(async.Render(0, count, 400, 120, 60.0f, 0.0, 0.0, asyncView), "async render failed");
    require(asyncView.grey == serialView.grey,
            "the background build did not converge on the serial result");

    // Cancelling part way leaves a genuinely partial cache. Rendering it must
    // still fill the whole rectangle and must not read unanalysed columns.
    xsap::core::SapSpectrogramCache partialCache;
    partialCache.BuildAsync(samples.data(), samples.size(), rate);
    partialCache.Cancel();
    if (partialCache.usable()) {
        xsap::core::SpectrogramView partialView;
        require(partialCache.Render(0, count, 400, 120, 60.0f, 0.0, 0.0, partialView),
                "a cancelled, partially built cache must still render");
        require(partialView.grey.size() == 400U * 120U,
                "a partial render must still fill the whole rectangle");
    }
}

/*****************************************************************************
 * testSpectrogramFrequencyAxis
 *
 * A rendered spectrogram must place a known tone on the row the frequency axis
 * claims. Row 0 is the top of the image and carries the highest frequency, so
 * a tone at f belongs on row (1 - f / nyquist) * height. This pins down the
 * real FFT untangling, the bin to row reduction and the vertical flip at once.
 *
 *****************************************************************************/
void testSpectrogramFrequencyAxis()
{
    const int rate = 48000;
    const std::size_t count = static_cast<std::size_t>(rate); // one second
    const int width = 60;
    const int height = 240;

    for (double frequency : {1000.0, 6000.0, 12000.0, 18000.0, 23000.0}) {
        std::vector<int16_t> samples(count);
        for (std::size_t i = 0; i < count; i++) {
            samples[i] = static_cast<int16_t>(
                16000.0 * std::sin(2.0 * M_PI * frequency * static_cast<double>(i) / rate));
        }

        xsap::core::SapSpectrogramCache cache;
        require(cache.Build(samples.data(), count, rate, 1), "tone build failed");

        xsap::core::SpectrogramView view;
        require(cache.Render(0, count, width, height, 60.0f, 0.0, 0.0, view),
                "tone render failed");

        int darkest = 256;
        int darkestRow = -1;
        for (int y = 0; y < height; y++) {
            const int grey = view.grey[static_cast<std::size_t>(y) * width + width / 2];
            if (grey < darkest) {
                darkest = grey;
                darkestRow = y;
            }
        }

        const double nyquist = rate / 2.0;
        const int expected = static_cast<int>((1.0 - frequency / nyquist) * height);
        require(std::abs(darkestRow - expected) <= 2,
                "a tone was drawn on the wrong row of the frequency axis");
        require(darkest < 64, "a full scale tone must be drawn dark");
    }
}

/*****************************************************************************
 * testReloadAllMatchesWinSap
 *
 * Reload All preserves an unavailable file's in-memory samples, but removes a
 * readable slot when the replacement data fails validation.
 *
 *****************************************************************************/
void testReloadAllMatchesWinSap(const std::filesystem::path& directory)
{
    const auto goodPath = directory / "reload-good.raw";
    const auto doomedPath = directory / "reload-doomed.raw";
    const auto invalidPath = directory / "reload-invalid.raw";

    const std::vector<std::int16_t> original(320, 100);
    for (const auto& path : {goodPath, doomedPath, invalidPath}) {
        std::ofstream output(path, std::ios::binary);
        writeSamples(output, original);
    }

    Panel panel(true);
    std::string error;
    TrackOpenOptions options;
    options.rawPcmSampleRate = 8000;
    std::size_t goodSlot = 99;
    std::size_t doomedSlot = 99;
    std::size_t invalidSlot = 99;
    // The doomed file goes in first on purpose: a reload that gives up at the
    // first failure would then never reach the readable one.
    require(panel.openTrack(doomedPath, TrackType::WaveformPcm16, options, doomedSlot, error),
            error);
    require(panel.openTrack(goodPath, TrackType::WaveformPcm16, options, goodSlot, error), error);
    require(panel.openTrack(invalidPath, TrackType::WaveformPcm16, options, invalidSlot, error),
            error);

    // Change one file on disk and remove the other behind the program's back.
    const std::vector<std::int16_t> edited(640, -200);
    {
        std::ofstream output(goodPath, std::ios::binary);
        writeSamples(output, edited);
    }
    std::error_code removal;
    std::filesystem::remove(doomedPath, removal);
    require(!removal, "could not remove the fixture");
    {
        std::ofstream output(invalidPath, std::ios::binary | std::ios::trunc);
    }

    std::vector<std::string> preserved;
    std::vector<std::string> emptied;
    const std::size_t reloaded = panel.reloadAll(preserved, emptied);

    require(reloaded == 1, "reloadAll should have refreshed exactly the readable track");
    require(preserved.size() == 1, "reloadAll should preserve exactly one unavailable file");
    require(preserved.front().find("reload-doomed.raw") != std::string::npos,
            "the preserved failure should name the unavailable file");
    require(emptied.size() == 1, "reloadAll should empty exactly one invalid readable file");
    require(emptied.front().find("reload-invalid.raw") != std::string::npos,
            "the emptied failure should name the invalid file");

    const Track* good = panel.track(goodSlot);
    require(good != nullptr, "the readable track was dropped");
    require(good->sampleCount() == edited.size(),
            "the readable track did not pick up the change made on disk");

    const Track* doomed = panel.track(doomedSlot);
    require(doomed != nullptr, "an unreadable track must not be closed by Reload All");
    require(doomed->sampleCount() == original.size(),
            "an unreadable track must keep the samples already in memory");
    require(panel.track(invalidSlot) == nullptr,
            "a readable file that fails reload validation must empty its slot");
}

/*****************************************************************************
 * writeWaveBytes
 *
 * Writes a WAV fixture of any format from already encoded sample bytes. A
 * non-zero subFormat writes a WAVE_FORMAT_EXTENSIBLE header carrying that
 * tag in its SubFormat GUID.
 *
 *****************************************************************************/
void writeWaveBytes(const std::filesystem::path& path,
                    std::uint16_t formatTag,
                    std::uint16_t channels,
                    std::uint16_t bitsPerSample,
                    std::uint16_t blockAlign,
                    const std::vector<std::uint8_t>& bytes,
                    std::uint16_t subFormat = 0)
{
    const std::uint32_t fmtBytes = subFormat != 0 ? 40U : 16U;
    const auto dataBytes = static_cast<std::uint32_t>(bytes.size());
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    writeU32(output, 4U + 8U + fmtBytes + 8U + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeU32(output, fmtBytes);
    writeU16(output, subFormat != 0 ? 0xFFFEU : formatTag);
    writeU16(output, channels);
    writeU32(output, 8000U);
    writeU32(output, 8000U * blockAlign);
    writeU16(output, blockAlign);
    writeU16(output, bitsPerSample);
    if (subFormat != 0) {
        static const unsigned char guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                   0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        writeU16(output, 22);
        writeU16(output, bitsPerSample);
        writeU32(output, channels == 2 ? 3U : 4U);
        writeU16(output, subFormat);
        output.write(reinterpret_cast<const char*>(guidTail), sizeof(guidTail));
    }
    output.write("data", 4);
    writeU32(output, dataBytes);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "failed to write WAV fixture");
}

void appendLe(std::vector<std::uint8_t>& bytes, std::uint64_t value, int size)
{
    for (int i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffU));
    }
}

std::vector<std::int16_t> openWavSamples(const std::filesystem::path& path,
                                         xsap::core::WavSampleFormat expectedFormat)
{
    Track track;
    std::string error;
    require(track.open(path, TrackType::WaveformPcm16, 8000, error), error);
    require(track.isWav() && track.sampleFormat() == expectedFormat,
            "WAV sample format was not recognised: " + path.filename().string());
    return track.audioSamples();
}

/*****************************************************************************
 * testWavSampleFormats
 *
 * Every supported WAV sample format converts to 16 bits with WinSap's
 * rounding and clipping, including a WAVE_FORMAT_EXTENSIBLE header.
 *
 *****************************************************************************/
void testWavSampleFormats(const std::filesystem::path& directory)
{
    using xsap::core::WavSampleFormat;

    const auto pcm8 = directory / "pcm8.wav";
    writeWaveBytes(pcm8, 1, 1, 8, 1, {0, 128, 255, 129});
    require(openWavSamples(pcm8, WavSampleFormat::Pcm8) ==
                std::vector<std::int16_t>({-32768, 0, 32512, 256}),
            "8-bit unsigned PCM converted incorrectly");

    std::vector<std::uint8_t> bytes;
    for (const std::int32_t value : {8388607, 128, 127, -8388608, -256, 0x123456}) {
        appendLe(bytes, static_cast<std::uint32_t>(value), 3);
    }
    const auto pcm24 = directory / "pcm24.wav";
    writeWaveBytes(pcm24, 1, 1, 24, 3, bytes);
    const std::vector<std::int16_t> expected24{32767, 1, 0, -32768, -1, 4660};
    require(openWavSamples(pcm24, WavSampleFormat::Pcm24) == expected24,
            "24-bit PCM converted incorrectly");
    const auto pcm24Extensible = directory / "pcm24ext.wav";
    writeWaveBytes(pcm24Extensible, 1, 1, 24, 3, bytes, 1);
    require(openWavSamples(pcm24Extensible, WavSampleFormat::Pcm24) == expected24,
            "extensible 24-bit PCM converted incorrectly");

    bytes.clear();
    for (const std::int64_t value : {2147483647LL, 32768LL, 32767LL, -2147483648LL, 0x12345678LL}) {
        appendLe(bytes, static_cast<std::uint32_t>(value), 4);
    }
    const auto pcm32 = directory / "pcm32.wav";
    writeWaveBytes(pcm32, 1, 1, 32, 4, bytes);
    require(openWavSamples(pcm32, WavSampleFormat::Pcm32) ==
                std::vector<std::int16_t>({32767, 1, 0, -32768, 4660}),
            "32-bit PCM converted incorrectly");

    const std::vector<double> floats{1.0, -1.0, 0.5, -0.25, 2.0, 1.0 / 65536.0, -3.0};
    const std::vector<std::int16_t> expectedFloat{32767, -32768, 16384, -8192, 32767, 1, -32768};
    bytes.clear();
    for (const double value : floats) {
        const float single = static_cast<float>(value);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &single, sizeof(bits));
        appendLe(bytes, bits, 4);
    }
    const auto float32 = directory / "float32.wav";
    writeWaveBytes(float32, 3, 1, 32, 4, bytes);
    require(openWavSamples(float32, WavSampleFormat::Float32) == expectedFloat,
            "32-bit float converted incorrectly");
    const auto float32Extensible = directory / "float32ext.wav";
    writeWaveBytes(float32Extensible, 3, 1, 32, 4, bytes, 3);
    require(openWavSamples(float32Extensible, WavSampleFormat::Float32) == expectedFloat,
            "extensible 32-bit float converted incorrectly");

    bytes.clear();
    for (const double value : floats) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe(bytes, bits, 8);
    }
    const auto float64 = directory / "float64.wav";
    writeWaveBytes(float64, 3, 1, 64, 8, bytes);
    require(openWavSamples(float64, WavSampleFormat::Float64) == expectedFloat,
            "64-bit float converted incorrectly");
}

/*****************************************************************************
 * testStereoChannels
 *
 * A stereo WAV opens one channel per track, as the UI puts the left channel
 * in the upper panel and the right channel in the lower one.
 *
 *****************************************************************************/
void testStereoChannels(const std::filesystem::path& directory)
{
    std::vector<std::uint8_t> bytes;
    for (int i = 1; i <= 3; ++i) {
        appendLe(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(i * 100)), 2);
        appendLe(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(-i * 100)), 2);
    }
    const auto path = directory / "stereo.wav";
    writeWaveBytes(path, 1, 2, 16, 4, bytes);

    const auto info = xsap::probeWavFile(path);
    require(info.has_value() && info->channels == 2 && xsap::wavFormatError(path, *info).empty(),
            "a 16-bit stereo WAV should probe as loadable");

    Panel upper(true);
    Panel lower(false);
    TrackOpenOptions options;
    std::string error;
    require(upper.openTrackAt(0, path, TrackType::WaveformPcm16, options, error), error);
    options.channel = 1;
    require(lower.openTrackAt(0, path, TrackType::WaveformPcm16, options, error), error);
    const Track* left = upper.track(0);
    const Track* right = lower.track(0);
    require(left->isStereoChannel() && left->channel() == 0 && left->channelCount() == 2,
            "left channel metadata is wrong");
    require(right->isStereoChannel() && right->channel() == 1, "right channel metadata is wrong");
    require(left->audioSamples() == std::vector<std::int16_t>({100, 200, 300}),
            "left channel samples are wrong");
    require(right->audioSamples() == std::vector<std::int16_t>({-100, -200, -300}),
            "right channel samples are wrong");

    Track third;
    require(!third.open(path, TrackType::WaveformPcm16, 8000, 2, error),
            "a stereo file has no third channel");

    const auto mono = directory / "mono-for-stereo.wav";
    writeWave(mono, {1, 2, 3}, 8000);
    Track monoTrack;
    require(monoTrack.open(mono, TrackType::WaveformPcm16, 8000, error), error);
    require(!monoTrack.isStereoChannel() && monoTrack.channelCount() == 1,
            "a mono WAV must not be a stereo channel");
}

/*****************************************************************************
 * testUnsupportedWavFormats
 *
 * More than two channels, unsupported sample formats and an inconsistent
 * block alignment are refused with a message naming the problem.
 *
 *****************************************************************************/
void testUnsupportedWavFormats(const std::filesystem::path& directory)
{
    std::string error;
    const auto three = directory / "three.wav";
    writeWaveBytes(three, 1, 3, 16, 6, std::vector<std::uint8_t>(12, 0));
    Track track;
    require(!track.open(three, TrackType::WaveformPcm16, 8000, error) &&
                error.find("Only mono and stereo") != std::string::npos,
            "a three-channel WAV should be refused");

    const auto adpcm = directory / "adpcm.wav";
    writeWaveBytes(adpcm, 2, 1, 4, 256, std::vector<std::uint8_t>(256, 0));
    const auto info = xsap::probeWavFile(adpcm);
    require(info.has_value() &&
                info->sampleFormat == xsap::core::WavSampleFormat::Unsupported,
            "an ADPCM WAV should probe as unsupported");
    const std::string message = xsap::wavFormatError(adpcm, *info);
    require(message.find("not supported") != std::string::npos &&
                message.find("4-bit") != std::string::npos,
            "the unsupported-format message should name the format: " + message);
    error.clear();
    require(!track.open(adpcm, TrackType::WaveformPcm16, 8000, error) && !error.empty(),
            "an ADPCM WAV should not open");

    const auto misaligned = directory / "misaligned.wav";
    writeWaveBytes(misaligned, 1, 1, 24, 4, std::vector<std::uint8_t>(12, 0));
    error.clear();
    require(!track.open(misaligned, TrackType::WaveformPcm16, 8000, error),
            "a WAV whose block alignment does not match its format should not open");
}

/*****************************************************************************
 * testMarkedAreaHasData
 *
 * Play Up/Down play the first file with data in the complete mark, skipping
 * a file that ends before it.
 *
 *****************************************************************************/
void testMarkedAreaHasData(const std::filesystem::path& directory)
{
    const auto shortPath = directory / "short.wav";
    const auto longPath = directory / "long.wav";
    writeWave(shortPath, std::vector<std::int16_t>(100, 1), 8000);
    writeWave(longPath, std::vector<std::int16_t>(1000, 2), 8000);

    Panel panel(true);
    TrackOpenOptions options;
    std::string error;
    require(panel.openTrackAt(0, shortPath, TrackType::WaveformPcm16, options, error), error);
    require(panel.openTrackAt(1, longPath, TrackType::FftAmplitude, options, error), error);
    require(!panel.firstMarkedDataSlot().has_value(), "no file has data without a mark");

    require(panel.setMarkStart(500, error), error);
    require(!panel.markedAreaHasData(1), "a half-set mark has no data");
    require(panel.setMarkEnd(600, error), error);
    require(!panel.markedAreaHasData(0), "the mark starts after the short file ends");
    require(panel.markedAreaHasData(1), "the long file has data in the mark");
    require(panel.firstMarkedDataSlot() == std::optional<std::size_t>(1),
            "Play Up/Down should pick the first file with data in the mark");

    panel.clearMark();
    require(panel.setMarkStart(50, error) && panel.setMarkEnd(150, error), error);
    require(panel.firstMarkedDataSlot() == std::optional<std::size_t>(0),
            "the first file with data in the mark is slot zero");
}

/*****************************************************************************
 * testStereoLimitsFrameRange
 *
 * While a stereo channel is open, a longer mono file in the same panel does
 * not extend the frame range, so both panels stop at the stereo file's end.
 *
 *****************************************************************************/
void testStereoLimitsFrameRange(const std::filesystem::path& directory)
{
    std::vector<std::uint8_t> bytes;
    for (int i = 0; i < 200; ++i) {
        appendLe(bytes, 100, 2);
        appendLe(bytes, 200, 2);
    }
    const auto stereo = directory / "range-stereo.wav";
    writeWaveBytes(stereo, 1, 2, 16, 4, bytes);
    const auto mono = directory / "range-mono.wav";
    writeWave(mono, std::vector<std::int16_t>(1000, 3), 8000);

    Panel panel(true);
    TrackOpenOptions options;
    std::string error;
    require(panel.openTrackAt(0, mono, TrackType::WaveformPcm16, options, error), error);
    require(panel.setFrameLength(10, error), error);
    require(panel.totalFrameCount() == 100, "a mono-only panel spans the mono file");
    require(panel.openTrackAt(1, stereo, TrackType::WaveformPcm16, options, error), error);
    require(panel.totalFrameCount() == 20, "a stereo file limits the frame range");
    require(panel.setFrameBegin(90, error), error);
    require(panel.frameBegin() + panel.visibleFrameCount() <= 20,
            "the view must not move past the end of the stereo file");
    require(panel.closeTrack(1, error), error);
    require(panel.totalFrameCount() == 100, "closing the stereo file restores the mono range");
}

/*****************************************************************************
 * testParameterFileNames
 *
 * Parameter mode and the .par extension are hard-linked, so every spelling
 * of the extension must be recognised and nothing else.
 *
 *****************************************************************************/
void testParameterFileNames()
{
    require(xsap::isParameterFileName("frames.par") && xsap::isParameterFileName("/a/B.PAR") &&
                xsap::isParameterFileName("mixed.Par"),
            ".par in any case should be a parameter file");
    require(!xsap::isParameterFileName("frames.wav") && !xsap::isParameterFileName("x.pars") &&
                !xsap::isParameterFileName("par") && !xsap::isParameterFileName("x.res"),
            "only the .par extension selects parameter mode");
}

/*****************************************************************************
 * testParameterFramesAreUnlimited
 *
 * WinSAP once indexed parameter frames in a fixed 40000-entry table, and a
 * huge FRAME= number sized the frame cache directly. The cache now holds only
 * the frames present, so long and sparse files both work.
 *
 *****************************************************************************/
void testParameterFramesAreUnlimited(const std::filesystem::path& directory)
{
    std::string error;
    // More frames than the old 40000-entry WinSAP table.
    const auto manyFrames = directory / "many-frames.par";
    {
        std::ofstream output(manyFrames, std::ios::binary);
        for (int frame = 0; frame <= 50000; ++frame) {
            output << "FRAME=" << frame << "\nline " << frame << "\nnext " << frame << "\n";
        }
    }
    Track manyTrack;
    require(manyTrack.open(manyFrames, TrackType::ParameterText, 16000, error), error);
    require(manyTrack.parameterFrameCount() == 50001, "frames past 40000 were not counted");
    const auto late = manyTrack.parameterFrame(45678);
    require(late.has_value() && late->line1 == "line 45678" && late->line2 == "next 45678",
            "a frame past 40000 has the wrong text");

    // Sparse numbering up to the top of the range is cached per frame present,
    // so it neither allocates per frame number nor loses the last frame.
    const auto sparse = directory / "sparse-frames.par";
    {
        std::ofstream output(sparse, std::ios::binary);
        output << "FRAME=0\nfirst line\nsecond line\n"
               << "FRAME=999999999\nfar line\nfar line two\n";
    }
    Track sparseTrack;
    require(sparseTrack.open(sparse, TrackType::ParameterText, 16000, error), error);
    require(sparseTrack.parameterFrameCount() == 1000000000U,
            "a sparse frame number did not set the frame count");
    const auto far = sparseTrack.parameterFrame(999999999);
    require(far.has_value() && far->line1 == "far line", "the sparse last frame is unreadable");
    require(!sparseTrack.parameterFrame(5).has_value(), "a gap frame should be empty");

    // Numbers past the frame arithmetic range are ignored rather than trusted.
    const auto tooLarge = directory / "too-large-frame.par";
    {
        std::ofstream output(tooLarge, std::ios::binary);
        output << "FRAME=0\nfirst line\nsecond line\n"
               << "FRAME=7\nseven\nseven two\n"
               << "FRAME=2000000000\nignored\nignored two\n";
    }
    Track largeTrack;
    require(largeTrack.open(tooLarge, TrackType::ParameterText, 16000, error), error);
    require(largeTrack.parameterFrameCount() == 8,
            "an out of range FRAME= number changed the frame count");
}

/*****************************************************************************
 * testSaveKeepsOriginalOnFailure
 *
 * Save As writes a staging file and renames it over the destination, so a
 * write that fails part way leaves the existing file as it was. The failure
 * is forced with RLIMIT_FSIZE, which applies even to root.
 *
 *****************************************************************************/
void testSaveKeepsOriginalOnFailure(const std::filesystem::path& directory)
{
    const auto target = directory / "keep.wav";
    const std::vector<std::int16_t> original{1, 2, 3, 4};
    writeWave(target, original, 8000);
    const auto originalSize = std::filesystem::file_size(target);

    const std::vector<std::int16_t> large(64 * 1024, 7);
    rlimit previous{};
    require(getrlimit(RLIMIT_FSIZE, &previous) == 0, "cannot read the file size limit");
    const auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = previous;
    limited.rlim_cur = 4096;
    require(setrlimit(RLIMIT_FSIZE, &limited) == 0, "cannot lower the file size limit");
    std::string error;
    const bool saved = xsap::core::WriteWavFile(target.string(), large.data(), large.size(),
                                                8000, error);
    const int restored = setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, previousHandler);
    require(restored == 0, "cannot restore the file size limit");
    require(!saved, "a write past the file size limit should fail");

    require(std::filesystem::exists(target) && std::filesystem::file_size(target) == originalSize,
            "a failed save must leave the destination untouched");
    Track track;
    require(track.open(target, TrackType::WaveformPcm16, 8000, error), error);
    require(track.audioSamples() == original, "a failed save changed the destination samples");

    require(xsap::core::WriteWavFile(target.string(), large.data(), large.size(), 8000, error),
            error);
    require(track.reload(error), error);
    require(track.sampleCount() == large.size(), "a successful save must replace the file");

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        require(entry.path().extension() != ".xsaptmp",
                "a staging file was left behind: " + entry.path().string());
    }
}

} // namespace

/*****************************************************************************
 * main
 *
 * Runs every test in a scratch directory that is removed on the way out.
 *
 * A failure throws, so the first one stops the run and its message goes to
 * stderr; use ctest --output-on-failure to see it.
 *
 *****************************************************************************/
int main()
{
    const auto directory =
        std::filesystem::temp_directory_path() / "xsap-model-tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);

    try {
        testRawAndPanel(directory);
        testLockedFrameBeginUsesUpperResult(directory);
        testOpenExpandsOnlyForLongerTrack(directory);
        testWaveAndSpectrum(directory);
        testRawIsDetectedByMagicNotExtension(directory);
        testParameters(directory);
        testFftParity();
        testSpectrumHonoursFrameCount();
        testSpectrogramGeometry();
        testSpectrogramCache();
        testSpectrogramCacheChunking();
        testSpectrogramFrequencyAxis();
        testReloadAllMatchesWinSap(directory);
        testWavSampleFormats(directory);
        testStereoChannels(directory);
        testUnsupportedWavFormats(directory);
        testMarkedAreaHasData(directory);
        testStereoLimitsFrameRange(directory);
        testParameterFileNames();
        testParameterFramesAreUnlimited(directory);
        testSaveKeepsOriginalOnFailure(directory);
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::filesystem::remove_all(directory, error);
        return 1;
    }

    std::filesystem::remove_all(directory, error);
    return 0;
}
