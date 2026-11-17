/************************************************************************
 *
 *  XSap - Shared analysis core: parameter files
 *
 *  FILE:      core/SapParameterFile.h
 *
 *  PURPOSE:   Port of the WinSAP parameter-file handling (WinSAP
 *             src/x64/Panel.cpp: Panel::ParFileTotal,
 *             Panel::BuildParameterIndex and the text extraction half of
 *             Panel::DispID).
 *
 *             WinSAP buffers the whole file, indexes every "FRAME=" line by
 *             frame number and renders the two following lines. The same
 *             matching rules apply here: the tag must be at the start of a
 *             line, it is case sensitive, and frame numbers are parsed with
 *             atol() semantics.
 *
 *  Update:   2026.11 Sparse frame index; the old 40000 frame table limit is lifted
 *
 ************************************************************************/

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace xsap::core {

// FRAME= numbers at or above this are ignored. The index only holds the
// frames present, so this is no memory bound; it keeps frame arithmetic in
// the UI layers (scroll ranges, begin + count) clear of overflow, matching
// WinSAP PARAMETER_MAX_FRAMES.
inline constexpr long kMaxParameterFrames = 1000000000L;

// Byte offset of one "FRAME=" line within the file buffer.
struct ParameterIndexEntry {
    long frame = 0;
    long offset = 0;
};

// Sorted by frame number, one entry per frame present in the file.
using ParameterIndex = std::vector<ParameterIndexEntry>;

// WinSAP ParFileTotal - scans the trailing 1000 bytes for the highest
// "FRAME=" number below kMaxParameterFrames and returns that number + 1.
long ParFileTotal(std::FILE* fp);

// WinSAP BuildParameterIndex - collects the byte offset of every "FRAME="
// line within `buffer`. A repeated frame number keeps its last occurrence,
// as the WinSAP table overwrite did.
void BuildParameterIndex(const char* buffer, ParameterIndex& index);

// Text extraction half of WinSAP DispID: returns the two lines that follow
// the "FRAME=" line of `frameBegin`. Returns false when WinSAP would draw
// nothing (out-of-range frame, or a frame missing from the file).
bool GetParameterLines(const char* buffer,
                       const ParameterIndex& index,
                       long totalFrames,
                       long frameBegin,
                       std::string& line1,
                       std::string& line2);

// Header line ("FRAME=...") of `frameBegin`, using the same rules.
bool GetParameterHeader(const char* buffer,
                        const ParameterIndex& index,
                        long totalFrames,
                        long frameBegin,
                        std::string& header);

} // namespace xsap::core
