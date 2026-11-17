/************************************************************************
 *
 *  XSap - Shared analysis core: WAV / file utilities
 *
 *  FILE:      core/SapWaveFile.h
 *
 *  PURPOSE:   Port of the WinSAP WAV utilities (WinSAP
 *             src/common/FileDialogs.cpp: ReadWavFile, WavBytesPerSample,
 *             WavSampleFormatName, ConvertWavSamples, ReadWavChannel,
 *             FileLength) plus the
 *             RIFF probe WinSAP performs before opening a file
 *             (src/x64/WinsapApp.cpp OpenUpperFile / OpenLowerFile).
 *
 *             The chunk walk, the accepted formats and the returned offsets
 *             match WinSAP exactly, so both programs agree on where the PCM
 *             payload of a given file starts and how long it is.
 *
 ************************************************************************/

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

namespace xsap::core {

// WinSAP WavSampleFormat - the sample encodings of the 'data' chunk that can
// be read and converted to 16-bit PCM.
enum class WavSampleFormat {
    Unsupported,
    Pcm8,    // WAVE_FORMAT_PCM, 8-bit unsigned
    Pcm16,   // WAVE_FORMAT_PCM, 16-bit signed
    Pcm24,   // WAVE_FORMAT_PCM, 24-bit signed
    Pcm32,   // WAVE_FORMAT_PCM, 32-bit signed
    Float32, // WAVE_FORMAT_IEEE_FLOAT, 32-bit
    Float64  // WAVE_FORMAT_IEEE_FLOAT, 64-bit
};

struct WaveInfo {
    long headerLength = 0;     // Byte offset of the 'data' chunk payload
    uint32_t dataLength = 0;   // Size in bytes of the 'data' chunk payload
    uint32_t samplingRate = 0; // Sampling rate from the 'fmt ' chunk
    uint16_t channels = 1;     // Channel count from the 'fmt ' chunk
    WavSampleFormat sampleFormat = WavSampleFormat::Unsupported;
    uint16_t formatTag = 0;     // Format tag, the SubFormat's for WAVE_FORMAT_EXTENSIBLE
    uint16_t bitsPerSample = 0; // Bits per sample from the 'fmt ' chunk
};

// WinSAP ReadWavFile - walks the RIFF chunk list and reports the 'data'
// chunk offset/size together with the 'fmt ' sampling rate, channel count
// and sample format. WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT and
// WAVE_FORMAT_EXTENSIBLE (with either as its SubFormat) are recognised. A WAV
// whose format cannot be read still returns true with sampleFormat set to
// Unsupported, so the caller can name the problem.
bool ReadWavFile(std::FILE* fp, WaveInfo& info);

// WinSAP's RIFF/WAVE probe. Returns true and fills `info` when the file is a
// WAV, false for any other file (which WinSAP then treats as raw data).
bool ProbeWaveFile(std::FILE* fp, WaveInfo& info);

// WinSAP WavBytesPerSample - bytes of one sample of one channel, 0 if the
// format is unsupported.
int WavBytesPerSample(WavSampleFormat sampleFormat);

// WinSAP WavSampleFormatName - "8-bit PCM", "32-bit float", ...
const char* WavSampleFormatName(WavSampleFormat sampleFormat);

// WinSAP ConvertWavSamples - convert little-endian wav samples to 16-bit PCM,
// rounding to the nearest value and saturating at full scale.
void ConvertWavSamples(const uint8_t* source, std::size_t sampleCount,
                       WavSampleFormat sampleFormat, int16_t* destination);

// WinSAP ReadWavChannel - read frameCount frames from the current position
// and keep one channel of them, converted to 16-bit PCM.
bool ReadWavChannel(std::FILE* fp, std::size_t frameCount, int channels, int channel,
                    WavSampleFormat sampleFormat, int16_t* destination);

// WinSAP WriteWavFile - write a block of samples as a mono 16 bit PCM wav
// file using the canonical 44 byte RIFF header. The file is written to a
// sibling staging file and renamed over `path` only once it is complete and
// synced, so a failed write never truncates or removes an existing file.
bool WriteWavFile(const std::string& path, const int16_t* samples,
                  std::size_t sampleCount, uint32_t samplingRate,
                  std::string& error);

// WinSAP FileLength - length in bytes, file position restored to the start.
long FileLength(std::FILE* fp);

// WinSAP PathName - strip the directory portion from a path.
std::string PathName(const std::string& path);

} // namespace xsap::core
