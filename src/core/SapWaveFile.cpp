/************************************************************************
 *
 *  XSap - Shared analysis core: WAV / file utilities
 *
 *  FILE:      core/SapWaveFile.cpp
 *
 *  PURPOSE:   Transcription of WinSAP src/common/FileDialogs.cpp.
 *
 *             WinSAP `fread`s straight into 32/16-bit integers, which is
 *             correct on little-endian x86/x64 only. The reads here are
 *             explicitly little-endian so the result is identical on any
 *             host while still parsing byte-for-byte the same layout.
 *
 ************************************************************************/

#include "core/SapWaveFile.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xsap::core {

namespace {

constexpr uint32_t kRiffTag = 0x46464952; // 'RIFF'
constexpr uint32_t kWaveTag = 0x45564157; // 'WAVE'
constexpr uint32_t kFmtTag = 0x20746D66;  // 'fmt '
constexpr uint32_t kDataTag = 0x61746164; // 'data'

bool ReadU32Le(std::FILE* fp, uint32_t& value)
{
    unsigned char bytes[4];
    if (std::fread(bytes, 1, sizeof(bytes), fp) != sizeof(bytes)) {
        return false;
    }
    value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8U) |
            (static_cast<uint32_t>(bytes[2]) << 16U) |
            (static_cast<uint32_t>(bytes[3]) << 24U);
    return true;
}

void AppendU32Le(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

void AppendU16Le(std::vector<unsigned char>& out, uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void AppendTag(std::vector<unsigned char>& out, const char* tag)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>(tag[i]));
    }
}

} // namespace

/*****************************************************************************
*  ReadWavFile
*
*  Read the wav file header and extract sampling rate
*
*****************************************************************************/
bool ReadWavFile(std::FILE* fp, WaveInfo& info)
{
    uint32_t la = 0;
    bool haveFormat = false;
    WavSampleFormat sampleFormat = WavSampleFormat::Unsupported;
    uint32_t ckid = 0;
    uint32_t cksize = 0;
    uint32_t filesize = 0;
    uint16_t channel = 0;
    uint32_t sampleRate = 0;

    const int seekEnd = std::fseek(fp, 0L, SEEK_END);
    if (seekEnd != 0) {
        return false;
    }
    const long long physicalSize = std::ftell(fp);
    std::fseek(fp, 0L, SEEK_SET);
    if (physicalSize < 12) {
        return false;
    }
    if (!ReadU32Le(fp, la) || la != kRiffTag) { // 'RIFF'
        return false;
    }

    if (!ReadU32Le(fp, filesize)) {
        return false;
    }
    if (!ReadU32Le(fp, la) || la != kWaveTag) { // 'WAVE'
        return false;
    }

    // Chunk positions are 64-bit so that a chunk size near 4 GB cannot wrap the
    // next offset back to an earlier chunk where `long` is 32 bits, and the
    // walk stops at whichever of the declared RIFF end or the physical end of
    // file comes first.
    long long riffEnd = 8 + static_cast<long long>(filesize);
    if (riffEnd > physicalSize) {
        riffEnd = physicalSize;
    }
    long long chunkPos = 12;

    while (chunkPos + 8 <= riffEnd) {
        if (chunkPos > std::numeric_limits<long>::max()) {
            break;
        }
        const int seekResult = std::fseek(fp, static_cast<long>(chunkPos), SEEK_SET);
        if (seekResult != 0) {
            break;
        }
        if (!ReadU32Le(fp, ckid) || !ReadU32Le(fp, cksize)) {
            break;
        }
        const long long chunkData = chunkPos + 8;
        // RIFF chunks are word aligned, so an odd sized chunk carries a pad byte
        const long long nextChunk = chunkData + static_cast<long long>(cksize) +
                                    static_cast<long long>(cksize & 1U);

        switch (ckid) {
        case kFmtTag: { // 'fmt '
            // WAVEFORMATEX, optionally followed by the WAVE_FORMAT_EXTENSIBLE fields
            unsigned char fmt[40] = {};
            const std::size_t fmtLength = cksize < sizeof(fmt) ? cksize : sizeof(fmt);
            if (fmtLength < 16 || std::fread(fmt, 1, fmtLength, fp) != fmtLength) {
                return false;
            }
            uint16_t fmttag = static_cast<uint16_t>(fmt[0] | (fmt[1] << 8U));
            channel = static_cast<uint16_t>(fmt[2] | (fmt[3] << 8U));
            sampleRate = static_cast<uint32_t>(fmt[4]) |
                         (static_cast<uint32_t>(fmt[5]) << 8U) |
                         (static_cast<uint32_t>(fmt[6]) << 16U) |
                         (static_cast<uint32_t>(fmt[7]) << 24U);
            const uint16_t blockAlign = static_cast<uint16_t>(fmt[12] | (fmt[13] << 8U));
            const uint16_t bits = static_cast<uint16_t>(fmt[14] | (fmt[15] << 8U));
            if (fmttag == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE: the real tag leads the SubFormat GUID
                static const unsigned char guidTail[12] = {
                    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38 };
                fmttag = 0;
                if (fmtLength >= 40 && std::memcmp(fmt + 26, guidTail, sizeof(guidTail)) == 0 &&
                    fmt[38] == 0x9B && fmt[39] == 0x71) {
                    fmttag = static_cast<uint16_t>(fmt[24] | (fmt[25] << 8U));
                }
            }
            sampleFormat = WavSampleFormat::Unsupported;
            if (fmttag == 1) { // WAVE_FORMAT_PCM
                if (bits == 8) sampleFormat = WavSampleFormat::Pcm8;
                else if (bits == 16) sampleFormat = WavSampleFormat::Pcm16;
                else if (bits == 24) sampleFormat = WavSampleFormat::Pcm24;
                else if (bits == 32) sampleFormat = WavSampleFormat::Pcm32;
            } else if (fmttag == 3) { // WAVE_FORMAT_IEEE_FLOAT
                if (bits == 32) sampleFormat = WavSampleFormat::Float32;
                else if (bits == 64) sampleFormat = WavSampleFormat::Float64;
            }
            if (channel == 0 || sampleRate == 0 ||
                blockAlign != channel * WavBytesPerSample(sampleFormat)) {
                sampleFormat = WavSampleFormat::Unsupported;
            }
            info.samplingRate = sampleRate;
            info.channels = channel;
            info.formatTag = fmttag;
            info.bitsPerSample = bits;
            haveFormat = true;
            break;
        }

        case kDataTag: // 'data'
            if (!haveFormat) {
                std::fseek(fp, 0L, SEEK_SET);
                return false;
            }
            info.sampleFormat = sampleFormat;
            if (chunkData > std::numeric_limits<long>::max()) {
                std::fseek(fp, 0L, SEEK_SET);
                return false;
            }
            info.headerLength = static_cast<long>(chunkData);
            info.dataLength = cksize;
            std::fseek(fp, 0L, SEEK_SET);
            return true;

        default:
            break;
        }

        chunkPos = nextChunk;
    }

    std::fseek(fp, 0L, SEEK_SET);
    return false;
}

/*****************************************************************************
*  ProbeWaveFile
*
*  WinSAP's pre-open RIFF/WAVE test (WinsapApp::OpenUpperFile)
*
*****************************************************************************/
bool ProbeWaveFile(std::FILE* fp, WaveInfo& info)
{
    uint32_t la = 0;
    uint32_t filesize = 0;

    std::fseek(fp, 0L, SEEK_SET);
    bool isWav = false;

    if (ReadU32Le(fp, la) && la == kRiffTag) {                                // 'RIFF'
        if (ReadU32Le(fp, filesize) && ReadU32Le(fp, la) && la == kWaveTag) { // 'WAVE'
            isWav = ReadWavFile(fp, info);
        }
    }

    std::fseek(fp, 0L, SEEK_SET);
    return isWav;
}

/*****************************************************************************
*  WavBytesPerSample
*
*  Bytes of one sample of one channel
*
*****************************************************************************/
int WavBytesPerSample(WavSampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case WavSampleFormat::Pcm8:    return 1;
    case WavSampleFormat::Pcm16:   return 2;
    case WavSampleFormat::Pcm24:   return 3;
    case WavSampleFormat::Pcm32:   return 4;
    case WavSampleFormat::Float32: return 4;
    case WavSampleFormat::Float64: return 8;
    default:                       return 0;
    }
}

const char* WavSampleFormatName(WavSampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case WavSampleFormat::Pcm8:    return "8-bit PCM";
    case WavSampleFormat::Pcm16:   return "16-bit PCM";
    case WavSampleFormat::Pcm24:   return "24-bit PCM";
    case WavSampleFormat::Pcm32:   return "32-bit PCM";
    case WavSampleFormat::Float32: return "32-bit float";
    case WavSampleFormat::Float64: return "64-bit float";
    default:                       return "unsupported";
    }
}

namespace {

int16_t SaturateSample(long long value)
{
    return static_cast<int16_t>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
}

int16_t FloatToSample(double value)
{
    if (!(value == value)) {
        return 0; // NaN
    }
    const double scaled = value * 32768.0;
    if (scaled >= 32767.0) return 32767;
    if (scaled <= -32768.0) return -32768;
    return static_cast<int16_t>(std::floor(scaled + 0.5));
}

uint32_t LoadU32Le(const uint8_t* s)
{
    return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8U) |
           (static_cast<uint32_t>(s[2]) << 16U) | (static_cast<uint32_t>(s[3]) << 24U);
}

} // namespace

/*****************************************************************************
*  ConvertWavSamples
*
*  Convert little-endian wav samples to 16-bit PCM, rounding to the nearest
*  value and saturating at full scale. The bytes are assembled explicitly so
*  the result does not depend on the host byte order.
*
*****************************************************************************/
void ConvertWavSamples(const uint8_t* source, std::size_t sampleCount,
                       WavSampleFormat sampleFormat, int16_t* destination)
{
    for (std::size_t i = 0; i < sampleCount; ++i) {
        switch (sampleFormat) {
        case WavSampleFormat::Pcm8:
            destination[i] = static_cast<int16_t>((source[i] - 128) * 256);
            break;
        case WavSampleFormat::Pcm16:
            destination[i] = static_cast<int16_t>(
                static_cast<uint16_t>(source[2 * i] | (source[2 * i + 1] << 8U)));
            break;
        case WavSampleFormat::Pcm24: {
            const uint8_t* s = source + 3 * i;
            const long long value =
                static_cast<int32_t>(static_cast<uint32_t>(s[0]) << 8U |
                                     static_cast<uint32_t>(s[1]) << 16U |
                                     static_cast<uint32_t>(s[2]) << 24U) >> 8;
            destination[i] = SaturateSample((value + 128) >> 8);
            break;
        }
        case WavSampleFormat::Pcm32: {
            const int32_t value = static_cast<int32_t>(LoadU32Le(source + 4 * i));
            destination[i] = SaturateSample((static_cast<long long>(value) + 32768) >> 16);
            break;
        }
        case WavSampleFormat::Float32: {
            const uint32_t bits = LoadU32Le(source + 4 * i);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            destination[i] = FloatToSample(value);
            break;
        }
        case WavSampleFormat::Float64: {
            const uint64_t bits = static_cast<uint64_t>(LoadU32Le(source + 8 * i)) |
                                  (static_cast<uint64_t>(LoadU32Le(source + 8 * i + 4)) << 32U);
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            destination[i] = FloatToSample(value);
            break;
        }
        default:
            destination[i] = 0;
            break;
        }
    }
}

/*****************************************************************************
*  ReadWavChannel
*
*  Read frameCount frames and keep one channel, converted to 16-bit PCM
*
*****************************************************************************/
bool ReadWavChannel(std::FILE* fp, std::size_t frameCount, int channels, int channel,
                    WavSampleFormat sampleFormat, int16_t* destination)
{
    const int bytesPerSample = WavBytesPerSample(sampleFormat);
    if (bytesPerSample == 0 || channels < 1 || channel < 0 || channel >= channels) {
        return false;
    }
    const std::size_t frameBytes = static_cast<std::size_t>(bytesPerSample) *
                                   static_cast<std::size_t>(channels);
    const std::size_t chunkFrames = 65536;
    std::vector<uint8_t> buffer(chunkFrames * frameBytes);
    for (std::size_t done = 0; done < frameCount;) {
        const std::size_t count = frameCount - done < chunkFrames ? frameCount - done : chunkFrames;
        if (std::fread(buffer.data(), frameBytes, count, fp) != count) {
            return false;
        }
        const uint8_t* src = buffer.data() + static_cast<std::size_t>(channel) * bytesPerSample;
        if (channels == 1) {
            ConvertWavSamples(src, count, sampleFormat, destination + done);
        } else {
            for (std::size_t i = 0; i < count; i++, src += frameBytes) {
                ConvertWavSamples(src, 1, sampleFormat, destination + done + i);
            }
        }
        done += count;
    }
    return true;
}

/*****************************************************************************
*  WriteWavFile
*
*  Write a block of samples out as a mono 16 bit PCM wav file. The header is
*  assembled little-endian by hand so that it matches WinSAP's writer and is
*  correct on any host.
*
*****************************************************************************/
bool WriteWavFile(const std::string& path, const int16_t* samples,
                  std::size_t sampleCount, uint32_t samplingRate,
                  std::string& error)
{
    if (samples == nullptr || sampleCount == 0) {
        error = "There are no samples to write.";
        return false;
    }
    if (samplingRate == 0) {
        error = "The sampling rate is unknown.";
        return false;
    }
    // A RIFF size field is 32 bits, so refuse anything that would overflow it
    if (sampleCount > (0xFFFFFFFFUL - 36UL) / 2UL) {
        error = "The marked block is too large for a wav file.";
        return false;
    }

    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
    const uint32_t dataLength = static_cast<uint32_t>(sampleCount * 2);

    std::vector<unsigned char> header;
    header.reserve(44);
    AppendTag(header, "RIFF");
    AppendU32Le(header, 36U + dataLength);
    AppendTag(header, "WAVE");
    AppendTag(header, "fmt ");
    AppendU32Le(header, 16U);
    AppendU16Le(header, 1U); // WAVE_FORMAT_PCM
    AppendU16Le(header, channels);
    AppendU32Le(header, samplingRate);
    AppendU32Le(header, samplingRate * blockAlign);
    AppendU16Le(header, blockAlign);
    AppendU16Le(header, bitsPerSample);
    AppendTag(header, "data");
    AppendU32Le(header, dataLength);

    std::vector<unsigned char> payload;
    payload.reserve(dataLength);
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const uint16_t raw = static_cast<uint16_t>(samples[i]);
        payload.push_back(static_cast<unsigned char>(raw & 0xFFU));
        payload.push_back(static_cast<unsigned char>((raw >> 8U) & 0xFFU));
    }

    // Resolve a symbolic link so the rename below replaces the file it names
    // rather than the link itself.
    std::string target = path;
    std::error_code linkError;
    if (std::filesystem::is_symlink(path, linkError)) {
        const std::filesystem::path resolved = std::filesystem::canonical(path, linkError);
        if (!linkError) {
            target = resolved.string();
        }
    }

    // Stage the output beside the destination, on the same file system, so
    // the final rename is atomic.
    std::string staging;
    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
        staging = target + "." + std::to_string(static_cast<long>(::getpid())) + "." +
                  std::to_string(attempt) + ".xsaptmp";
        fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0 && errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        error = "Cannot open " + path + " for writing.";
        return false;
    }

    // Keep the permissions of a file being replaced.
    struct stat existing {};
    const int statResult = ::stat(target.c_str(), &existing);
    if (statResult == 0) {
        ::fchmod(fd, existing.st_mode & 07777);
    }

    std::FILE* fp = ::fdopen(fd, "wb");
    if (fp == nullptr) {
        ::close(fd);
        ::unlink(staging.c_str());
        error = "Cannot open " + path + " for writing.";
        return false;
    }

    const std::size_t headerWritten = std::fwrite(header.data(), 1, header.size(), fp);
    bool ok = headerWritten == header.size();
    if (ok) {
        const std::size_t payloadWritten = std::fwrite(payload.data(), 1, payload.size(), fp);
        ok = payloadWritten == payload.size();
    }
    if (ok) {
        const int flushed = std::fflush(fp);
        ok = flushed == 0;
    }
    if (ok) {
        const int synced = ::fsync(::fileno(fp));
        ok = synced == 0;
    }
    const int closed = std::fclose(fp);
    ok = ok && closed == 0;

    if (!ok) {
        ::unlink(staging.c_str());
        error = "Cannot write " + path + ".";
        return false;
    }

    const int renamed = std::rename(staging.c_str(), target.c_str());
    if (renamed != 0) {
        ::unlink(staging.c_str());
        error = "Cannot replace " + path + ".";
        return false;
    }

    // Make the rename itself durable; the data is already safe if this fails.
    const std::string directory = std::filesystem::path(target).parent_path().string();
    const int dirFd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }

    return true;
}

/*****************************************************************************
*  FileLength
*
*  Get the file length
*
*****************************************************************************/
long FileLength(std::FILE* fp)
{
    long off;

    std::fseek(fp, 0L, SEEK_END);
    off = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    return off;
}

/*****************************************************************************
*  PathName
*
*  Extract the file name from a full path
*
*****************************************************************************/
std::string PathName(const std::string& path)
{
    const std::size_t len = path.size();
    std::size_t i = len;
    while (i > 0) {
        const char ch = path[i - 1];
        if (ch == '\\' || ch == ':' || ch == '/') {
            break;
        }
        --i;
    }
    return path.substr(i);
}

} // namespace xsap::core
