/************************************************************************
 *
 *  XSap - Shared analysis core: parameter files
 *
 *  FILE:      core/SapParameterFile.cpp
 *
 *  PURPOSE:   Transcription of the parameter-file routines from WinSAP
 *             src/x64/Panel.cpp.
 *
 *  Update:   2026.11 Sparse frame index; the old 40000 frame table limit is lifted
 *
 ************************************************************************/

#include "core/SapParameterFile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace xsap::core {

namespace {

// WinSAP DispID copies at most 255 characters per display line.
constexpr std::size_t kMaxLineLength = 255;

std::string CopyLine(const char*& p)
{
    std::string line;
    std::size_t i = 0;
    while (*p != '\0' && *p != '\n' && i < kMaxLineLength) {
        line.push_back(*p++);
        ++i;
    }
    // WinSAP stops copying at 255 characters but still advances to the newline.
    while (*p != '\0' && *p != '\n') {
        ++p;
    }
    if (*p == '\n')
        ++p;
    return line;
}

// Frame number after "FRAME=" with atol() rules, or -1 when it is negative,
// overflows or reaches kMaxParameterFrames.
long ParseFrameNumber(const char* text)
{
    const long frame = std::strtol(text, nullptr, 10);
    return (frame >= 0 && frame < kMaxParameterFrames) ? frame : -1;
}

// Resolve the buffer position WinSAP's DispID would start from, or nullptr
// when WinSAP would return without drawing.
const char* FramePosition(const char* buffer,
                          const ParameterIndex& index,
                          long totalFrames,
                          long frameBegin)
{
    if (buffer == nullptr)
        return nullptr;

    // Check frame bounds
    if (frameBegin < 0 || frameBegin >= totalFrames)
        return nullptr;

    const auto entry = std::lower_bound(
        index.begin(), index.end(), frameBegin,
        [](const ParameterIndexEntry& item, long frame) { return item.frame < frame; });
    if (entry == index.end() || entry->frame != frameBegin)
        return nullptr; // Frame missing from this file

    return buffer + entry->offset;
}

} // namespace

/*****************************************************************************
 * ParFileTotal
 *
 * Calculate total number of frames in a parameter file
 * Reads last 1000 bytes and searches for highest FRAME= number
 *
 *****************************************************************************/
long ParFileTotal(std::FILE* fp)
{
    char buf[1001];
    char* p;
    long fileLen, seekPos;
    long maxFrame = -1;

    // Get file length
    std::fseek(fp, 0L, SEEK_END);
    fileLen = std::ftell(fp);

    // Seek to last 1000 bytes (or start of file if shorter)
    seekPos = (fileLen > 1000) ? (fileLen - 1000) : 0;
    std::fseek(fp, seekPos, SEEK_SET);

    // Read the end section
    std::size_t bytesRead = std::fread(buf, sizeof(char), 1000, fp);
    buf[bytesRead] = '\0';

    // Search through buffer for FRAME= lines
    p = buf;
    while (*p != '\0') {
        if (std::strncmp(p, "FRAME=", 6) == 0) {
            maxFrame = (std::max)(maxFrame, ParseFrameNumber(p + 6));
        }
        // Move to next line
        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    // Frame numbers are 0-based; a file without a usable tag in its tail still
    // counts frame 0, as WinSAP always did.
    return (std::max)(maxFrame, 0L) + 1;
}

/*****************************************************************************
 * BuildParameterIndex
 *
 * Build index mapping frame numbers to file byte positions
 * Parses entire parameter file buffer; memory follows the FRAME= lines
 * present, not the highest frame number.
 *
 *****************************************************************************/
void BuildParameterIndex(const char* buffer, ParameterIndex& index)
{
    index.clear();
    if (buffer == nullptr)
        return;

    const char* p = buffer;
    long pos = 0;

    // Parse through entire buffer
    while (*p != '\0') {
        // Check if line starts with "FRAME="
        if (std::strncmp(p, "FRAME=", 6) == 0) {
            const long frameNum = ParseFrameNumber(p + 6);
            if (frameNum >= 0) {
                index.push_back(ParameterIndexEntry{frameNum, pos});
            }
        }

        // Move to next line
        while (*p != '\0' && *p != '\n') {
            p++;
            pos++;
        }
        if (*p == '\n') {
            p++;
            pos++;
        }
    }

    // Files are normally written in frame order, so sort only when needed.
    // The stable sort keeps repeated frames in file order for the pass below.
    const auto byFrame = [](const ParameterIndexEntry& a, const ParameterIndexEntry& b) {
        return a.frame < b.frame;
    };
    if (!std::is_sorted(index.begin(), index.end(), byFrame)) {
        std::stable_sort(index.begin(), index.end(), byFrame);
    }

    // Keep the last occurrence of each repeated frame number
    std::size_t kept = 0;
    for (std::size_t i = 0; i < index.size(); ++i) {
        if (i + 1 < index.size() && index[i + 1].frame == index[i].frame) {
            continue;
        }
        index[kept++] = index[i];
    }
    index.resize(kept);
}

/*****************************************************************************
 * GetParameterLines
 *
 * Text extraction half of WinSAP DispID
 *
 *****************************************************************************/
bool GetParameterLines(const char* buffer,
                       const ParameterIndex& index,
                       long totalFrames,
                       long frameBegin,
                       std::string& line1,
                       std::string& line2)
{
    line1.clear();
    line2.clear();

    const char* p = FramePosition(buffer, index, totalFrames, frameBegin);
    if (p == nullptr)
        return false;

    // Skip the FRAME= line
    while (*p != '\0' && *p != '\n')
        p++;
    if (*p == '\n')
        p++;

    // Extract first data line, then the second data line
    line1 = CopyLine(p);
    line2 = CopyLine(p);
    return true;
}

/*****************************************************************************
 * GetParameterHeader
 *
 *****************************************************************************/
bool GetParameterHeader(const char* buffer,
                        const ParameterIndex& index,
                        long totalFrames,
                        long frameBegin,
                        std::string& header)
{
    header.clear();

    const char* p = FramePosition(buffer, index, totalFrames, frameBegin);
    if (p == nullptr)
        return false;

    header = CopyLine(p);
    return true;
}

} // namespace xsap::core
