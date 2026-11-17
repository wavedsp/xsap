/************************************************************************
 *
 *  XSap - Shared analysis core: spectrogram colour maps
 *
 *  FILE:      core/SapSpectrogramPalette.h
 *
 *  PURPOSE:   The colour maps used to paint the spectrogram. Shared verbatim
 *             with WinSap src/common/SpectrogramPalette.h.
 *
 *             The spectrogram analysis produces one intensity byte per pixel
 *             in the historic SAP polarity: 255 is silence and 0 is full
 *             scale. That polarity is baked into the cache and into the
 *             streaming renderer, and is deliberately left alone here, so a
 *             palette is simply a 256 entry lookup indexed by that byte. Grey
 *             therefore reproduces the original wsap picture exactly, entry
 *             for entry, and every other map is a drop in replacement for it.
 *
 *             Viridis is the default. It is perceptually uniform, so equal
 *             steps in level look like equal steps in brightness and it
 *             produces none of the false ridges that jet shows at its cyan
 *             and yellow transitions; it also survives being printed in grey
 *             and is legible with the common forms of colour blindness. Jet
 *             is kept because a good deal of commercial speech software uses
 *             it and people read those pictures by eye, and inferno because
 *             it suits a dark room.
 *
 *             The maps are defined by anchor colours and interpolated.
 *             Viridis and inferno use the canonical ten step samples of the
 *             matplotlib originals, which are close to linear between
 *             samples, so interpolation costs very little fidelity.
 *
 *  Update:    2026.1
 *
 ************************************************************************/

#ifndef XSAP_CORE_SAPSPECTROGRAMPALETTE_H
#define XSAP_CORE_SAPSPECTROGRAMPALETTE_H

#include <cstdint>

namespace xsap {
namespace core {

enum class SpectrogramPalette {
    Grey = 0,
    Viridis,
    Inferno,
    Jet,
    Count
};

inline constexpr SpectrogramPalette kDefaultSpectrogramPalette = SpectrogramPalette::Viridis;

struct SpectrogramColor {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

inline const char* SpectrogramPaletteName(SpectrogramPalette palette)
{
    switch (palette) {
    case SpectrogramPalette::Grey:
        return "Grey";
    case SpectrogramPalette::Viridis:
        return "Viridis";
    case SpectrogramPalette::Inferno:
        return "Inferno";
    case SpectrogramPalette::Jet:
        return "Jet";
    default:
        return "Grey";
    }
}

inline SpectrogramPalette NextSpectrogramPalette(SpectrogramPalette palette)
{
    const int count = static_cast<int>(SpectrogramPalette::Count);
    int index = static_cast<int>(palette) + 1;
    if (index < 0 || index >= count) {
        index = 0;
    }
    return static_cast<SpectrogramPalette>(index);
}

namespace detail {

struct PaletteAnchor {
    float position; // 0 = silence, 1 = full scale
    std::uint8_t r, g, b;
};

// matplotlib viridis, ten step sampling.
inline constexpr PaletteAnchor kViridis[] = {
    {0.0f / 9.0f, 68, 1, 84},
    {1.0f / 9.0f, 72, 40, 120},
    {2.0f / 9.0f, 62, 73, 137},
    {3.0f / 9.0f, 49, 104, 142},
    {4.0f / 9.0f, 38, 130, 142},
    {5.0f / 9.0f, 31, 158, 137},
    {6.0f / 9.0f, 53, 183, 121},
    {7.0f / 9.0f, 110, 206, 88},
    {8.0f / 9.0f, 181, 222, 43},
    {9.0f / 9.0f, 253, 231, 37},
};

// matplotlib inferno, ten step sampling.
inline constexpr PaletteAnchor kInferno[] = {
    {0.0f / 9.0f, 0, 0, 4},
    {1.0f / 9.0f, 27, 12, 65},
    {2.0f / 9.0f, 74, 12, 107},
    {3.0f / 9.0f, 120, 28, 109},
    {4.0f / 9.0f, 165, 44, 96},
    {5.0f / 9.0f, 207, 68, 70},
    {6.0f / 9.0f, 237, 105, 37},
    {7.0f / 9.0f, 251, 155, 6},
    {8.0f / 9.0f, 247, 209, 61},
    {9.0f / 9.0f, 252, 255, 164},
};

// The classic jet ramp, whose anchors are not evenly spaced.
inline constexpr PaletteAnchor kJet[] = {
    {0.000f, 0, 0, 131},
    {0.125f, 0, 60, 170},
    {0.375f, 5, 255, 255},
    {0.625f, 255, 255, 0},
    {0.875f, 250, 0, 0},
    {1.000f, 128, 0, 0},
};

inline SpectrogramColor Interpolate(const PaletteAnchor* anchors, int count, float position)
{
    if (position <= anchors[0].position) {
        return SpectrogramColor{anchors[0].r, anchors[0].g, anchors[0].b};
    }
    if (position >= anchors[count - 1].position) {
        return SpectrogramColor{anchors[count - 1].r,
                                anchors[count - 1].g,
                                anchors[count - 1].b};
    }

    int upper = 1;
    while (upper < count - 1 && anchors[upper].position < position) {
        upper++;
    }
    const PaletteAnchor& a = anchors[upper - 1];
    const PaletteAnchor& b = anchors[upper];

    const float extent = b.position - a.position;
    const float t = extent > 0.0f ? (position - a.position) / extent : 0.0f;

    const float r = a.r + (b.r - a.r) * t;
    const float g = a.g + (b.g - a.g) * t;
    const float blue = a.b + (b.b - a.b) * t;

    return SpectrogramColor{static_cast<std::uint8_t>(r + 0.5f),
                            static_cast<std::uint8_t>(g + 0.5f),
                            static_cast<std::uint8_t>(blue + 0.5f)};
}

} // namespace detail

/*****************************************************************************
 * BuildSpectrogramPalette
 *
 * Fills 256 colours indexed by the renderer's intensity byte, where index 255
 * is silence and index 0 is full scale.
 *
 *****************************************************************************/
inline void BuildSpectrogramPalette(SpectrogramPalette palette, SpectrogramColor out[256])
{
    if (palette == SpectrogramPalette::Grey) {
        // The original picture: the intensity byte is the grey level itself,
        // so silence is white and full scale is black.
        for (int index = 0; index < 256; index++) {
            const std::uint8_t level = static_cast<std::uint8_t>(index);
            out[index] = SpectrogramColor{level, level, level};
        }
        return;
    }

    const detail::PaletteAnchor* anchors = detail::kViridis;
    int count = static_cast<int>(sizeof(detail::kViridis) / sizeof(detail::kViridis[0]));

    if (palette == SpectrogramPalette::Inferno) {
        anchors = detail::kInferno;
        count = static_cast<int>(sizeof(detail::kInferno) / sizeof(detail::kInferno[0]));
    } else if (palette == SpectrogramPalette::Jet) {
        anchors = detail::kJet;
        count = static_cast<int>(sizeof(detail::kJet) / sizeof(detail::kJet[0]));
    }

    for (int index = 0; index < 256; index++) {
        // Index 255 is silence, which is the dark end of these maps.
        const float position = static_cast<float>(255 - index) / 255.0f;
        out[index] = detail::Interpolate(anchors, count, position);
    }
}

} // namespace core
} // namespace xsap

#endif // XSAP_CORE_SAPSPECTROGRAMPALETTE_H
