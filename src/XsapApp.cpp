/************************************************************************
 *
 *  XSap - Main Application Implementation
 *
 *  FILE:      XsapApp.cpp
 *
 *  PURPOSE:   The whole user interface: main window, menus, toolbar, status
 *             bar, dialogs, and the drawing of every display type.
 *
 *             This is the one file with no WinSap counterpart to transcribe,
 *             because it replaces GDI with Cairo and Win32 with GTKMM. The
 *             layout, the keyboard bindings and the display behaviour still
 *             follow WinSap src/x64/WinsapApp.cpp and Panel.cpp, so the two
 *             programs feel the same and show the same picture.
 *
 *  Update:    2026.1
 *             2026.11 Parameter mode and the *.par filter select each other
 *                     and every .par file opens as parameters; a raw sample
 *                     rate change discards the cached spectrogram images
 *
 ************************************************************************/

#include "XsapApp.h"

#include "AudioPlayer.h"
#include "HelpWindow.h"
#include "Model.h"
#include "core/SapSpectrogramPalette.h"
#include "core/SapWaveFile.h"

#include <gtkmm.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/general.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace xsap {

namespace {

// The version is set by CMake from project(VERSION ...) so that the build
// system and the program can never disagree. The fallback only applies to a
// build that does not go through CMake, and must be kept in step by hand.
#ifndef XSAP_VERSION
#define XSAP_VERSION "3.1.0"
#endif

constexpr char kVersion[] = XSAP_VERSION;
constexpr unsigned int kDefaultSampleRate = 16000;
constexpr int kDefaultWidth = 1200;
constexpr int kDefaultHeight = 800;
constexpr int kMinimumWidth = 760;
constexpr int kMinimumHeight = 540;
constexpr int kPanelGap = 1;
constexpr int kToolbarIconSize = 32;
constexpr int kToolbarHeight = 48;
// The application icon is 48x48, so the About dialog draws it at its native
// size. GTK4 sizes a Gtk::Image from its pixel-size, not from the resolution of
// the pixbuf, so this has to be set explicitly or the logo comes out tiny.
constexpr int kAboutIconSize = 48;

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    /*****************************************************************************
     * contains
     *
     * Reports whether a point falls inside a layout rectangle. The half-open
     * right and bottom edges keep adjacent hit regions from overlapping.
     *
     *****************************************************************************/
    bool contains(double px, double py) const noexcept
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

/*****************************************************************************
 * trimCopy
 *
 * Returns a copy without leading or trailing white space. Dialog parsers
 * use it so typed values can be forgiving without changing stored state.
 *
 *****************************************************************************/
std::string trimCopy(const std::string& value)
{
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

/*****************************************************************************
 * pathText
 *
 * Converts a filesystem path to the UTF-8 text used throughout the GTK
 * interface.
 *
 *****************************************************************************/
std::string pathText(const std::filesystem::path& path)
{
    return path.u8string();
}

/*****************************************************************************
 * fileNameText
 *
 * Chooses the leaf name for compact UI labels, falling back to the whole
 * path when a path has no filename component.
 *
 *****************************************************************************/
std::string fileNameText(const std::filesystem::path& path)
{
    const std::string name = path.filename().u8string();
    return name.empty() ? pathText(path) : name;
}

/*****************************************************************************
 * lowerExtension
 *
 * Normalises a path extension for file-type checks. Case folding keeps
 * chooser logic independent of the spelling returned by the filesystem.
 *
 *****************************************************************************/
std::string lowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().u8string();
    /*****************************************************************************
     * transform
     *
     * Handles the transform step of the Xsap GTK interface. Keeping the logic
     * in one helper lets the menus, drawing code and state updates stay
     * consistent.
     *
     *****************************************************************************/
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

/*****************************************************************************
 * parseSize
 *
 * Parses an unsigned size from dialog text and returns a user-facing error
 * instead of throwing. The explicit range check keeps oversized input from
 * wrapping.
 *
 *****************************************************************************/
std::optional<std::size_t> parseSize(const std::string& text, std::string& error)
{
    const std::string value = trimCopy(text);
    if (value.empty()) {
        error = "A value is required.";
        return std::nullopt;
    }
    if (value.front() == '-') {
        error = "Enter a non-negative integer.";
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        error = "Enter a non-negative integer.";
        return std::nullopt;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        error = "The value is too large.";
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

/*****************************************************************************
 * parseRate
 *
 * Parses a strictly positive sample rate from dialog text. Raw PCM playback
 * and display need a real rate even when the file carries no metadata.
 *
 *****************************************************************************/
std::optional<unsigned int> parseRate(const std::string& text, std::string& error)
{
    const auto value = parseSize(text, error);
    if (!value.has_value() || *value == 0U) {
        if (error.empty()) {
            error = "The sample rate must be greater than zero.";
        }
        return std::nullopt;
    }
    if (*value > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        error = "The sample rate is too large.";
        return std::nullopt;
    }
    return static_cast<unsigned int>(*value);
}

/*****************************************************************************
 * parseDouble
 *
 * Parses a finite floating point value for amplitude limits. Rejecting
 * infinities and trailing text keeps the panel scaling predictable.
 *
 *****************************************************************************/
std::optional<double> parseDouble(const std::string& text, std::string& error)
{
    const std::string value = trimCopy(text);
    if (value.empty()) {
        error = "A value is required.";
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        error = "Enter a finite number.";
        return std::nullopt;
    }
    return parsed;
}

/*****************************************************************************
 * parseLimits
 *
 * Parses the lower and upper amplitude limits entered in one field. The
 * ordering check protects later drawing code from a zero or inverted range.
 *
 *****************************************************************************/
std::optional<Limits> parseLimits(const std::string& text, std::string& error)
{
    std::istringstream stream(text);
    std::string lower;
    std::string upper;
    std::string extra;
    if (!(stream >> lower >> upper) || (stream >> extra)) {
        error = "Enter lower and upper limits separated by whitespace.";
        return std::nullopt;
    }
    std::string lowerError;
    const auto lowerValue = parseDouble(lower, lowerError);
    if (!lowerValue.has_value()) {
        error = lowerError;
        return std::nullopt;
    }
    std::string upperError;
    const auto upperValue = parseDouble(upper, upperError);
    if (!upperValue.has_value()) {
        error = upperError;
        return std::nullopt;
    }
    if (*lowerValue == *upperValue) {
        error = "The limits must be different.";
        return std::nullopt;
    }
    return Limits{std::min(*lowerValue, *upperValue), std::max(*lowerValue, *upperValue)};
}

/*****************************************************************************
 * trackTypeText
 *
 * Returns the short track labels shown in lists and status text. The
 * compact names fit the space-constrained WinSap-style interface.
 *
 *****************************************************************************/
std::string trackTypeText(TrackType type)
{
    switch (type) {
    case TrackType::WaveformPcm16:
        return "WAVE";
    case TrackType::ParameterText:
        return "PARAM";
    case TrackType::FftAmplitude:
        return "FFT";
    }
    return "TRACK";
}

/*****************************************************************************
 * formatTime
 *
 * Formats a sample count as seconds for playback progress. A zero rate is
 * treated as silence rather than allowing division by zero.
 *
 *****************************************************************************/
std::string formatTime(std::size_t samples, unsigned int sampleRate)
{
    if (sampleRate == 0U) {
        return "0.00 s";
    }
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << (static_cast<double>(samples) / static_cast<double>(sampleRate)) << " s";
    return stream.str();
}

/*****************************************************************************
 * formatPanelTime
 *
 * Formats a sample index as the fixed time stamp shown in the frame header.
 * Raw files fall back to the historical 8 kHz panel convention.
 *
 *****************************************************************************/
std::string formatPanelTime(int64_t samples, unsigned int sampleRate)
{
    if (sampleRate == 0U) {
        sampleRate = 8000U;
    }
    const int64_t milliseconds = samples * 1000 / static_cast<int64_t>(sampleRate);
    const int64_t hours = milliseconds / 3600000;
    const int64_t minutes = (milliseconds / 60000) % 60;
    const int64_t seconds = (milliseconds / 1000) % 60;
    const int64_t remainder = milliseconds % 1000;
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes
           << ':' << std::setw(2) << seconds << '.' << std::setw(3) << remainder;
    return stream.str();
}

/*****************************************************************************
 * setSource
 *
 * Converts the small HTML-style colour strings used by the UI into Cairo
 * source colours. Bad input deliberately falls back to white.
 *
 *****************************************************************************/
void setSource(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& color)
{
    if (color.size() == 7U && color.front() == '#') {
        const auto red = static_cast<double>(std::strtoul(color.substr(1, 2).c_str(), nullptr, 16)) / 255.0;
        const auto green = static_cast<double>(std::strtoul(color.substr(3, 2).c_str(), nullptr, 16)) / 255.0;
        const auto blue = static_cast<double>(std::strtoul(color.substr(5, 2).c_str(), nullptr, 16)) / 255.0;
        cr->set_source_rgb(red, green, blue);
    } else {
        cr->set_source_rgb(1.0, 1.0, 1.0);
    }
}

/*****************************************************************************
 * drawText
 *
 * Draws monospace text at an exact Cairo position. The display imitates
 * WinSap fixed-grid readouts, so labels are placed manually.
 *
 *****************************************************************************/
void drawText(const Cairo::RefPtr<Cairo::Context>& cr,
              double x,
              double y,
              const std::string& text,
              double size,
              const std::string& color)
{
    if (text.empty()) {
        return;
    }
    setSource(cr, color);
    cr->select_font_face("Monospace",
                         Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(size);
    cr->move_to(x, y);
    cr->show_text(text);
}

/*****************************************************************************
 * strokeRect
 *
 * Strokes a rectangle inside its bounds. Offsetting by half the line width
 * keeps one-pixel borders crisp and avoids clipping.
 *
 *****************************************************************************/
void strokeRect(const Cairo::RefPtr<Cairo::Context>& cr,
                const Rect& rect,
                const std::string& color,
                double lineWidth = 1.0)
{
    setSource(cr, color);
    cr->set_line_width(lineWidth);
    cr->rectangle(rect.x + lineWidth / 2.0,
                  rect.y + lineWidth / 2.0,
                  std::max(0.0, rect.width - lineWidth),
                  std::max(0.0, rect.height - lineWidth));
    cr->stroke();
}

class PlotArea final : public Gtk::DrawingArea {
public:
    using DrawFunction =
        std::function<void(const Cairo::RefPtr<Cairo::Context>&, int, int)>;
    using ClickFunction = std::function<void(double, double, unsigned int)>;
    using MotionFunction = std::function<void(double, double)>;
    using KeyFunction = std::function<bool(guint, guint, Gdk::ModifierType)>;

    /*****************************************************************************
     * PlotArea
     *
     * Creates the drawing surface and wires GTK controllers to simple
     * callbacks. Keeping this wrapper small isolates GTK event plumbing from
     * app state.
     *
     *****************************************************************************/
    PlotArea()
    {
        set_content_width(720);
        set_content_height(520);
        set_hexpand(true);
        set_vexpand(true);
        set_focusable(true);

        click_ = Gtk::GestureClick::create();
        click_->set_button(0);
        click_->signal_pressed().connect([this](int, double x, double y) {
            if (clickFunction_) {
                clickFunction_(x, y, click_->get_current_button());
            }
        });
        add_controller(click_);

        motion_ = Gtk::EventControllerMotion::create();
        motion_->signal_motion().connect([this](double x, double y) {
            if (motionFunction_) {
                motionFunction_(x, y);
            }
        });
        add_controller(motion_);

        key_ = Gtk::EventControllerKey::create();
        key_->signal_key_pressed().connect(
            sigc::slot<bool(guint, guint, Gdk::ModifierType)>(
                [this](guint keyval, guint keycode, Gdk::ModifierType state) {
                    return keyFunction_ ? keyFunction_(keyval, keycode, state) : false;
                }),
            false);
        add_controller(key_);
    }

    /*****************************************************************************
     * set_callbacks
     *
     * Installs the drawing, mouse and keyboard callbacks for the plot area. The
     * draw callback is registered last so it always sees the current handlers.
     *
     *****************************************************************************/
    void set_callbacks(DrawFunction draw,
                       ClickFunction click,
                       MotionFunction motion,
                       KeyFunction key)
    {
        drawFunction_ = std::move(draw);
        clickFunction_ = std::move(click);
        motionFunction_ = std::move(motion);
        keyFunction_ = std::move(key);
        set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
            if (drawFunction_) {
                drawFunction_(cr, width, height);
            }
        });
    }

private:
    Glib::RefPtr<Gtk::GestureClick> click_;
    Glib::RefPtr<Gtk::EventControllerMotion> motion_;
    Glib::RefPtr<Gtk::EventControllerKey> key_;
    DrawFunction drawFunction_;
    ClickFunction clickFunction_;
    MotionFunction motionFunction_;
    KeyFunction keyFunction_;
};

class AdaptiveIcon final : public Gtk::DrawingArea {
public:
    /*****************************************************************************
     * AdaptiveIcon
     *
     * Stores normal and high-DPI pixbufs and draws the best one for the current
     * scale factor. Cairo scaling keeps toolbar icons centred in their logical
     * size.
     *
     *****************************************************************************/
    AdaptiveIcon(Glib::RefPtr<Gdk::Pixbuf> normal,
                 Glib::RefPtr<Gdk::Pixbuf> highDpi,
                 int logicalSize)
        : normal_(std::move(normal)),
          highDpi_(std::move(highDpi)),
          logicalSize_(logicalSize)
    {
        set_content_width(logicalSize_);
        set_content_height(logicalSize_);
        set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
            const auto& pixbuf = get_scale_factor() > 1 && highDpi_ ? highDpi_ : normal_;
            if (!pixbuf || width <= 0 || height <= 0) {
                return;
            }
            const double scale = std::min(
                static_cast<double>(width) / static_cast<double>(pixbuf->get_width()),
                static_cast<double>(height) / static_cast<double>(pixbuf->get_height()));
            const double drawnWidth = static_cast<double>(pixbuf->get_width()) * scale;
            const double drawnHeight = static_cast<double>(pixbuf->get_height()) * scale;
            cr->save();
            cr->translate((static_cast<double>(width) - drawnWidth) / 2.0,
                          (static_cast<double>(height) - drawnHeight) / 2.0);
            cr->scale(scale, scale);
            Gdk::Cairo::set_source_pixbuf(cr, pixbuf, 0.0, 0.0);
            cr->paint();
            cr->restore();
        });
    }

private:
    Glib::RefPtr<Gdk::Pixbuf> normal_;
    Glib::RefPtr<Gdk::Pixbuf> highDpi_;
    int logicalSize_ = 16;
};

/*****************************************************************************
 * destroyDialogLater
 *
 * Hides a transient dialog and deletes it from the idle loop. Deferring
 * destruction lets GTK finish processing the response signal safely.
 *
 *****************************************************************************/
void destroyDialogLater(Gtk::Window* dialog)
{
    dialog->set_visible(false);
    Glib::signal_idle().connect_once([dialog]() { delete dialog; });
}

} // namespace

struct XsapApp::Impl {
    struct Selection {
        bool valid = false;
        bool upper = true;
        std::size_t slot = 0;
    };

    struct PanelLayout {
        Rect panel;
        Rect frameInfo;
        std::array<Rect, Panel::kMaxTracks> labels{};
        Rect parameters;
        Rect plot;
    };

    struct PlaybackSegment {
        bool upper = true;
        int64_t beginSample = 0;
        std::size_t sourceSampleCount = 0;
        std::size_t combinedOffset = 0;
        // A stereo file plays both channels, so the cursor runs in both panels.
        bool stereo = false;
    };

    struct PlaybackJob {
        bool active = false;
        unsigned int sampleRate = 0;
        std::vector<PlaybackSegment> segments;
        std::string description;
    };

    struct QueuedPlayback {
        std::vector<int16_t> samples;
        unsigned int sampleRate = 0;
        std::vector<PlaybackSegment> segments;
        std::string description;
        // Right channel of a stereo item; empty for mono.
        std::vector<int16_t> rightSamples;
    };

    // The part of a panel's state that stereo files keep equal in both panels.
    struct StereoView {
        std::size_t frameBegin = 0;
        std::size_t visibleFrameCount = 0;
        std::size_t frameLength = 0;
        std::size_t frameSkip = 0;
        double lowerLimit = 0.0;
        double upperLimit = 0.0;
        std::optional<SampleRange> mark;
        std::optional<int64_t> pendingMark;

        bool operator==(const StereoView& other) const noexcept
        {
            return frameBegin == other.frameBegin &&
                   visibleFrameCount == other.visibleFrameCount &&
                   frameLength == other.frameLength && frameSkip == other.frameSkip &&
                   lowerLimit == other.lowerLimit && upperLimit == other.upperLimit &&
                   mark.has_value() == other.mark.has_value() &&
                   (!mark.has_value() ||
                    (mark->begin == other.mark->begin && mark->end == other.mark->end)) &&
                   pendingMark == other.pendingMark;
        }
    };

    struct TrackRef {
        std::size_t slot = 0;
        const Track* track = nullptr;
    };

    struct CloseEntry {
        bool upper = true;
        std::size_t slot = 0;
        Gtk::CheckButton* check = nullptr;
    };

    struct MenuEntry {
        const char* label;
        const char* action;
        const char* iconFile;
        const char* fallbackIcon;
        const char* shortcut;
        bool check;
    };

    struct MenuHotkey {
        guint keyval = 0;
        std::string action;
        bool check = false;
    };

    struct MenuRecord {
        Gtk::MenuButton* button = nullptr;
        Gtk::Popover* popover = nullptr;
        Gtk::Widget* firstItem = nullptr;
        std::vector<MenuHotkey> hotkeys;
    };

    std::unique_ptr<Gtk::ApplicationWindow> window_;
    std::unique_ptr<Gtk::Box> root_;
    std::unique_ptr<Gtk::Box> toolbar_;
    std::unique_ptr<Gtk::Box> statusBar_;
    std::unique_ptr<Gtk::Label> statusLabel_;
    std::unique_ptr<Gtk::Label> fileLabel_;
    std::unique_ptr<Gtk::Label> cursorLabel_;
    std::unique_ptr<Gtk::Label> playbackLabel_;
    std::unique_ptr<Gtk::Label> wavRateLabel_;
    std::unique_ptr<Gtk::Label> infoLabel_;
    std::unique_ptr<PlotArea> plotArea_;
    // Right-click menu of a file name, parented to the plot area.
    std::unique_ptr<Gtk::Popover> fileMenu_;
    Glib::RefPtr<Gtk::Application> application_;
    Glib::RefPtr<Gtk::EventControllerKey> windowKeyController_;
    Gtk::Box* menuBar_ = nullptr;
    std::vector<MenuRecord> menus_;
    Glib::Dispatcher playbackDispatcher_;
    sigc::connection closeConnection_;

    Panel upperPanel_{true};
    Panel lowerPanel_{false};
    Selection selection_;
    PanelLayout upperLayout_;
    PanelLayout lowerLayout_;
    std::optional<int64_t> pendingUpperMark_;
    std::optional<int64_t> pendingLowerMark_;
    bool activeUpper_ = true;
    bool overlayLower_ = false;
    bool lockStarts_ = true;
    // Last views the stereo sync saw, used to tell which panel was changed.
    StereoView lastUpperView_;
    StereoView lastLowerView_;
    bool syncingStereo_ = false;
    unsigned int rawSampleRate_ = kDefaultSampleRate;
    bool warnParameterExtensionMismatch_ = true;
    bool warnBinaryExtensionMismatch_ = true;

    // Window geometry restored from, and written back to, the settings file.
    int settingsWidth_ = kDefaultWidth;
    int settingsHeight_ = kDefaultHeight;
    bool settingsMaximized_ = false;

    // Cached Cairo surfaces for the spectrogram, so a repaint that changes
    // nothing does not rebuild the image. Keyed on the track and the analysis
    // parameters; one entry per track that is currently showing one.
    struct SpectrogramSurface {
        const Track* owner = nullptr;
        std::size_t firstSample = 0;
        std::size_t span = 0;
        int width = 0;
        int height = 0;
        int columns = 0;
        bool fromCache = false;
        float rangeDb = 0.0f;
        xsap::core::SpectrogramPalette palette = xsap::core::kDefaultSpectrogramPalette;
        std::uint64_t generation = 0;
        Cairo::RefPtr<Cairo::ImageSurface> surface;
    };
    mutable std::vector<SpectrogramSurface> spectrogramSurfaces_;
    // Grey ramp span, in dB below the file peak. 60 dB shows the noise floor
    // without washing the picture out; 40 dB drops real detail.
    float spectrogramRangeDb_ = xsap::core::kDefaultDynamicRangeDb;
    // The spectrogram colour map. Viridis by default: perceptually uniform, so
    // it shows none of the false ridges jet produces, and legible to the common
    // forms of colour blindness.
    xsap::core::SpectrogramPalette spectrogramPalette_ = xsap::core::kDefaultSpectrogramPalette;
    // Set during a repaint that fell back to per view analysis because the
    // background whole file analysis had not finished.
    mutable bool spectrogramBuildPending_ = false;
    sigc::connection spectrogramPoll_;
    std::string statusText_;
    std::string coordinateText_;
    std::string hoverFileText_;
    std::filesystem::path executablePath_;

    AudioPlayer audioPlayer_;
    PlaybackJob playbackJob_;
    std::vector<QueuedPlayback> playbackQueue_;
    std::size_t playbackQueueIndex_ = 0;
    std::atomic<std::size_t> playbackSamples_{0};
    std::atomic<int> playbackNotice_{0};
    int handledPlaybackNotice_ = 0;
    std::size_t handledPlaybackSamples_ = 0;
    bool playbackStoppedByUser_ = false;
    std::string playbackCommandFeedback_;
    bool argumentError_ = false;

    /*****************************************************************************
     * Impl
     *
     * Connects the playback dispatcher used to marshal audio-thread progress
     * back to GTK main thread.
     *
     *****************************************************************************/
    Impl()
    {
        playbackDispatcher_.connect(sigc::mem_fun(*this, &Impl::onPlaybackDispatch));
    }

    /*****************************************************************************
     * ~Impl
     *
     * Stops playback and disconnects the close handler before GTK widgets are
     * destroyed. That prevents late callbacks from touching torn-down state.
     *
     *****************************************************************************/
    ~Impl()
    {
        stopPlayback(false);
        if (fileMenu_) {
            fileMenu_->unparent();
        }
        if (closeConnection_.connected()) {
            closeConnection_.disconnect();
        }
    }

    /*****************************************************************************
     * run
     *
     * Parses startup arguments, creates the GTK application and opens any
     * initial waveforms. File-open requests are forwarded by GApplication
     * when an existing instance owns the application ID.
     *
     *****************************************************************************/
    int run(int argc, char** argv)
    {
        std::vector<std::filesystem::path> startup;
        if (!parseArguments(argc, argv, startup)) {
            return argumentError_ ? 1 : 0;
        }
        executablePath_ = argc > 0 && argv != nullptr && argv[0] != nullptr
                              ? std::filesystem::absolute(argv[0])
                              : std::filesystem::current_path() / "xsap";

        if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr &&
            std::getenv("BROADWAY_DISPLAY") == nullptr) {
            std::cerr << "xsap: unable to open a GTK display (set DISPLAY or WAYLAND_DISPLAY).\n";
            return 1;
        }

        application_ = Gtk::Application::create("org.tianwang.xsap",
                                               Gio::Application::Flags::HANDLES_OPEN);
        application_->signal_activate().connect([this]() {
            if (!window_ || !window_->get_visible()) {
                loadSettings();
                buildWindow();
                if (settingsMaximized_ || std::getenv("WSL_DISTRO_NAME") != nullptr ||
                    std::getenv("WSL_INTEROP") != nullptr) {
                    window_->maximize();
                }
            }
            window_->present();
        });
        application_->signal_open().connect(
            [this](const Gio::Application::type_vec_files& files, const Glib::ustring&) {
                application_->activate();
                std::vector<std::filesystem::path> paths;
                for (const auto& file : files) {
                    paths.emplace_back(file->get_path());
                }
                stopPlayback(false);
                setStatus("Opening requested waveforms.");
                openStartupWaveforms(paths);
            });
        std::vector<std::string> arguments{
            argv != nullptr && argc > 0 ? argv[0] : "xsap"};
        if (!startup.empty()) {
            arguments.emplace_back("--");
        }
        for (const auto& path : startup) {
            arguments.push_back(std::filesystem::absolute(path).string());
        }
        std::vector<char*> appArgv;
        for (auto& argument : arguments) {
            appArgv.push_back(argument.data());
        }
        appArgv.push_back(nullptr);
        return application_->run(static_cast<int>(arguments.size()), appArgv.data());
    }

private:
    /*****************************************************************************
     * parseArguments
     *
     * Handles xsap command line options before GTK starts. Non-option arguments
     * are kept as startup waveforms and errors are reported on stderr.
     *
     *****************************************************************************/
    bool parseArguments(int argc,
                        char** argv,
                        std::vector<std::filesystem::path>& startup)
    {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index] != nullptr ? argv[index] : "";
            if (argument == "--help") {
                std::cout << "Usage: xsap [--help] [--version] [waveform ...]\n"
                          << "Opens each non-option argument as an upper/lower waveform track.\n";
                return false;
            }
            if (argument == "--version") {
                std::cout << kVersion << '\n';
                return false;
            }
            if (argument == "--") {
                for (++index; index < argc; ++index) {
                    startup.emplace_back(argv[index] != nullptr ? argv[index] : "");
                }
                break;
            }
            if (!argument.empty() && argument.front() == '-') {
                std::cerr << "xsap: unknown option '" << argument << "'\n"
                          << "Usage: xsap [--help] [--version] [waveform ...]\n";
                argumentError_ = true;
                return false;
            }
            startup.emplace_back(argument);
        }
        return true;
    }

    /*****************************************************************************
     * settingsPath
     *
     * Returns the key file that holds the saved window geometry and the raw
     * PCM sampling rate. WinSap keeps the same values under the registry key
     * HKCU\Software\WangTian\WinSap; below the XDG configuration directory is
     * where Ubuntu expects them.
     *
     *****************************************************************************/
    static std::filesystem::path settingsPath()
    {
        return std::filesystem::path(Glib::get_user_config_dir()) / "xsap" / "xsap.conf";
    }

    /*****************************************************************************
     * loadSettings
     *
     * Reads the saved status back before the window is built. Anything missing
     * or unreadable simply leaves the compiled in defaults alone, so a first
     * run and a damaged file behave the same way.
     *
     *****************************************************************************/
    void loadSettings()
    {
        auto keyFile = Glib::KeyFile::create();
        try {
            if (!keyFile->load_from_file(settingsPath().string())) {
                return;
            }
        } catch (const Glib::Error&) {
            return;
        }

        try {
            const int width = keyFile->get_integer("Window", "Width");
            const int height = keyFile->get_integer("Window", "Height");
            if (width > 0 && height > 0) {
                settingsWidth_ = std::max(width, kMinimumWidth);
                settingsHeight_ = std::max(height, kMinimumHeight);
            }
        } catch (const Glib::Error&) {
        }

        try {
            settingsMaximized_ = keyFile->get_boolean("Window", "Maximized");
        } catch (const Glib::Error&) {
        }

        try {
            const int rate = keyFile->get_integer("Audio", "Rate");
            if (rate > 0) {
                rawSampleRate_ = static_cast<unsigned int>(rate);
            }
        } catch (const Glib::Error&) {
        }

        try {
            warnParameterExtensionMismatch_ =
                keyFile->get_boolean("Warnings", "ParameterExtensionMismatch");
        } catch (const Glib::Error&) {
        }

        try {
            warnBinaryExtensionMismatch_ =
                keyFile->get_boolean("Warnings", "BinaryExtensionMismatch");
        } catch (const Glib::Error&) {
        }

        try {
            const int palette = keyFile->get_integer("Spectrogram", "Palette");
            if (palette >= 0 &&
                palette < static_cast<int>(xsap::core::SpectrogramPalette::Count)) {
                spectrogramPalette_ =
                    static_cast<xsap::core::SpectrogramPalette>(palette);
            }
        } catch (const Glib::Error&) {
        }

        try {
            const double rangeDb = keyFile->get_double("Spectrogram", "RangeDb");
            if (std::isfinite(rangeDb) && rangeDb >= 20.0 && rangeDb <= 120.0) {
                spectrogramRangeDb_ = static_cast<float>(rangeDb);
            }
        } catch (const Glib::Error&) {
        }
    }

    /*****************************************************************************
     * saveSettings
     *
     * Merges the requested groups into the settings file and leaves the rest of
     * it alone. The geometry is written automatically when the program quits,
     * while the raw PCM sampling rate is only written when the user asks for it
     * from the Options menu. Spectrogram display changes are written
     * immediately. GTK4 owns the window position, so unlike WinSap only the
     * size can be restored.
     *
     *****************************************************************************/
    bool saveSettings(std::string& error,
                      bool includeGeometry,
                      bool includeRate,
                      bool includeWarnings,
                      bool includeSpectrogram)
    {
        auto keyFile = Glib::KeyFile::create();
        const std::filesystem::path path = settingsPath();

        // Start from what is already on disk so writing one group does not
        // throw the other one away.
        try {
            keyFile->load_from_file(path.string());
        } catch (const Glib::Error&) {
        }

        if (includeGeometry) {
            int width = settingsWidth_;
            int height = settingsHeight_;
            bool maximized = settingsMaximized_;
            if (window_) {
                maximized = window_->is_maximized();
                if (!maximized) {
                    width = window_->get_width();
                    height = window_->get_height();
                }
            }
            if (width > 0 && height > 0) {
                settingsWidth_ = width;
                settingsHeight_ = height;
            }
            settingsMaximized_ = maximized;

            keyFile->set_integer("Window", "Width", settingsWidth_);
            keyFile->set_integer("Window", "Height", settingsHeight_);
            keyFile->set_boolean("Window", "Maximized", settingsMaximized_);
        }

        if (includeRate) {
            keyFile->set_integer("Audio", "Rate", static_cast<int>(rawSampleRate_));
        }

        if (includeWarnings) {
            keyFile->set_boolean("Warnings", "ParameterExtensionMismatch",
                                 warnParameterExtensionMismatch_);
            keyFile->set_boolean("Warnings", "BinaryExtensionMismatch",
                                 warnBinaryExtensionMismatch_);
        }

        if (includeSpectrogram) {
            keyFile->set_integer("Spectrogram", "Palette",
                                 static_cast<int>(spectrogramPalette_));
            keyFile->set_double("Spectrogram", "RangeDb", spectrogramRangeDb_);
        }

        try {
            std::error_code code;
            std::filesystem::create_directories(path.parent_path(), code);
            if (code) {
                error = "Unable to create " + path.parent_path().string() + ": " + code.message();
                return false;
            }
            keyFile->save_to_file(path.string());
        } catch (const Glib::Error& failure) {
            error = failure.what();
            return false;
        }
        return true;
    }

    /*****************************************************************************
     * buildWindow
     *
     * Constructs the main window, controllers, menus, toolbar, plot area and
     * status bar. The layout is assembled once and then repainted through
     * queued draws.
     *
     *****************************************************************************/
    void buildWindow()
    {
        window_ = std::make_unique<Gtk::ApplicationWindow>();
        root_ = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL);
        toolbar_ = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        statusBar_ = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        statusLabel_ = std::make_unique<Gtk::Label>();
        fileLabel_ = std::make_unique<Gtk::Label>();
        cursorLabel_ = std::make_unique<Gtk::Label>();
        playbackLabel_ = std::make_unique<Gtk::Label>();
        wavRateLabel_ = std::make_unique<Gtk::Label>();
        infoLabel_ = std::make_unique<Gtk::Label>();
        plotArea_ = std::make_unique<PlotArea>();
        window_->set_application(application_);
        window_->set_title(std::string("Xsap ") + kVersion + " — Speech Analysis Program");
        window_->set_default_size(settingsWidth_, settingsHeight_);
        window_->set_size_request(kMinimumWidth, kMinimumHeight);
        window_->set_resizable(true);
        window_->set_child(*root_);
        window_->set_hide_on_close(false);
        windowKeyController_ = Gtk::EventControllerKey::create();
        windowKeyController_->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        windowKeyController_->signal_key_pressed().connect(
            sigc::slot<bool(guint, guint, Gdk::ModifierType)>(
                [this](guint keyval, guint keycode, Gdk::ModifierType state) {
                    return handleKey(keyval, keycode, state);
                }),
            false);
        window_->add_controller(windowKeyController_);

        installCss();
        installActions();
        menuBar_ = buildMenuBar();
        menuBar_->add_css_class("main-menu");
        root_->append(*menuBar_);
        buildToolbar();
        root_->append(*toolbar_);

        plotArea_->set_callbacks(
            [this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
                draw(cr, width, height);
            },
            [this](double x, double y, unsigned int button) { handleClick(x, y, button); },
            [this](double x, double y) { handleMotion(x, y); },
            [this](guint keyval, guint keycode, Gdk::ModifierType state) {
                return handleKey(keyval, keycode, state);
            });
        root_->append(*plotArea_);
        plotArea_->grab_focus();

        buildStatusBar();
        root_->append(*statusBar_);
        closeConnection_ = window_->signal_close_request().connect(
            sigc::slot<bool()>([this]() {
                stopPlayback(false);
                // WinSap saves the window geometry from OnDestroy on every
                // exit, so do the same here. The sampling rate is deliberately
                // left alone; it is only stored on request.
                std::string error;
                saveSettings(error, true, false, false, false);
                return false;
            }),
            false);
        updateStatusSegments();
        plotArea_->queue_draw();
    }

    /*****************************************************************************
     * installCss
     *
     * Installs the dark application styling used by the GTK widgets. CSS keeps
     * the chrome close to the WinSap colours without custom drawing every
     * control.
     *
     *****************************************************************************/
    void installCss()
    {
        auto provider = Gtk::CssProvider::create();
        provider->load_from_data(R"css(
            window {
                background: #10151e;
            }
            .main-menu {
                background: #17202c;
                color: #e7eef7;
            }
            .main-menu > menubutton > button {
                min-height: 28px;
                padding: 2px 8px;
            }
            .xsap-menu {
                padding: 4px;
            }
            .xsap-menu button,
            .xsap-menu checkbutton {
                min-width: 240px;
                min-height: 28px;
                padding: 3px 8px;
            }
            .xsap-menu .menu-shortcut {
                color: alpha(currentColor, 0.7);
            }
            .toolbar {
                background: #1b2635;
                border-bottom: 1px solid #304154;
                padding: 3px 6px;
            }
            .toolbar button {
                color: #d9e8f5;
                min-width: 39px;
                min-height: 39px;
                padding: 3px;
            }
            .toolbar button:hover {
                background: #2b455c;
            }
            .statusbar {
                background: #17202c;
                border-top: 1px solid #304154;
                padding: 3px 4px;
                color: #b3c4d7;
            }
            .status-segment {
                padding: 0 6px;
                font-family: monospace;
            }
            .status-dim {
                color: #66788c;
            }
            .status-warn {
                color: #ff5555;
            }
            entry, spinbutton {
                min-height: 30px;
            }
        )css");
        const auto display = Gdk::Display::get_default();
        if (display) {
            Gtk::StyleContext::add_provider_for_display(
                display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }

    /*****************************************************************************
     * addAction
     *
     * Registers a stateless application action. The callback runs from the idle
     * loop so menu popovers can release their grab before more UI opens.
     *
     *****************************************************************************/
    void addAction(const std::string& name, const std::function<void()>& callback)
    {
        auto action = Gio::SimpleAction::create(name);
        action->signal_activate().connect([this, name, callback](const Glib::VariantBase&) {
            // Let a menu popover release its GTK grab before an action opens or hides UI.
            Glib::signal_idle().connect_once([this, name, callback] {
                if (playbackJob_.active && name != "stop" && name != "quit") {
                    playbackCommandFeedback_ = "Stop playback before using commands.";
                    updateStatusSegments();
                    return;
                }
                callback();
            });
        });
        application_->add_action(action);
    }

    /*****************************************************************************
     * addToggleAction
     *
     * Registers a boolean application action and mirrors GTK requested state
     * before invoking app logic. Menus and accelerators then share one source
     * of truth.
     *
     *****************************************************************************/
    void addToggleAction(const std::string& name,
                         bool initialState,
                         const std::function<void(bool)>& callback)
    {
        auto action = Gio::SimpleAction::create_bool(name, initialState);
        action->signal_change_state().connect(
            [this, action, callback](const Glib::VariantBase& requestedState) {
                if (playbackJob_.active) {
                    playbackCommandFeedback_ = "Stop playback before using commands.";
                    updateStatusSegments();
                    return;
                }
                const bool enabled =
                    Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(requestedState).get();
                action->set_state(requestedState);
                Glib::signal_idle().connect_once(
                    [callback, enabled]() { callback(enabled); });
            });
        application_->add_action(action);
    }

    /*****************************************************************************
     * setToggleActionState
     *
     * Updates a toggle action without running its callback. Panel-open paths
     * use this to keep menu checkmarks in step with state changes.
     *
     *****************************************************************************/
    void setToggleActionState(const std::string& name, bool enabled)
    {
        const auto action =
            std::dynamic_pointer_cast<Gio::SimpleAction>(application_->lookup_action(name));
        if (action) {
            action->set_state(Glib::Variant<bool>::create(enabled));
        }
    }

    /*****************************************************************************
     * installActions
     *
     * Creates every menu, toolbar and accelerator action. Centralising the
     * bindings keeps the WinSap-compatible shortcuts consistent across entry
     * points.
     *
     *****************************************************************************/
    void installActions()
    {
        addAction("open-upper", [this] { openWithChooser(true); });
        addAction("open-down", [this] { openWithChooser(false); });
        addAction("save-as", [this] { saveMarkedBlock(); });
        addAction("close-files", [this] { showCloseFilesDialog(); });
        addAction("close-all", [this] {
            closePanel(true);
            closePanel(false);
            setStatus("Closed all files.");
        });
        addAction("reload", [this] { reloadPanels(); });
        addAction("quit", [this] {
            stopPlayback(false);
            // Quitting the application does not raise close-request, so the
            // geometry has to be written here as well.
            std::string error;
            saveSettings(error, true, false, false, false);
            application_->quit();
        });
        addAction("frame-length-up", [this] { showFrameLength(true); });
        addAction("frame-length-down", [this] { showFrameLength(false); });
        addAction("goto-up", [this] { showGoto(true); });
        addAction("goto-down", [this] { showGoto(false); });
        addAction("visible-up", [this] { showVisibleFrames(true); });
        addAction("visible-down", [this] { showVisibleFrames(false); });
        addAction("pages-up", [this] { showFrameSkip(true); });
        addAction("pages-down", [this] { showFrameSkip(false); });
        addAction("limits-up", [this] { showLimits(true); });
        addAction("limits-down", [this] { showLimits(false); });
        addAction("backward", [this] { navigate([](Panel& panel, std::string& error) {
                                           return panel.moveOneFrame(-1, error);
                                       },
                                                "Moved backward."); });
        addAction("forward", [this] { navigate([](Panel& panel, std::string& error) {
                                          return panel.moveOneFrame(1, error);
                                      },
                                               "Moved forward."); });
        addAction("fast-backward", [this] { navigate([](Panel& panel, std::string& error) {
                                                return panel.moveBySkip(-1, error);
                                            },
                                                     "Moved backward by frame skip."); });
        addAction("fast-forward", [this] { navigate([](Panel& panel, std::string& error) {
                                               return panel.moveBySkip(1, error);
                                           },
                                                    "Moved forward by frame skip."); });
        addAction("zoom-in", [this] { navigateAll([](Panel& panel, std::string& error) {
                                          return panel.zoomIn(error);
                                      },
                                                  "Zoomed in."); });
        addAction("zoom-out", [this] { navigateAll([](Panel& panel, std::string& error) {
                                           return panel.zoomOut(error);
                                       },
                                                   "Zoomed out."); });
        addAction("view-all", [this] { navigateAll([](Panel& panel, std::string& error) {
                                           return panel.viewAll(error);
                                       },
                                                   "Showing all frames."); });
        addAction("expand", [this] { changeAmplitudeAll([](Panel& panel, std::string& error) {
                                         return panel.expandAmplitude(0.5, error);
                                     },
                                                        "Expanded amplitude."); });
        addAction("compress", [this] { changeAmplitudeAll([](Panel& panel, std::string& error) {
                                           return panel.compressAmplitude(2.0, error);
                                       },
                                                          "Compressed amplitude."); });
        addAction("overlay", [this] {
            overlayLower_ = !overlayLower_;
            setStatus(std::string("Lower waveform overlay ") +
                      (overlayLower_ ? "enabled." : "disabled."));
            redraw();
        });
        addToggleAction("lock", lockStarts_, [this](bool enabled) {
            lockStarts_ = enabled;
            if (lockStarts_ && upperPanel_.openTrackCount() != 0 &&
                lowerPanel_.openTrackCount() != 0) {
                std::string ignored;
                lowerPanel_.setFrameBeginFrom(upperPanel_, ignored);
            }
            setStatus(std::string("Panel starting-frame lock ") +
                      (lockStarts_ ? "enabled." : "disabled."));
            redraw();
        });
        addAction("look-marked", [this] { lookMarkedBlock(); });
        addAction("sync-panels", [this] { syncPanels(); });
        addAction("clear-mark", [this] { activePanel().clearMark(); pendingMark().reset(); setStatus("Mark cleared."); redraw(); });
        addAction("configure-rate", [this] { showRawSampleRate(); });
        addAction("spectrogram-contrast-up", [this] { adjustSpectrogramRange(-5.0f); });
        addAction("spectrogram-contrast-down", [this] { adjustSpectrogramRange(5.0f); });
        addAction("spectrogram-palette", [this] { cycleSpectrogramPalette(); });
        addAction("save-raw-rate", [this] {
            std::string error;
            if (saveSettings(error, false, true, false, false)) {
                setStatus("Raw samplingrate saved to " + settingsPath().string() + ".");
            } else {
                setStatus("Unable to save raw samplingrate: " + error);
            }
        });
        addAction("help-topics", [this] { showHelpTopics(); });
        addAction("play-all", [this] { playAll(); });
        addAction("play-up", [this] { playPanel(true); });
        addAction("play-down", [this] { playPanel(false); });
        addAction("stop", [this] { stopPlayback(true); });
        addAction("about", [this] { showAboutDialog(); });
    }

    /*****************************************************************************
     * menuRow
     *
     * Builds a single row for a popover menu, including icon, label and
     * shortcut text. The row layout recreates a traditional menu inside GTK 4
     * widgets.
     *
     *****************************************************************************/
    Gtk::Box* menuRow(const MenuEntry& entry)
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        if (entry.iconFile[0] != '\0' || entry.fallbackIcon[0] != '\0') {
            row->append(*icon(entry.iconFile, entry.fallbackIcon, 16));
        }
        auto label = Gtk::make_managed<Gtk::Label>(entry.label);
        label->set_use_underline(true);
        label->set_xalign(0.0F);
        label->set_hexpand(true);
        row->append(*label);
        if (entry.shortcut[0] != '\0') {
            auto shortcut = Gtk::make_managed<Gtk::Label>(entry.shortcut);
            shortcut->add_css_class("menu-shortcut");
            shortcut->set_xalign(1.0F);
            row->append(*shortcut);
        }
        return row;
    }

    /*****************************************************************************
     * menuMnemonic
     *
     * Extracts the underlined mnemonic from a menu label and converts it to a
     * key value. The popover menus need this because they are built manually.
     *
     *****************************************************************************/
    static guint menuMnemonic(const char* label)
    {
        const std::string text(label);
        const auto marker = text.find('_');
        if (marker == std::string::npos || marker + 1 >= text.size()) {
            return 0;
        }
        return gdk_keyval_to_lower(
            gdk_unicode_to_keyval(static_cast<gunichar>(
                static_cast<unsigned char>(text[marker + 1]))));
    }

    /*****************************************************************************
     * activateMenuEntry
     *
     * Runs the action behind a mnemonic, toggling check actions by changing
     * their state. Keyboard menu navigation then behaves like clicking.
     *
     *****************************************************************************/
    void activateMenuEntry(const MenuHotkey& hotkey)
    {
        if (hotkey.check) {
            const auto action =
                std::dynamic_pointer_cast<Gio::SimpleAction>(
                    application_->lookup_action(hotkey.action));
            if (action) {
                bool current = false;
                action->get_state(current);
                action->change_state(Glib::Variant<bool>::create(!current));
            }
            return;
        }
        application_->activate_action(hotkey.action);
    }

    /*****************************************************************************
     * openAdjacentMenu
     *
     * Moves keyboard focus to the neighbouring top-level menu. The idle hop
     * lets the current popover close before the next one grabs focus.
     *
     *****************************************************************************/
    void openAdjacentMenu(std::size_t current, int direction)
    {
        if (menus_.empty()) {
            return;
        }
        const std::size_t count = menus_.size();
        const std::size_t target =
            direction < 0 ? (current + count - 1) % count : (current + 1) % count;
        menus_[current].popover->popdown();
        Glib::signal_idle().connect_once([this, target]() {
            menus_[target].button->popup();
            if (menus_[target].firstItem != nullptr) {
                menus_[target].firstItem->grab_focus();
            }
        });
    }

    /*****************************************************************************
     * handleOpenMenuKey
     *
     * Handles arrow, escape and mnemonic keys while a popover menu is open. It
     * supplies the classic menu-bar keyboard behaviour missing from plain GTK
     * buttons.
     *
     *****************************************************************************/
    bool handleOpenMenuKey(guint keyval, Gdk::ModifierType state)
    {
        for (std::size_t index = 0; index < menus_.size(); ++index) {
            MenuRecord& menu = menus_[index];
            if (!menu.popover->get_visible()) {
                continue;
            }
            if (keyval == GDK_KEY_Left) {
                openAdjacentMenu(index, -1);
                return true;
            }
            if (keyval == GDK_KEY_Right) {
                openAdjacentMenu(index, 1);
                return true;
            }
            if (keyval == GDK_KEY_Escape) {
                menu.popover->popdown();
                plotArea_->grab_focus();
                return true;
            }
            const auto modifiers =
                Gdk::ModifierType::ALT_MASK | Gdk::ModifierType::CONTROL_MASK |
                Gdk::ModifierType::SUPER_MASK;
            if ((state & modifiers) != Gdk::ModifierType{}) {
                return false;
            }
            const guint mnemonic = gdk_keyval_to_lower(keyval);
            const auto match =
                std::find_if(menu.hotkeys.begin(), menu.hotkeys.end(),
                             [mnemonic](const MenuHotkey& hotkey) {
                                 return hotkey.keyval == mnemonic;
                             });
            if (match != menu.hotkeys.end()) {
                const MenuHotkey hotkey = *match;
                menu.popover->popdown();
                Glib::signal_idle().connect_once(
                    [this, hotkey]() { activateMenuEntry(hotkey); });
                return true;
            }
            return false;
        }
        return false;
    }

    /*****************************************************************************
     * addMenu
     *
     * Builds one top-level popover menu from the declarative entry table. The
     * same table supplies rows, actions and mnemonic hotkeys.
     *
     *****************************************************************************/
    void addMenu(Gtk::Box& bar,
                 const char* title,
                 const std::vector<MenuEntry>& entries)
    {
        auto menuButton = Gtk::make_managed<Gtk::MenuButton>();
        menuButton->set_has_frame(false);
        auto titleLabel = Gtk::make_managed<Gtk::Label>(title);
        titleLabel->set_use_underline(true);
        titleLabel->set_mnemonic_widget(*menuButton);
        menuButton->set_child(*titleLabel);

        auto popover = Gtk::make_managed<Gtk::Popover>();
        popover->set_autohide(true);
        auto menuKeyController = Gtk::EventControllerKey::create();
        menuKeyController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        menuKeyController->signal_key_pressed().connect(
            sigc::slot<bool(guint, guint, Gdk::ModifierType)>(
                [this](guint keyval, guint, Gdk::ModifierType state) {
                    return handleOpenMenuKey(keyval, state);
                }),
            false);
        popover->add_controller(menuKeyController);
        auto items = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        items->add_css_class("xsap-menu");
        MenuRecord menu{menuButton, popover, nullptr, {}};

        for (const auto& entry : entries) {
            if (entry.action[0] == '\0') {
                items->append(*Gtk::make_managed<Gtk::Separator>(
                    Gtk::Orientation::HORIZONTAL));
                continue;
            }
            if (entry.check) {
                auto item = Gtk::make_managed<Gtk::CheckButton>();
                item->set_child(*menuRow(entry));
                item->set_action_name("app." + std::string(entry.action));
                item->signal_toggled().connect([popover]() {
                    Glib::signal_idle().connect_once([popover]() { popover->popdown(); });
                });
                items->append(*item);
                if (menu.firstItem == nullptr) {
                    menu.firstItem = item;
                }
            } else {
                auto item = Gtk::make_managed<Gtk::Button>();
                item->set_has_frame(false);
                item->set_child(*menuRow(entry));
                item->signal_clicked().connect([this, popover, action = std::string(entry.action)] {
                    popover->popdown();
                    Glib::signal_idle().connect_once(
                        [this, action]() { application_->activate_action(action); });
                });
                items->append(*item);
                if (menu.firstItem == nullptr) {
                    menu.firstItem = item;
                }
            }
            const guint mnemonic = menuMnemonic(entry.label);
            if (mnemonic != 0) {
                menu.hotkeys.push_back({mnemonic, entry.action, entry.check});
            }
        }
        popover->set_child(*items);
        menuButton->set_popover(*popover);
        bar.append(*menuButton);
        menus_.push_back(std::move(menu));
    }

    /*****************************************************************************
     * buildMenuBar
     *
     * Builds the complete menu bar and installs WinSap-compatible accelerators.
     * The tables keep labels, icons and shortcuts close to their actions.
     *
     *****************************************************************************/
    Gtk::Box* buildMenuBar()
    {
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        addMenu(*bar, "_File", {
                                   {"_Open Up ...", "open-upper", "openUp.png", "document-open", "F3", false},
                                   {"O_pen Down...", "open-down", "openDown.png", "document-open", "F4", false},
                                   {"", "", "", "", "", false},
                                   {"_Save as...", "save-as", "saveAs.png", "document-save", "F2", false},
                                   {"_Close Files...", "close-files", "", "", "F9", false},
                                   {"Close _All", "close-all", "", "", "Ctrl+W", false},
                                   {"", "", "", "", "", false},
                                   {"_Reload All", "reload", "", "", "Ctrl+A", false},
                                   {"", "", "", "", "", false},
                                   {"E_xit", "quit", "", "", "Alt+X", false},
                               });
        addMenu(*bar, "_Up", {
                                 {"_Frame Length", "frame-length-up", "", "", "", false},
                                 {"_Goto Frame", "goto-up", "", "", "", false},
                                 {"_Number of Frames", "visible-up", "", "", "", false},
                                 {"_Limit", "limits-up", "", "", "", false},
                                 {"_Page Numbers", "pages-up", "", "", "", false},
                             });
        addMenu(*bar, "_Down", {
                                   {"_Frame Length", "frame-length-down", "", "", "", false},
                                   {"_Goto Frame", "goto-down", "", "", "", false},
                                   {"_Number of Frames", "visible-down", "", "", "", false},
                                   {"_Limit", "limits-down", "", "", "", false},
                                   {"_Page Numbers", "pages-down", "", "", "", false},
                               });
        addMenu(*bar, "_Tools", {
                                    {"Move Frame _Forward", "forward", "moveForward.png", "go-next", "Right", false},
                                    {"Move Frame _Backward", "backward", "moveBackward.png", "go-previous", "Left", false},
                                    {"F_ast Forward", "fast-forward", "fastForward.png", "media-seek-forward", "PgDn", false},
                                    {"Fa_st Backward", "fast-backward", "fastBackward.png", "media-seek-backward", "PgUp", false},
                                    {"", "", "", "", "", false},
                                    {"A_dd Down to Up", "overlay", "", "", "Ctrl+F2", false},
                                    {"", "", "", "", "", false},
                                    {"_Expand", "expand", "", "", "Ctrl+F5", false},
                                    {"_Compress", "compress", "", "", "Ctrl+F6", false},
                                    {"", "", "", "", "", false},
                                    {"_Lock Starting Frame", "lock", "", "", "Ctrl+L", true},
                                });
        addMenu(*bar, "_Play", {
                                   {"_Play All", "play-all", "playAll.png", "media-playback-start", "F5", false},
                                   {"P_lay Up", "play-up", "playUp.png", "media-playback-start", "F6", false},
                                   {"Pl_ay Down", "play-down", "playDown.png", "media-playback-start", "F7", false},
                                   {"_Stop", "stop", "", "", "F8", false},
                               });
        addMenu(*bar, "_Options", {
                                      {"Set _Raw Samplingrate...", "configure-rate", "", "", "", false},
                                      {"_Save Raw Samplingrate", "save-raw-rate", "", "", "", false},
                                      {"", "", "", "", "", false},
                                      {"Spectrogram Contrast _Up", "spectrogram-contrast-up", "", "", "Ctrl+U", false},
                                      {"Spectrogram Contrast _Down", "spectrogram-contrast-down", "", "", "Ctrl+D", false},
                                      {"Spectrogram _Colour Map", "spectrogram-palette", "", "", "Ctrl+P", false},
                                  });
        addMenu(*bar, "_Window", {
                                     {"_Look Marked Block", "look-marked", "", "", "Ctrl+F4", false},
                                     {"_Sync Panels", "sync-panels", "", "", "Ctrl+F10", false},
                                     {"", "", "", "", "", false},
                                     {"_Zoom In", "zoom-in", "zoomIn.png", "zoom-in", "Ctrl+F7", false},
                                     {"Z_oom Out", "zoom-out", "zoomOut.png", "zoom-out", "Ctrl+F8", false},
                                     {"_View all", "view-all", "viewAll.png", "zoom-fit-best", "Ctrl+F9", false},
                                 });
        addMenu(*bar, "_Help", {
                                   {"_Help Topics", "help-topics", "", "", "F1", false},
                                   {"", "", "", "", "", false},
                                   {"_About Xsap...", "about", "about.png", "help-about", "", false},
                               });

        application_->set_accels_for_action("app.save-as", {"F2"});
        application_->set_accels_for_action("app.open-upper", {"F3"});
        application_->set_accels_for_action("app.open-down", {"F4"});
        application_->set_accels_for_action("app.play-all", {"F5"});
        application_->set_accels_for_action("app.play-up", {"F6"});
        application_->set_accels_for_action("app.play-down", {"F7"});
        application_->set_accels_for_action("app.stop", {"F8"});
        application_->set_accels_for_action("app.close-files", {"F9"});
        application_->set_accels_for_action("app.backward", {"Left"});
        application_->set_accels_for_action("app.forward", {"Right"});
        application_->set_accels_for_action("app.fast-backward", {"Page_Up"});
        application_->set_accels_for_action("app.fast-forward", {"Page_Down"});
        application_->set_accels_for_action("app.overlay", {"<Primary>F2"});
        application_->set_accels_for_action("app.look-marked", {"<Primary>F4"});
        application_->set_accels_for_action("app.sync-panels", {"<Primary>F10"});
        application_->set_accels_for_action("app.expand", {"<Primary>F5"});
        application_->set_accels_for_action("app.compress", {"<Primary>F6"});
        application_->set_accels_for_action("app.zoom-in", {"<Primary>F7"});
        application_->set_accels_for_action("app.zoom-out", {"<Primary>F8"});
        application_->set_accels_for_action("app.view-all", {"<Primary>F9"});
        application_->set_accels_for_action("app.lock", {"<Primary>L"});
        application_->set_accels_for_action("app.close-all", {"<Primary>w"});
        application_->set_accels_for_action("app.reload", {"<Primary>a"});
        application_->set_accels_for_action("app.spectrogram-contrast-up", {"<Primary>u"});
        application_->set_accels_for_action("app.spectrogram-contrast-down", {"<Primary>d"});
        application_->set_accels_for_action("app.spectrogram-palette", {"<Primary>p"});
        application_->set_accels_for_action("app.quit", {"<Alt>X"});
        application_->set_accels_for_action("app.help-topics", {"F1"});
        return bar;
    }

    /*****************************************************************************
     * assetDirectory
     *
     * Finds the icon directory beside the executable or in the install prefix.
     * Supporting both paths lets the same binary work from a build tree or
     * package.
     *
     *****************************************************************************/
    std::filesystem::path assetDirectory() const
    {
        const auto buildAssets = executablePath_.parent_path() / "assets";
        if (std::filesystem::is_directory(buildAssets / "icons")) {
            return buildAssets / "icons";
        }
#ifdef XSAP_INSTALL_ASSET_DIR
        const std::filesystem::path installed = std::filesystem::path(XSAP_INSTALL_ASSET_DIR) / "icons";
        if (std::filesystem::is_directory(installed)) {
            return installed;
        }
#endif
        return {};
    }

    /*****************************************************************************
     * icon
     *
     * Loads a toolbar or menu icon, preferring bundled artwork and falling back
     * to the theme. High-DPI companions are paired when available.
     *
     *****************************************************************************/
    Gtk::Widget* icon(const std::string& file,
                      const std::string& fallback,
                      int logicalSize = kToolbarIconSize)
    {
        const auto path = assetDirectory() / file;
        if (!path.empty() && std::filesystem::is_regular_file(path)) {
            try {
                auto normal = Gdk::Pixbuf::create_from_file(path.u8string());
                const auto highDpiPath =
                    path.parent_path() /
                    (path.stem().u8string() + "@2x" + path.extension().u8string());
                Glib::RefPtr<Gdk::Pixbuf> highDpi;
                if (std::filesystem::is_regular_file(highDpiPath)) {
                    highDpi = Gdk::Pixbuf::create_from_file(highDpiPath.u8string());
                }
                if (normal) {
                    return Gtk::make_managed<AdaptiveIcon>(normal, highDpi, logicalSize);
                }
            } catch (const Glib::Error&) {
            }
        }
        auto image = Gtk::make_managed<Gtk::Image>();
        image->set_from_icon_name(fallback);
        image->set_pixel_size(logicalSize);
        return image;
    }

    /*****************************************************************************
     * addToolbarButton
     *
     * Adds one icon button to the toolbar and connects it to an application
     * action. Sharing actions keeps toolbar clicks and shortcuts identical.
     *
     *****************************************************************************/
    void addToolbarButton(const std::string& file,
                          const std::string& fallback,
                          const std::string& tooltip,
                          const std::string& action)
    {
        auto button = Gtk::make_managed<Gtk::Button>();
        button->set_has_frame(false);
        button->set_tooltip_text(tooltip);
        button->set_child(*icon(file, fallback));
        button->signal_clicked().connect([this, action] {
            application_->activate_action(action);
        });
        toolbar_->append(*button);
    }

    /*****************************************************************************
     * addToolbarSeparator
     *
     * Adds the narrow vertical dividers that group WinSap-style toolbar
     * commands.
     *
     *****************************************************************************/
    void addToolbarSeparator()
    {
        auto separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
        separator->set_margin_start(4);
        separator->set_margin_end(4);
        toolbar_->append(*separator);
    }

    /*****************************************************************************
     * buildToolbar
     *
     * Builds the fixed toolbar in the historical command order. Each button
     * delegates to the action already used by menus and accelerators.
     *
     *****************************************************************************/
    void buildToolbar()
    {
        toolbar_->set_css_classes({"toolbar"});
        toolbar_->set_size_request(-1, kToolbarHeight);
        addToolbarButton("openUp.png", "document-open", "Open upper file (F3)", "open-upper");
        addToolbarButton("openDown.png", "document-open", "Open down file (F4)", "open-down");
        addToolbarButton("saveAs.png", "document-save", "Save marked block as wav (F2)", "save-as");
        addToolbarSeparator();
        addToolbarButton("fastBackward.png", "media-seek-backward", "Fast backward", "fast-backward");
        addToolbarButton("moveBackward.png", "go-previous", "Move backward", "backward");
        addToolbarButton("moveForward.png", "go-next", "Move forward", "forward");
        addToolbarButton("fastForward.png", "media-seek-forward", "Fast forward", "fast-forward");
        addToolbarButton("viewAll.png", "zoom-fit-best", "View all (Ctrl+F9)", "view-all");
        addToolbarSeparator();
        addToolbarButton("zoomIn.png", "zoom-in", "Zoom in (Ctrl+F7)", "zoom-in");
        addToolbarButton("zoomOut.png", "zoom-out", "Zoom out (Ctrl+F8)", "zoom-out");
        addToolbarSeparator();
        addToolbarButton("playAll.png", "media-playback-start", "Play all (F5)", "play-all");
        addToolbarButton("playUp.png", "media-playback-start", "Play upper (F6)", "play-up");
        addToolbarButton("playDown.png", "media-playback-start", "Play down (F7)", "play-down");
        addToolbarSeparator();
        addToolbarButton("about.png", "help-about", "About Xsap", "about");
    }

    /*****************************************************************************
     * buildStatusBar
     *
     * Creates the segmented status bar and fixes the widths of the compact
     * panes. Stable widths stop the plot area from shifting as hover text
     * changes.
     *
     *****************************************************************************/
    void buildStatusBar()
    {
        statusBar_->set_css_classes({"statusbar"});
        for (Gtk::Label* label :
             {statusLabel_.get(), fileLabel_.get(), cursorLabel_.get(), playbackLabel_.get(),
              wavRateLabel_.get(), infoLabel_.get()}) {
            label->set_xalign(0.0F);
            label->add_css_class("status-segment");
        }
        const auto fixPaneWidth = [](Gtk::Label& label, int characters) {
            label.set_width_chars(characters);
            label.set_max_width_chars(characters);
            label.set_ellipsize(Pango::EllipsizeMode::END);
            label.set_hexpand(false);
        };
        // Wide enough for the longest of the three readouts, which is the
        // spectrogram's " (00:00:01.234, 21094 Hz)".
        fixPaneWidth(*statusLabel_, 26);
        fixPaneWidth(*fileLabel_, 15);
        fixPaneWidth(*cursorLabel_, 15);
        fixPaneWidth(*playbackLabel_, 12);
        fixPaneWidth(*wavRateLabel_, 12);
        infoLabel_->set_hexpand(true);
        infoLabel_->set_ellipsize(Pango::EllipsizeMode::END);
        statusBar_->append(*statusLabel_);
        statusBar_->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        statusBar_->append(*fileLabel_);
        statusBar_->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        statusBar_->append(*cursorLabel_);
        statusBar_->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        statusBar_->append(*playbackLabel_);
        statusBar_->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        statusBar_->append(*wavRateLabel_);
        statusBar_->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        statusBar_->append(*infoLabel_);
    }

    /*****************************************************************************
     * setStatus
     *
     * Stores a status message, refreshes the status bar and queues a repaint.
     * Drawing and status text are kept in sync after every command.
     *
     *****************************************************************************/
    void setStatus(std::string status)
    {
        statusText_ = std::move(status);
        updateStatusSegments();
        redraw();
    }

    /*****************************************************************************
     * setSegmentState
     *
     * Applies the dimmed or warning style to one status bar segment. Both
     * classes are removed first so repeated refreshes cannot stack them.
     *
     *****************************************************************************/
    static void setSegmentState(Gtk::Label& label, bool dimmed, bool warning)
    {
        label.remove_css_class("status-dim");
        label.remove_css_class("status-warn");
        if (warning) {
            label.add_css_class("status-warn");
        } else if (dimmed) {
            label.add_css_class("status-dim");
        }
    }

    /*****************************************************************************
     * updateStatusSegments
     *
     * Refreshes the coordinate, mark, rate, playback and information panes.
     * Hover and playback text take precedence over the last command message.
     *
     *****************************************************************************/
    void updateStatusSegments()
    {
        if (!statusLabel_) {
            return;
        }
        statusLabel_->set_text(coordinateText_);
        fileLabel_->set_text(pendingUpperMark_.has_value()
                                 ? "Up Mark Left"
                                 : (upperPanel_.selectedSampleRange().has_value()
                                        ? "Up Mark All"
                                        : ""));
        cursorLabel_->set_text(pendingLowerMark_.has_value()
                                   ? "Down Mark Left"
                                   : (lowerPanel_.selectedSampleRange().has_value()
                                          ? "Down Mark All"
                                          : ""));
        // The raw rate is greyed while no raw stream is open because the
        // setting has nothing to act on. The wav rate stays blank until a wav
        // file is loaded and turns red when the open files disagree.
        bool rawActive = false;
        bool wavFound = false;
        bool wavMixed = false;
        uint32_t firstWavRate = 0;
        for (const Panel* source : {&upperPanel_, &lowerPanel_}) {
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                const Track* trackPtr = source->track(slot);
                if (trackPtr == nullptr || !trackPtr->isBinary()) {
                    continue;
                }
                if (trackPtr->isWav()) {
                    if (!wavFound) {
                        wavFound = true;
                        firstWavRate = trackPtr->sampleRate();
                    } else if (trackPtr->sampleRate() != firstWavRate) {
                        wavMixed = true;
                    }
                } else {
                    rawActive = true;
                }
            }
        }

        playbackLabel_->set_text("Raw " + std::to_string(rawSampleRate_) + " Hz");
        setSegmentState(*playbackLabel_, !rawActive, false);

        wavRateLabel_->set_text(wavFound ? "Wav " + std::to_string(firstWavRate) + " Hz"
                                         : std::string());
        setSegmentState(*wavRateLabel_, false, wavMixed);

        if (playbackJob_.active) {
            infoLabel_->set_text(
                playbackCommandFeedback_.empty()
                    ? formatTime(playbackSamples_.load(), playbackJob_.sampleRate) + "  " +
                          playbackJob_.description + "  (right-click to stop)"
                    : playbackCommandFeedback_);
        } else {
            infoLabel_->set_text(hoverFileText_.empty() ? statusText_ : hoverFileText_);
        }
    }

    /*****************************************************************************
     * redraw
     *
     * Queues a plot repaint and refreshes status text. Callers can use one
     * helper after any state change that affects the display.
     *
     *****************************************************************************/
    void redraw()
    {
        syncStereoState(std::nullopt);
        if (plotArea_) {
            plotArea_->queue_draw();
        }
        updateStatusSegments();
    }

    Panel& panel(bool upper) noexcept
    {
        return upper ? upperPanel_ : lowerPanel_;
    }

    const Panel& panel(bool upper) const noexcept
    {
        return upper ? upperPanel_ : lowerPanel_;
    }

    Panel& activePanel() noexcept
    {
        return panel(activeUpper_);
    }

    const Panel& activePanel() const noexcept
    {
        return panel(activeUpper_);
    }

    std::optional<int64_t>& pendingMark() noexcept
    {
        return activeUpper_ ? pendingUpperMark_ : pendingLowerMark_;
    }

    std::optional<int64_t>& pendingMark(bool upper) noexcept
    {
        return upper ? pendingUpperMark_ : pendingLowerMark_;
    }

    /*****************************************************************************
     * selectedTrack
     *
     * Returns the currently selected track if it still exists. Closing files
     * can invalidate the saved slot, so the lookup is checked every time.
     *
     *****************************************************************************/
    std::optional<TrackRef> selectedTrack() const noexcept
    {
        if (!selection_.valid) {
            return std::nullopt;
        }
        const Track* trackPtr = panel(selection_.upper).track(selection_.slot);
        if (trackPtr == nullptr) {
            return std::nullopt;
        }
        return TrackRef{selection_.slot, trackPtr};
    }

    /*****************************************************************************
     * selectedTrackForPanel
     *
     * Returns the selection only when it belongs to the requested panel.
     * Dialogs use this to avoid applying a panel command to the wrong side.
     *
     *****************************************************************************/
    std::optional<TrackRef> selectedTrackForPanel(bool upper) const noexcept
    {
        if (!selection_.valid || selection_.upper != upper) {
            return std::nullopt;
        }
        const Track* trackPtr = panel(upper).track(selection_.slot);
        return trackPtr != nullptr ? std::optional<TrackRef>(TrackRef{selection_.slot, trackPtr})
                                   : std::nullopt;
    }

    /*****************************************************************************
     * firstBinaryTrack
     *
     * Returns slot zero when it contains a binary track. WinSAP panel playback
     * and export are tied to that slot and do not follow track selection.
     *
     *****************************************************************************/
    std::optional<TrackRef> firstBinaryTrack(const Panel& source) const noexcept
    {
        const Track* trackPtr = source.track(0);
        if (trackPtr != nullptr && trackPtr->isBinary()) {
            return TrackRef{0, trackPtr};
        }
        return std::nullopt;
    }

    /*****************************************************************************
     * setActivePanel
     *
     * Marks the upper or down panel as the active target and repaints the
     * display.
     *
     *****************************************************************************/
    void setActivePanel(bool upper)
    {
        activeUpper_ = upper;
        redraw();
    }

    /*****************************************************************************
     * syncPanelActivity
     *
     * Moves focus to the other panel when the selected panel becomes empty.
     * A panel participates automatically whenever it contains an open file.
     *
     *****************************************************************************/
    void syncPanelActivity()
    {
        if (!panel(activeUpper_).isActive() && panel(!activeUpper_).isActive()) {
            activeUpper_ = !activeUpper_;
        }
    }

    /*****************************************************************************
     * selectTrack
     *
     * Selects an open slot and makes its panel active. Status feedback mirrors
     * the small selection cues used by the original interface.
     *
     *****************************************************************************/
    void selectTrack(bool upper, std::size_t slot)
    {
        if (panel(upper).track(slot) == nullptr) {
            return;
        }
        selection_ = Selection{true, upper, slot};
        setActivePanel(upper);
        setStatus(std::string("Selected ") + (upper ? "upper" : "down") + " slot " +
                  std::to_string(slot + 1) + ".");
    }

    /*****************************************************************************
     * refreshSelection
     *
     * Repairs the selection after files are closed or reloaded. It preserves a
     * valid choice where possible and otherwise chooses the first remaining
     * track.
     *
     *****************************************************************************/
    void refreshSelection()
    {
        syncPanelActivity();
        if (selectedTrack().has_value()) {
            redraw();
            return;
        }
        selection_.valid = false;
        for (bool upper : {activeUpper_, !activeUpper_}) {
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                if (panel(upper).track(slot) != nullptr) {
                    selection_ = Selection{true, upper, slot};
                    setActivePanel(upper);
                    redraw();
                    return;
                }
            }
        }
        redraw();
    }

    /*****************************************************************************
     * closePanel
     *
     * Closes every track in one panel and clears its pending mark. Selection
     * and activity are then rebuilt from the remaining tracks.
     *
     *****************************************************************************/
    void closePanel(bool upper)
    {
        panel(upper).closeAll();
        syncPanelActivity();
        pendingMark(upper).reset();
        setStatus(std::string("Closed all ") + (upper ? "upper" : "down") + " tracks.");
        refreshSelection();
    }

    /*****************************************************************************
     * panelHasFft
     *
     * Reports whether a panel contains an FFT amplitude track. Stereo sync and
     * the playback cursor treat such a panel differently.
     *
     *****************************************************************************/
    bool panelHasFft(const Panel& source) const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr != nullptr && trackPtr->type() == TrackType::FftAmplitude) {
                return true;
            }
        }
        return false;
    }

    /*****************************************************************************
     * panelHasBinary
     *
     * Reports whether a panel contains a waveform or FFT amplitude track, the
     * files that can be marked and played.
     *
     *****************************************************************************/
    bool panelHasBinary(const Panel& source) const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr != nullptr && trackPtr->isBinary()) {
                return true;
            }
        }
        return false;
    }

    /*****************************************************************************
     * stereoActive / stereoFftActive
     *
     * A stereo file is open when the upper panel holds a left channel; it is
     * in FFT mode when that channel is an FFT amplitude track (WinSap
     * StereoActive / StereoFftActive).
     *
     *****************************************************************************/
    bool stereoActive() const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = upperPanel_.track(slot);
            if (trackPtr != nullptr && trackPtr->isStereoChannel()) {
                return true;
            }
        }
        return false;
    }

    bool stereoFftActive() const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = upperPanel_.track(slot);
            if (trackPtr != nullptr && trackPtr->isStereoChannel() &&
                trackPtr->type() == TrackType::FftAmplitude) {
                return true;
            }
        }
        return false;
    }

    StereoView stereoView(bool upper) const
    {
        const Panel& source = panel(upper);
        return StereoView{source.frameBegin(),
                          source.visibleFrameCount(),
                          source.frameLength(),
                          source.frameSkip(),
                          source.lowerLimit(),
                          source.upperLimit(),
                          source.selectedSampleRange(),
                          upper ? pendingUpperMark_ : pendingLowerMark_};
    }

    /*****************************************************************************
     * syncStereoState
     *
     * Keeps both halves of every stereo file together and, while one is open,
     * gives the two panels the same view and mark (WinSap SyncStereoState). A
     * channel whose partner was closed is closed as well. Without an explicit
     * source the panel that changed since the last sync is copied.
     *
     *****************************************************************************/
    void syncStereoState(std::optional<bool> fromUpper)
    {
        if (syncingStereo_) {
            return;
        }
        syncingStereo_ = true;
        bool closed = false;
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* upTrack = upperPanel_.track(slot);
            const Track* downTrack = lowerPanel_.track(slot);
            const bool up = upTrack != nullptr && upTrack->isStereoChannel();
            const bool down = downTrack != nullptr && downTrack->isStereoChannel();
            if (!up && !down) {
                continue;
            }
            if (up && down && upTrack->channel() == 0 && downTrack->channel() == 1 &&
                upTrack->path() == downTrack->path()) {
                continue;
            }
            std::string ignored;
            if (up) {
                upperPanel_.closeTrack(slot, ignored);
            }
            if (down) {
                lowerPanel_.closeTrack(slot, ignored);
            }
            closed = true;
        }
        if (closed) {
            for (const bool upper : {true, false}) {
                if (panel(upper).openTrackCount() == 0) {
                    pendingMark(upper).reset();
                }
            }
            refreshSelection();
        }

        if (stereoActive()) {
            if (!fromUpper.has_value()) {
                if (!(stereoView(true) == lastUpperView_)) {
                    fromUpper = true;
                } else if (!(stereoView(false) == lastLowerView_)) {
                    fromUpper = false;
                }
            }
            if (fromUpper.has_value()) {
                const Panel& source = panel(*fromUpper);
                Panel& target = panel(!*fromUpper);
                std::string ignored;
                // Frame length first: it decides how many frames the files hold
                if (target.frameLength() != source.frameLength()) {
                    target.setFrameLength(source.frameLength(), ignored);
                }
                target.setVisibleFrameCount(source.visibleFrameCount(), ignored);
                target.setFrameBegin(source.frameBegin(), ignored);
                target.setFrameSkip(source.frameSkip(), ignored);

                // FFT amplitude files use dB limits, which do not carry over to samples
                if (panelHasFft(source) == panelHasFft(target) &&
                    (target.lowerLimit() != source.lowerLimit() ||
                     target.upperLimit() != source.upperLimit())) {
                    target.setAmplitudeLimits(source.lowerLimit(), source.upperLimit(), ignored);
                }

                const std::optional<SampleRange> mark = source.selectedSampleRange();
                const std::optional<int64_t> pending = pendingMark(*fromUpper);
                target.clearMark();
                pendingMark(!*fromUpper).reset();
                if (mark.has_value()) {
                    if (!target.setMarkStart(mark->begin, ignored) ||
                        !target.setMarkEnd(mark->end, ignored)) {
                        target.clearMark();
                    }
                } else if (pending.has_value() && target.setMarkStart(*pending, ignored)) {
                    pendingMark(!*fromUpper) = pending;
                }
            }
        }
        lastUpperView_ = stereoView(true);
        lastLowerView_ = stereoView(false);
        syncingStereo_ = false;
    }

    /*****************************************************************************
     * allowMonoOpenWithStereo
     *
     * A mono file may join a stereo file only in the upper panel, and only when
     * that keeps waveform and FFT mode apart (WinSap AllowMonoOpenWithStereo).
     *
     *****************************************************************************/
    bool allowMonoOpenWithStereo(TrackType type)
    {
        if (!stereoActive()) {
            return true;
        }
        if (stereoFftActive()) {
            showMessage("Open File",
                        "A stereo file is open in FFT mode, and no other file can be open with it.\n"
                        "Close the stereo file first.");
            return false;
        }
        if (type == TrackType::FftAmplitude) {
            showMessage("Open File",
                        "A stereo file is open in waveform mode, so files cannot be opened in FFT mode.\n"
                        "Close the stereo files first.");
            return false;
        }
        return true;
    }

    /*****************************************************************************
     * openStereoFile
     *
     * Opens a stereo WAV with its left channel in upper slot upperIndex and its
     * right channel in the same lower slot; a negative index picks a free slot.
     * Files that cannot stay open with it are listed and closed only after the
     * user agrees (WinSap OpenStereoFile).
     *
     *****************************************************************************/
    void openStereoFile(const std::filesystem::path& path, int upperIndex, TrackType type)
    {
        // In FFT mode a stereo file stands alone. In waveform mode the lower panel
        // is given over to right channels and no file may be in FFT mode.
        const bool fftMode = type == TrackType::FftAmplitude;
        auto conflicts = [this, fftMode](bool upper, std::size_t slot) {
            const Track* trackPtr = panel(upper).track(slot);
            if (trackPtr == nullptr) {
                return false;
            }
            if (fftMode || trackPtr->type() == TrackType::FftAmplitude) {
                return true;
            }
            return !upper && !trackPtr->isStereoChannel();
        };
        bool hasConflicts = false;
        for (std::size_t slot = 0; slot < Panel::kMaxTracks && !hasConflicts; ++slot) {
            hasConflicts = conflicts(true, slot) || conflicts(false, slot);
        }
        if (!hasConflicts) {
            finishOpenStereoFile(path, upperIndex, type);
            return;
        }

        // Name each file that would be closed and the reason it cannot stay
        bool lowerFiles = false;
        bool fftFiles = false;
        std::string closeList;
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const bool closeUp = conflicts(true, slot);
            const bool closeDown = conflicts(false, slot);
            if (!closeUp && !closeDown) {
                continue;
            }
            const bool pair = isStereoPairSlot(slot);
            for (int side = 0; side < 2; ++side) {
                const Track* trackPtr = panel(side == 0).track(slot);
                if (trackPtr == nullptr || !(pair || (side == 0 ? closeUp : closeDown))) {
                    continue;
                }
                if (pair && side == 1) {
                    continue; // listed once as a stereo file
                }
                const bool fft = trackPtr->type() == TrackType::FftAmplitude;
                fftFiles = fftFiles || fft;
                lowerFiles = lowerFiles || (side == 1 && !fft);
                closeList += "    ";
                closeList += pair ? "Both panels" : (side == 0 ? "Upper panel" : "Lower panel");
                closeList += ": ";
                closeList += fileNameText(trackPtr->path());
                closeList += pair ? " (stereo" : " (";
                closeList += pair ? (fft ? ", FFT mode)" : ")") : (fft ? "FFT mode)" : "waveform)");
                closeList += "\n";
            }
        }

        std::string message = fileNameText(path);
        if (fftMode) {
            message += " is a stereo file being opened in FFT mode. Its left channel goes "
                       "to the upper panel and its right channel to the lower panel, and a "
                       "stereo file in FFT mode must be the only open file.\n";
        } else {
            message += " is a stereo file being opened in waveform mode. Its left channel "
                       "goes to the upper panel and its right channel to the lower panel.\n";
            if (lowerFiles) {
                message += "\n- The lower panel can then only show the right channels of "
                           "stereo files, so mono files open there must be closed.\n";
            }
            if (fftFiles) {
                message += "\n- Waveform and FFT mode cannot be mixed while a stereo file "
                           "is open, so files open in FFT mode must be closed.\n";
            }
        }
        message += "\nThese files will be closed:\n";
        message += closeList;
        message += "\nClose them and open the stereo file?";
        showConfirm(
            "Open Stereo File", message,
            [this, path, upperIndex, type, conflicts]() {
                for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                    bool closeUp = conflicts(true, slot);
                    bool closeDown = conflicts(false, slot);
                    // A stereo file goes as a whole
                    if ((closeUp || closeDown) && isStereoPairSlot(slot)) {
                        closeUp = closeDown = true;
                    }
                    std::string ignored;
                    if (closeUp) {
                        upperPanel_.closeTrack(slot, ignored);
                    }
                    if (closeDown) {
                        lowerPanel_.closeTrack(slot, ignored);
                    }
                }
                // Everything else is gone, so the file takes the first slot
                finishOpenStereoFile(path, type == TrackType::FftAmplitude ? 0 : upperIndex,
                                     type);
            },
            [this]() { setStatus("Stereo file not opened."); });
    }

    bool isStereoPairSlot(std::size_t slot) const noexcept
    {
        const Track* upTrack = upperPanel_.track(slot);
        const Track* downTrack = lowerPanel_.track(slot);
        return upTrack != nullptr && upTrack->isStereoChannel() && downTrack != nullptr &&
               downTrack->isStereoChannel();
    }

    /*****************************************************************************
     * finishOpenStereoFile
     *
     * Loads both channels of a stereo file into the same slot of the two
     * panels once any conflicting files are closed, then syncs the lower view
     * to the upper one.
     *
     *****************************************************************************/
    bool finishOpenStereoFile(const std::filesystem::path& path, int upperIndex, TrackType type)
    {
        if (upperIndex < 0 || upperIndex >= static_cast<int>(Panel::kMaxTracks)) {
            upperIndex = static_cast<int>(Panel::kMaxTracks) - 1;
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                if (upperPanel_.track(slot) == nullptr) {
                    upperIndex = static_cast<int>(slot);
                    break;
                }
            }
        }
        const auto slot = static_cast<std::size_t>(upperIndex);
        std::string error;
        upperPanel_.closeTrack(slot, error);
        lowerPanel_.closeTrack(slot, error);
        error.clear();

        const bool upperWasEmpty = upperPanel_.openTrackCount() == 0;
        const bool lowerWasEmpty = lowerPanel_.openTrackCount() == 0;
        TrackOpenOptions options;
        options.rawPcmSampleRate = rawSampleRate_;
        options.channel = 0;
        bool opened = upperPanel_.openTrackAt(slot, path, type, options, error);
        if (opened) {
            options.channel = 1;
            opened = lowerPanel_.openTrackAt(slot, path, type, options, error);
        }
        if (!opened) {
            std::string ignored;
            upperPanel_.closeTrack(slot, ignored);
            lowerPanel_.closeTrack(slot, ignored);
            refreshSelection();
            showMessage("Open failed", "Cannot load file " + pathText(path) + "!\n" + error,
                        Gtk::MessageType::ERROR);
            return false;
        }
        for (const bool upper : {true, false}) {
            if (!(upper ? upperWasEmpty : lowerWasEmpty)) {
                continue;
            }
            const Limits limits = panel(upper).track(slot)->defaultLimits();
            if (limits.lower < limits.upper) {
                std::string ignored;
                panel(upper).setAmplitudeLimits(limits.lower, limits.upper, ignored);
            }
        }
        syncStereoState(true);
        selectTrack(true, slot);
        setStatus("Opened stereo " + pathText(path) + " in slot " + std::to_string(slot + 1) +
                  ": left channel up, right channel down.");
        return true;
    }

    /*****************************************************************************
     * setupFile
     *
     * Routes a file chosen for one panel (WinSap SetupFileUp / SetupFileDown):
     * unreadable WAV formats are refused, a stereo WAV is split over both
     * panels and the stereo rules decide where a mono file may go.
     *
     *****************************************************************************/
    void setupFile(bool upper, const std::filesystem::path& path, TrackType type)
    {
        const auto info = probeWavFile(path);
        if (info.has_value()) {
            const std::string formatError = wavFormatError(path, *info);
            if (!formatError.empty()) {
                showMessage("Unsupported Format", formatError, Gtk::MessageType::WARNING);
                return;
            }
            // WAV files are always binary - override parameter mode if selected
            if (type == TrackType::ParameterText) {
                type = TrackType::WaveformPcm16;
            }
            if (info->channels == 2U) {
                openStereoFile(path, -1, type);
                return;
            }
        } else if (isParameterFileName(path)) {
            type = TrackType::ParameterText;
        }
        if (!upper && stereoActive()) {
            showMessage("Open Down",
                        "The lower panel shows the right channels of the open stereo files.\n"
                        "Close the stereo files before opening a mono file in the lower panel.");
            return;
        }
        if (!allowMonoOpenWithStereo(type)) {
            return;
        }
        openTrackInPanel(upper, path, type);
    }

    /*****************************************************************************
     * openTrackInPanel
     *
     * Opens a file into the requested panel, replacing the last slot when the
     * panel is full. A candidate panel is committed only after the open
     * succeeds.
     *
     *****************************************************************************/
    bool openTrackInPanel(bool upper,
                          const std::filesystem::path& path,
                          TrackType type)
    {
        Panel& target = panel(upper);
        const bool wasEmpty = target.openTrackCount() == 0;
        TrackOpenOptions options;
        options.rawPcmSampleRate = rawSampleRate_;
        std::size_t slot = 0;
        std::string error;
        Panel candidate = target;
        if (candidate.openTrackCount() >= Panel::kMaxTracks) {
            slot = Panel::kMaxTracks - 1U;
            if (!candidate.closeTrack(slot, error)) {
                showMessage("Open failed", error, Gtk::MessageType::ERROR);
                return false;
            }
            options.slot = slot;
        }
        if (!candidate.openTrack(path, type, options, slot, error)) {
            showMessage("Open failed", error, Gtk::MessageType::ERROR);
            return false;
        }
        target = std::move(candidate);
        const Track* opened = target.track(slot);
        if (wasEmpty && opened != nullptr) {
            const Limits limits = opened->defaultLimits();
            std::string ignored;
            target.setAmplitudeLimits(limits.lower, limits.upper, ignored);
        }
        selectTrack(upper, slot);
        setStatus("Opened " + trackTypeText(type) + " " + fileNameText(path) + " in " +
                  (upper ? "upper" : "down") + " panel.");
        return true;
    }

    /*****************************************************************************
     * openStartupWaveforms
     *
     * Opens command-line waveforms alternately into upper and down panels.
     * Errors are accumulated so one bad startup file does not hide later ones.
     *
     *****************************************************************************/
    void openStartupWaveforms(const std::vector<std::filesystem::path>& startup)
    {
        std::vector<std::string> errors;
        for (std::size_t argumentIndex = 0; argumentIndex < startup.size(); ++argumentIndex) {
            const auto& path = startup[argumentIndex];
            const bool upper = (argumentIndex % 2U) == 0U;
            const std::size_t slot = argumentIndex / 2U;
            if (slot >= Panel::kMaxTracks) {
                errors.push_back("Ignored '" + pathText(path) + "': both panels are full.");
                continue;
            }
            const auto info = probeWavFile(path);
            if (info.has_value()) {
                const std::string formatError = wavFormatError(path, *info);
                if (!formatError.empty()) {
                    errors.push_back(formatError);
                    continue;
                }
                // A stereo file always opens with its left channel up and right channel down
                if (info->channels == 2U) {
                    openStereoFile(path, upper ? static_cast<int>(slot) : -1,
                                   TrackType::WaveformPcm16);
                    continue;
                }
            }
            if (!upper && stereoActive()) {
                errors.push_back("Ignored '" + pathText(path) +
                                 "': The lower panel shows the right channels of the open "
                                 "stereo files.\nClose the stereo files before opening a mono "
                                 "file in the lower panel.");
                continue;
            }
            const TrackType type = !info.has_value() && isParameterFileName(path)
                                       ? TrackType::ParameterText
                                       : TrackType::WaveformPcm16;
            Panel& target = panel(upper);
            const bool wasEmpty = target.openTrackCount() == 0;
            TrackOpenOptions options;
            options.rawPcmSampleRate = rawSampleRate_;
            options.slot = slot;
            std::size_t openedSlot = slot;
            std::string error;
            Panel candidate = target;
            if (candidate.track(slot) != nullptr && !candidate.closeTrack(slot, error)) {
                errors.push_back("Failed to replace '" + pathText(path) + "': " + error);
                continue;
            }
            if (!candidate.openTrack(path, type, options, openedSlot, error)) {
                errors.push_back("Failed to open '" + pathText(path) + "': " + error);
                continue;
            }
            target = std::move(candidate);
            pendingMark(upper).reset();
            if (wasEmpty) {
                const Track* opened = target.track(openedSlot);
                if (opened != nullptr) {
                    const Limits limits = opened->defaultLimits();
                    if (limits.lower < limits.upper) {
                        std::string ignored;
                        target.setAmplitudeLimits(limits.lower, limits.upper, ignored);
                    }
                }
            }
            selectTrack(upper, openedSlot);
        }
        if (!startup.empty() && errors.empty()) {
            setStatus("Startup waveforms opened.");
        } else if (!errors.empty()) {
            std::ostringstream message;
            for (std::size_t index = 0; index < errors.size(); ++index) {
                if (index != 0) {
                    message << '\n';
                }
                message << errors[index];
            }
            showMessage("Startup open results", message.str(), Gtk::MessageType::WARNING);
        }
    }

    /*****************************************************************************
     * addFilter
     *
     * Adds a named group of filename patterns to a file chooser. Grouping
     * filters keeps the open and save dialogs readable.
     *
     *****************************************************************************/
    Glib::RefPtr<Gtk::FileFilter> addFilter(Gtk::FileChooserDialog& dialog,
                                            const Glib::ustring& name,
                                            const std::vector<Glib::ustring>& patterns)
    {
        auto filter = Gtk::FileFilter::create();
        filter->set_name(name);
        for (const auto& pattern : patterns) {
            filter->add_pattern(pattern);
        }
        dialog.add_filter(filter);
        return filter;
    }

    /*****************************************************************************
     * openWithChooser
     *
     * Shows the open-file chooser and translates the selected mode into a track
     * type. Parameter mode and the *.par filter select each other, a .par file
     * always opens as parameters, and suspicious mode combinations ask for
     * confirmation.
     *
     *****************************************************************************/
    void openWithChooser(bool upper)
    {
        auto* dialog = new Gtk::FileChooserDialog(
            *window_, std::string("Open ") + (upper ? "upper" : "down") + " file",
            Gtk::FileChooser::Action::OPEN, true);
        dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        dialog->add_button("_Open", Gtk::ResponseType::ACCEPT);
        dialog->set_modal(true);
        // FileChooser choices report no change signal, so the mode is an
        // ordinary drop-down that can follow, and drive, the filter.
        constexpr guint kModeBinary = 0;
        constexpr guint kModeParameter = 1;
        constexpr guint kModeFft = 2;
        auto* modeRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        modeRow->set_margin(8);
        modeRow->append(*Gtk::make_managed<Gtk::Label>("Mode"));
        auto* modeChoice = Gtk::make_managed<Gtk::DropDown>(
            std::vector<Glib::ustring>{"Binary", "Parameter", "FFT Amplitude"});
        modeChoice->set_selected(kModeBinary);
        modeRow->append(*modeChoice);
        dialog->get_content_area()->append(*modeRow);
        const auto firstFilter = addFilter(*dialog, "WAV audio (*.wav)", {"*.wav", "*.WAV"});
        addFilter(*dialog, "Raw PCM (*.pcm, *.raw, *.dat, *.out)",
                  {"*.pcm", "*.PCM", "*.raw", "*.RAW", "*.dat", "*.DAT", "*.out", "*.OUT"});
        const auto parameterFilter =
            addFilter(*dialog, "Parameter (*.par)", {"*.par", "*.PAR"});
        addFilter(*dialog, "Residual, excitation, encoding (*.res, *.exc, *.cod)",
                  {"*.res", "*.RES", "*.exc", "*.EXC", "*.cod", "*.COD"});
        addFilter(*dialog, "All supported files",
                  {"*.wav", "*.WAV", "*.pcm", "*.PCM", "*.raw", "*.RAW", "*.dat", "*.DAT",
                   "*.out", "*.OUT", "*.par", "*.PAR", "*.res", "*.RES", "*.exc", "*.EXC",
                   "*.cod", "*.COD"});
        addFilter(*dialog, "All files", {"*"});
        dialog->set_filter(firstFilter);

        // Leaving the *.par filter drops Parameter mode, but a Binary or FFT
        // Amplitude choice is kept. Each handler only writes a value that
        // differs, so the pair settles after one round.
        dialog->property_filter().signal_changed().connect(
            [dialog, modeChoice, parameterFilter] {
                const bool parameterShown = dialog->get_filter() == parameterFilter;
                const guint mode = modeChoice->get_selected();
                if (parameterShown && mode != kModeParameter) {
                    modeChoice->set_selected(kModeParameter);
                } else if (!parameterShown && mode == kModeParameter) {
                    modeChoice->set_selected(kModeBinary);
                }
            });
        modeChoice->property_selected().signal_changed().connect(
            [dialog, modeChoice, parameterFilter, firstFilter] {
                const bool parameterShown = dialog->get_filter() == parameterFilter;
                const bool parameterMode = modeChoice->get_selected() == kModeParameter;
                if (parameterMode && !parameterShown) {
                    dialog->set_filter(parameterFilter);
                } else if (!parameterMode && parameterShown) {
                    dialog->set_filter(firstFilter);
                }
            });

        dialog->signal_response().connect([this, dialog, modeChoice, upper](int response) {
            if (response != Gtk::ResponseType::ACCEPT) {
                destroyDialogLater(dialog);
                return;
            }
            const auto file = dialog->get_file();
            if (!file) {
                destroyDialogLater(dialog);
                showMessage("Open failed", "No file was selected.", Gtk::MessageType::ERROR);
                return;
            }
            const std::filesystem::path path(file->get_path());
            const guint selectedMode = modeChoice->get_selected();
            destroyDialogLater(dialog);
            const std::string extension = lowerExtension(path);
            // A .par file opens as parameters whatever mode is shown, so it
            // can neither be read as raw binary nor raise a mismatch warning.
            const bool parameterFile = isParameterFileName(path);
            const std::string mode =
                parameterFile || selectedMode == kModeParameter
                    ? "parameter"
                    : (selectedMode == kModeFft ? "fft" : "binary");
            const bool parameterMismatch = mode == "parameter" && extension != ".par";
            const bool binaryExtension =
                extension == ".wav" || extension == ".pcm" ||
                extension == ".dat" || extension == ".raw" ||
                extension == ".exc" || extension == ".res" ||
                extension == ".cod";
            const bool binaryMismatch = mode != "parameter" && !binaryExtension;
            auto open = [this, upper, path, mode] {
                const TrackType type = mode == "parameter"
                                           ? TrackType::ParameterText
                                           : (mode == "fft" ? TrackType::FftAmplitude
                                                            : TrackType::WaveformPcm16);
                setupFile(upper, path, type);
            };

            const bool shouldWarn =
                (parameterMismatch && warnParameterExtensionMismatch_) ||
                (binaryMismatch && warnBinaryExtensionMismatch_);
            if (!shouldWarn) {
                open();
                return;
            }

            const bool parameterWarning = parameterMismatch;
            const std::string warning =
                parameterWarning
                    ? "You selected Parameter mode, but the file does not have a .par " "extension.\n\nContinue opening and stop showing this " "parameter-extension warning?"
                    : "You selected Binary or FFT Amplitude mode, but the file extension " "is not .wav, .pcm, .dat, .raw, .exc, .res, or .cod.\n\n" "Continue opening and stop showing this binary/spectrum-extension " "warning?";
            showExtensionWarning(
                "File Type Mismatch", warning,
                [this, parameterWarning, open = std::move(open)]() mutable {
                    if (parameterWarning) {
                        warnParameterExtensionMismatch_ = false;
                    } else {
                        warnBinaryExtensionMismatch_ = false;
                    }

                    std::string error;
                    if (!saveSettings(error, false, false, true, false)) {
                        showMessage("Warning preference not saved",
                                    "The file will still be opened, but Xsap could not save " "your warning preference: " + error,
                                    Gtk::MessageType::WARNING);
                    }
                    open();
                });
        });
        dialog->present();
    }

    /*****************************************************************************
     * saveMarkedBlock
     *
     * Prompts for a destination for the currently marked waveform range.
     * Existing files require confirmation to avoid silently overwriting data.
     *
     *****************************************************************************/
    void saveMarkedBlock()
    {
        bool upper = upperPanel_.selectedSampleRange().has_value();
        if (!upper && !lowerPanel_.selectedSampleRange().has_value()) {
            showMessage("Save failed",
                        "Mark a complete range in the upper or down panel first.",
                        Gtk::MessageType::WARNING);
            return;
        }
        if (!upper) {
            upper = false;
        }
        const auto reference = firstBinaryTrack(panel(upper));
        if (!reference.has_value()) {
            showMessage("Save failed", "Slot 1 is not an open binary track.", Gtk::MessageType::ERROR);
            return;
        }
        const auto range = panel(upper).selectedSampleRange();
        if (!range.has_value() || range->empty() ||
            range->end > static_cast<int64_t>(reference->track->sampleCount())) {
            showMessage("Save failed",
                        "The mark is empty or extends beyond slot 1.",
                        Gtk::MessageType::ERROR);
            return;
        }
        const std::size_t slot = reference->slot;
        auto* dialog = new Gtk::FileChooserDialog(
            *window_, "Save Marked Block As", Gtk::FileChooser::Action::SAVE, true);
        dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        dialog->add_button("_Save", Gtk::ResponseType::ACCEPT);
        dialog->set_modal(true);
        dialog->set_current_name("marked.wav");
        addFilter(*dialog, "Windows PCM files (*.wav)", {"*.wav", "*.WAV"});
        dialog->signal_response().connect([this, dialog, upper, slot](int response) {
            if (response != Gtk::ResponseType::ACCEPT) {
                destroyDialogLater(dialog);
                return;
            }
            const auto file = dialog->get_file();
            if (!file) {
                destroyDialogLater(dialog);
                showMessage("Save failed", "No destination was selected.", Gtk::MessageType::ERROR);
                return;
            }
            const std::filesystem::path path(file->get_path());
            destroyDialogLater(dialog);
            std::error_code existsError;
            if (std::filesystem::exists(path, existsError)) {
                if (existsError) {
                    showMessage("Save failed", "Cannot inspect destination: " + existsError.message(),
                                Gtk::MessageType::ERROR);
                    return;
                }
                showConfirm("Replace existing file?",
                            "Xsap will not overwrite a file without confirmation.",
                            [this, upper, slot, path] { writeMarkedWav(upper, slot, path); });
                return;
            }
            writeMarkedWav(upper, slot, path);
        });
        dialog->present();
    }

    /*****************************************************************************
     * writeMarkedWav
     *
     * Writes the marked samples as a mono 16-bit PCM wav file. The mark and
     * track are revalidated because the save dialog returns asynchronously.
     *
     *****************************************************************************/
    void writeMarkedWav(bool upper, std::size_t slot, const std::filesystem::path& path)
    {
        const Track* trackPtr = panel(upper).track(slot);
        const auto range = panel(upper).selectedSampleRange();
        if (trackPtr == nullptr || !trackPtr->isBinary() || !range.has_value() || range->empty() ||
            range->end > static_cast<int64_t>(trackPtr->sampleCount())) {
            showMessage("Save failed", "The selected track or mark is no longer valid.",
                        Gtk::MessageType::ERROR);
            return;
        }
        const std::vector<int16_t> samples = trackPtr->copySamples(*range);
        std::string error;
        if (!xsap::core::WriteWavFile(path.string(), samples.data(), samples.size(),
                                      trackPtr->sampleRate(), error)) {
            showMessage("Save failed", error, Gtk::MessageType::ERROR);
            return;
        }
        setStatus("Saved marked block to " + pathText(path) + ".");
    }

    /*****************************************************************************
     * showCloseFilesDialog
     *
     * Shows a two-column close-files dialog for the open tracks. Entries are
     * closed from the end so slot numbers remain stable during removal.
     *
     *****************************************************************************/
    void showCloseFilesDialog()
    {
        auto* dialog = new Gtk::Dialog("Close Files", *window_, true, true);
        dialog->set_default_size(560, 460);
        dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        dialog->add_button("_Close Selected", Gtk::ResponseType::ACCEPT);
        auto content = dialog->get_content_area();
        auto intro = Gtk::make_managed<Gtk::Label>(
            "Select one or more tracks from either panel. The lists mirror the WinSap close-files dialog.");
        intro->set_wrap(true);
        intro->set_xalign(0.0F);
        content->append(*intro);
        auto columns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        columns->set_margin(12);
        content->append(*columns);
        auto entries = std::make_shared<std::vector<CloseEntry>>();
        auto addPanel = [this, columns, entries](bool upper) {
            auto frame = Gtk::make_managed<Gtk::Frame>(upper ? "Upper files" : "Down files");
            auto list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            list->set_margin(8);
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                const Track* trackPtr = panel(upper).track(slot);
                if (trackPtr == nullptr) {
                    continue;
                }
                auto check = Gtk::make_managed<Gtk::CheckButton>(
                    std::to_string(slot + 1) + "  " + trackTypeText(trackPtr->type()) + "  " +
                    fileNameText(trackPtr->path()));
                check->set_hexpand(true);
                list->append(*check);
                entries->push_back(CloseEntry{upper, slot, check});
            }
            if (list->get_first_child() == nullptr) {
                auto empty = Gtk::make_managed<Gtk::Label>("No open tracks");
                empty->set_xalign(0.0F);
                list->append(*empty);
            }
            frame->set_child(*list);
            frame->set_hexpand(true);
            columns->append(*frame);
        };
        addPanel(true);
        addPanel(false);
        dialog->signal_response().connect([this, dialog, entries](int response) {
            if (response == Gtk::ResponseType::ACCEPT) {
                for (auto it = entries->rbegin(); it != entries->rend(); ++it) {
                    if (!it->check->get_active()) {
                        continue;
                    }
                    std::string error;
                    panel(it->upper).closeTrack(it->slot, error);
                }
                syncPanelActivity();
                setStatus("Closed selected files.");
                refreshSelection();
            }
            destroyDialogLater(dialog);
        });
        dialog->present();
    }

    /*****************************************************************************
     * showHelpTopics
     *
     * WinSap's Help Topics page, reworded for Linux, in the light colours
     * of the plot window.
     *
     *****************************************************************************/
    void showHelpTopics()
    {
        static const char* const Markup =
            "# Xsap Help\n"
            "Xsap is a speech and audio analysis program. It shows audio as waveforms, spectra and spectrograms "
            "in two panels, **Up** and **Down**, each holding several files on a shared timeline, so recordings "
            "can be compared frame by frame.\n"

            "## Opening files\n"
            "Choose **Open Up** (F3) or **Open Down** (F4), pick a file and a mode:\n"
            "- **Binary** - audio: WAV, or raw 16-bit PCM (.pcm, .dat, .raw, .exc, .res, .cod), drawn as a waveform.\n"
            "- **Parameter** - a .par text file of parameters, drawn as curves.\n"
            "- **FFT Amplitude** - audio drawn as a spectrum or spectrogram.\n"
            "WAV files may be 8, 16, 24 or 32-bit integer PCM or 32/64-bit float, mono or stereo. "
            "Raw PCM uses the rate set with **Options > Set Raw Samplingrate**.\n"
            "The first file in a panel opens in View All. Every file is drawn against its panel's timeline, so a "
            "sound at frame 150 stays at frame 150 whatever else is open. Files can also be given on the command "
            "line, or sent by XPlayer's **Open in XSap**; a second Xsap hands its files to the running one.\n"
            "### Stereo files\n"
            "A stereo WAV opens its left channel in the upper panel and its right channel in the lower one, "
            "labelled [L] and [R]. The two panels then share frames, zoom and marks, and closing either "
            "channel closes both.\n"
            "> A stereo file in FFT Amplitude mode must be the only open file. While a stereo file is open in "
            "Binary mode, no file can use FFT Amplitude mode, and mono files cannot open in the lower panel.\n"

            "## Moving through a file\n"
            "The keys move the upper panel when it has files, otherwise the lower one. With **Lock Starting "
            "Frame** (Ctrl+L) on, the lower panel follows the upper panel's start.\n"
            "Left arrow / Right arrow\tOne frame back or forward\n"
            "Up arrow / Down arrow\tOne screen back or forward\n"
            "Page Up / Page Down\tBack or forward by the frame skip\n"
            "Home / End\tStart or end of the longest file\n"
            "The **Up** and **Down** menus set each panel's Frame Length, Goto Frame, Number of Frames (the frames "
            "on screen), amplitude Limit and Page Numbers.\n"

            "## Marking and saving\n"
            "Right-click a waveform twice to mark a range; right-click a third time to clear it. **Save As** (F2) "
            "writes the marked range of the first file in the upper panel as a WAV file, or of the lower panel "
            "when the upper one has no mark. **Look Marked Block** (Ctrl+F4) fills the screen with the mark.\n"

            "## Playing\n"
            "- **Play All** (F5) plays every open audio file, upper panel first.\n"
            "- **Play Up** (F6) and **Play Down** (F7) play the marked range of the first file in that panel.\n"
            "- Click a file name to play the whole file. A stereo file always plays in stereo.\n"
            "- Right-click a file name for **Close file** and **Play marked area**; a stereo file adds "
            "**Play this channel** and **Play marked area in this channel**.\n"
            "- **Stop** (F8), or a right-click, stops playing and clears the queue.\n"
            "A cursor follows playback, over the spectrogram too. Sound goes through PulseAudio (PipeWire), or ALSA "
            "when PulseAudio is not available.\n"

            "## Spectrum and spectrogram\n"
            "In FFT Amplitude mode, a view of 1024 samples or fewer is a line spectrum, level against frequency. "
            "Raising Number of Frames lengthens the analysis window for finer frequency detail. A longer view "
            "becomes a spectrogram, frequency against time, with a mark shown as white lines at its edges.\n"
            "The status bar reads (sample, amplitude) on a waveform, (Hz, dB) on a spectrum and (time, Hz) on a "
            "spectrogram. The contrast and colour map are remembered for the next start.\n"

            "## Shortcut keys\n"
            "### Files\n"
            "F3\tOpen Up\n"
            "F4\tOpen Down\n"
            "F2\tSave the marked range as\n"
            "F9\tClose files...\n"
            "Ctrl+W\tClose all\n"
            "Ctrl+A\tReload all\n"
            "Alt+X\tExit\n"
            "### Playing\n"
            "F5\tPlay all\n"
            "F6\tPlay Up\n"
            "F7\tPlay Down\n"
            "F8\tStop\n"
            "### View\n"
            "Ctrl+F7\tZoom in\n"
            "Ctrl+F8\tZoom out\n"
            "Ctrl+F9\tView all\n"
            "Ctrl+F4\tLook at the marked block\n"
            "Ctrl+F5\tExpand the amplitude\n"
            "Ctrl+F6\tCompress the amplitude\n"
            "Ctrl+F2\tAdd Down to Up: overlay the lower waveform in red\n"
            "Ctrl+F10\tSync panels: copy frame length, count and start\n"
            "Ctrl+L\tLock starting frame\n"
            "### Spectrogram\n"
            "Ctrl+U\tContrast up (narrower dB range)\n"
            "Ctrl+D\tContrast down (wider dB range)\n"
            "Ctrl+P\tNext colour map: Viridis, Inferno, Jet, Grey\n"
            "### Help\n"
            "F1\tHelp Topics\n"
            "Esc\tClose the Help Topics window\n"
            "Help > About Xsap\tThe version\n"

            "## Settings\n"
            "The raw sampling rate (with Options > Save Raw Samplingrate), spectrogram contrast, colour map, "
            "window position and open-file warnings are kept in ~/.config/xsap/xsap.conf.\n";
        const wavedsp::HelpTheme theme{wavedsp::HelpColor("#ffffff"), wavedsp::HelpColor("#202020"),
                                       wavedsp::HelpColor("#005493"), wavedsp::HelpColor("#0066b4"),
                                       wavedsp::HelpColor("#6e6e6e"), wavedsp::HelpColor("#f0f4f9"), false};
        wavedsp::ShowHelp(*window_, "Xsap Help Topics", Markup, theme, "xsap");
    }

    /*****************************************************************************
     * showAboutDialog
     *
     * Displays the About dialog with the bundled logo when available. Theme
     * icons provide a harmless fallback for incomplete installs.
     *
     *****************************************************************************/
    void showAboutDialog()
    {
        auto* dialog = new Gtk::Dialog("About Xsap", *window_, true, true);
        dialog->set_default_size(500, 260);
        dialog->add_button("_OK", Gtk::ResponseType::OK);

        auto content = dialog->get_content_area();
        content->set_margin(20);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        row->set_valign(Gtk::Align::START);
        content->append(*row);

        const auto logoPath = assetDirectory() / "xsap.png";
        if (std::filesystem::is_regular_file(logoPath)) {
            try {
                auto logo = Gdk::Pixbuf::create_from_file(logoPath.u8string());
                auto image = Gtk::make_managed<Gtk::Image>(logo);
                image->set_pixel_size(kAboutIconSize);
                image->set_valign(Gtk::Align::START);
                row->append(*image);
            } catch (const Glib::Error&) {
                auto fallback = Gtk::make_managed<Gtk::Image>();
                fallback->set_from_icon_name("audio-x-generic-symbolic");
                fallback->set_pixel_size(kAboutIconSize);
                fallback->set_valign(Gtk::Align::START);
                row->append(*fallback);
            }
        }

        auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        text->set_valign(Gtk::Align::START);
        auto name = Gtk::make_managed<Gtk::Label>();
        name->set_markup("<span size=\"xx-large\" weight=\"bold\">Xsap</span>");
        name->set_xalign(0.0F);
        text->append(*name);

        auto description = Gtk::make_managed<Gtk::Label>("Speech Analysis Program (SAP)");
        description->set_xalign(0.0F);
        text->append(*description);

        auto version = Gtk::make_managed<Gtk::Label>(std::string("Version ") + kVersion);
        version->set_xalign(0.0F);
        text->append(*version);

        auto copyright = Gtk::make_managed<Gtk::Label>("Copyright 2026 Tian Wang");
        copyright->set_margin_top(12);
        copyright->set_xalign(0.0F);
        text->append(*copyright);
        row->append(*text);

        dialog->signal_response().connect([dialog](int) {
            destroyDialogLater(dialog);
        });
        dialog->present();
    }

    /*****************************************************************************
     * showMessage
     *
     * Shows a message dialog from the GTK idle loop. Scheduling it avoids
     * opening modal UI from callbacks that may still be unwinding.
     *
     *****************************************************************************/
    void showMessage(const std::string& title,
                     const std::string& body,
                     Gtk::MessageType type = Gtk::MessageType::INFO)
    {
        Glib::signal_idle().connect_once([this, title, body, type]() {
            if (!window_) {
                return;
            }
            auto* dialog =
                new Gtk::MessageDialog(*window_, body, false, type, Gtk::ButtonsType::OK, true);
            dialog->set_title(title);
            dialog->signal_response().connect([dialog](int) {
                destroyDialogLater(dialog);
            });
            dialog->present();
        });
    }

    /*****************************************************************************
     * showConfirm
     *
     * Shows a yes-no dialog and runs the accepted (or optional declined) action
     * later on the idle loop. The extra idle hop keeps dialog destruction
     * separate from follow-up UI.
     *
     *****************************************************************************/
    void showConfirm(const std::string& title,
                     const std::string& body,
                     std::function<void()> yes,
                     std::function<void()> no = {})
    {
        Glib::signal_idle().connect_once(
            [this, title, body, yes = std::move(yes), no = std::move(no)]() mutable {
                if (!window_) {
                    return;
                }
                auto* dialog = new Gtk::MessageDialog(
                    *window_, body, false, Gtk::MessageType::QUESTION,
                    Gtk::ButtonsType::YES_NO, true);
                dialog->set_title(title);
                dialog->signal_response().connect(
                    [dialog, yes = std::move(yes), no = std::move(no)](int response) mutable {
                        destroyDialogLater(dialog);
                        if (response == Gtk::ResponseType::YES) {
                            Glib::signal_idle().connect_once(std::move(yes));
                        } else if (no) {
                            Glib::signal_idle().connect_once(std::move(no));
                        }
                    });
                dialog->present();
            });
    }

    /*****************************************************************************
     * showExtensionWarning
     *
     * Uses explicit Continue and Cancel buttons for file-mode mismatches.
     * Continuing persists the caller's warning preference before opening.
     *
     *****************************************************************************/
    void showExtensionWarning(const std::string& title,
                              const std::string& body,
                              std::function<void()> continueOpen)
    {
        Glib::signal_idle().connect_once(
            [this, title, body, continueOpen = std::move(continueOpen)]() mutable {
                if (!window_) {
                    return;
                }
                auto* dialog = new Gtk::MessageDialog(
                    *window_, body, false, Gtk::MessageType::WARNING,
                    Gtk::ButtonsType::NONE, true);
                dialog->set_title(title);
                dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
                dialog->add_button("_Continue", Gtk::ResponseType::ACCEPT);
                dialog->set_default_response(Gtk::ResponseType::CANCEL);
                dialog->signal_response().connect(
                    [dialog, continueOpen = std::move(continueOpen)](int response) mutable {
                        destroyDialogLater(dialog);
                        if (response == Gtk::ResponseType::ACCEPT) {
                            Glib::signal_idle().connect_once(std::move(continueOpen));
                        }
                    });
                dialog->present();
            });
    }

    /*****************************************************************************
     * showInput
     *
     * Shows a small prompt dialog and lets the caller validate the entered
     * text. Rejected input leaves the dialog open so the user can correct it.
     *
     *****************************************************************************/
    void showInput(const std::string& title,
                   const std::string& prompt,
                   const std::string& initial,
                   std::function<std::optional<std::string>(const std::string&)> accept)
    {
        auto* dialog = new Gtk::Dialog(title, *window_, true, true);
        dialog->set_default_size(500, 170);
        dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        dialog->add_button("_OK", Gtk::ResponseType::ACCEPT);
        auto box = dialog->get_content_area();
        box->set_spacing(8);
        box->set_margin(16);
        auto label = Gtk::make_managed<Gtk::Label>(prompt);
        label->set_wrap(true);
        label->set_xalign(0.0F);
        box->append(*label);
        auto entry = Gtk::make_managed<Gtk::Entry>();
        entry->set_text(initial);
        entry->set_activates_default(true);
        box->append(*entry);
        dialog->set_default_response(Gtk::ResponseType::ACCEPT);
        dialog->signal_response().connect([this, dialog, entry, accept = std::move(accept)](
                                              int response) mutable {
            if (response != Gtk::ResponseType::ACCEPT) {
                destroyDialogLater(dialog);
                return;
            }
            const auto error = accept(entry->get_text());
            if (error.has_value()) {
                showMessage("Input rejected", *error, Gtk::MessageType::WARNING);
                return;
            }
            destroyDialogLater(dialog);
        });
        dialog->present();
        entry->grab_focus();
        entry->select_region(0, -1);
    }

    /*****************************************************************************
     * showFrameLength
     *
     * Prompts for a panel frame length and applies it to that panel. Validation
     * remains in the panel model so UI and core rules stay aligned.
     *
     *****************************************************************************/
    void showFrameLength(bool upper)
    {
        if (panel(upper).openTrackCount() == 0) {
            return;
        }
        showInput(std::string(upper ? "Upper" : "Down") + " frame length",
                  "Frame length in samples (valid range: 1 or greater):",
                  std::to_string(panel(upper).frameLength()),
                  [this, upper](const std::string& text) {
                      std::string error;
                      const auto value = parseSize(text, error);
                      if (!value.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      if (!panel(upper).setFrameLength(*value, error)) {
                          return std::optional<std::string>(error);
                      }
                      setStatus(std::string(upper ? "Upper" : "Down") + " frame length updated.");
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * showGoto
     *
     * Prompts for a frame index and moves the requested panel there. Upper-
     * panel moves can also move down when starting-frame lock is enabled.
     *
     *****************************************************************************/
    void showGoto(bool upper)
    {
        if (panel(upper).openTrackCount() == 0) {
            return;
        }
        showInput(std::string(upper ? "Upper" : "Down") + " goto frame",
                  "Frame index:",
                  std::to_string(panel(upper).frameBegin()),
                  [this, upper](const std::string& text) {
                      std::string error;
                      const auto value = parseSize(text, error);
                      if (!value.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      if (!applyFrameOperation([value](Panel& p, std::string& e) {
                              return p.gotoFrame(*value, e);
                          },
                                               upper, error)) {
                          return std::optional<std::string>(error);
                      }
                      setStatus("Frame position updated.");
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * showVisibleFrames
     *
     * Prompts for the number of frames to display in one panel. The view is
     * redrawn only after the panel accepts the new count.
     *
     *****************************************************************************/
    void showVisibleFrames(bool upper)
    {
        if (panel(upper).openTrackCount() == 0) {
            return;
        }
        showInput(std::string(upper ? "Upper" : "Down") + " visible frames",
                  "Number of frames to display (valid range: 1 or greater):",
                  std::to_string(panel(upper).visibleFrameCount()),
                  [this, upper](const std::string& text) {
                      std::string error;
                      const auto value = parseSize(text, error);
                      if (!value.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      if (!panel(upper).setVisibleFrameCount(*value, error)) {
                          return std::optional<std::string>(error);
                      }
                      setStatus("Visible frame count updated.");
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * showFrameSkip
     *
     * Prompts for the PageUp and PageDown jump size. Zero and negative values
     * are rejected before they can turn navigation into a no-op.
     *
     *****************************************************************************/
    void showFrameSkip(bool upper)
    {
        if (panel(upper).openTrackCount() == 0) {
            return;
        }
        showInput(std::string(upper ? "Upper" : "Down") + " Page Numbers",
                  "Input number of frames to jump when using PageUp/PageDown (>0):",
                  std::to_string(panel(upper).frameSkip()),
                  [this, upper](const std::string& text) {
                      const std::string input = trimCopy(text);
                      if (!input.empty() && input.front() == '-') {
                          return std::optional<std::string>(
                              "Page size must be greater than zero.");
                      }
                      std::string error;
                      const auto value = parseSize(input, error);
                      if (!value.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      if (*value == 0U) {
                          return std::optional<std::string>(
                              "Page size must be greater than zero.");
                      }
                      if (!panel(upper).setFrameSkip(*value, error)) {
                          return std::optional<std::string>(error);
                      }
                      setStatus(std::string(upper ? "Upper" : "Down") +
                                " PageUp/PageDown jump updated.");
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * showLimits
     *
     * Prompts for lower and upper amplitude limits for one panel. The current
     * limits seed the dialog so small adjustments are easy.
     *
     *****************************************************************************/
    void showLimits(bool upper)
    {
        if (panel(upper).openTrackCount() == 0) {
            return;
        }
        std::ostringstream initial;
        initial << panel(upper).lowerLimit() << ' ' << panel(upper).upperLimit();
        showInput(std::string(upper ? "Upper" : "Down") + " amplitude limits",
                  "Enter lower and upper limits:",
                  initial.str(),
                  [this, upper](const std::string& text) {
                      std::string error;
                      const auto limits = parseLimits(text, error);
                      if (!limits.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      if (!panel(upper).setAmplitudeLimits(limits->lower, limits->upper, error)) {
                          return std::optional<std::string>(error);
                      }
                      setStatus("Amplitude limits updated.");
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * showRawSampleRate
     *
     * Prompts for the default sample rate used by raw PCM tracks. Open raw
     * tracks are updated immediately while WAV metadata remains authoritative.
     *
     *****************************************************************************/
    void showRawSampleRate()
    {
        showInput("Set Raw Samplingrate",
                  "Sampling rate for raw PCM streams (Hz):",
                  std::to_string(rawSampleRate_),
                  [this](const std::string& text) {
                      std::string error;
                      const auto value = parseRate(text, error);
                      if (!value.has_value()) {
                          return std::optional<std::string>(error);
                      }
                      rawSampleRate_ = *value;
                      for (Panel* target : {&upperPanel_, &lowerPanel_}) {
                          for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                              Track* trackPtr = target->track(slot);
                              if (trackPtr != nullptr && trackPtr->isBinary() &&
                                  !trackPtr->isWav()) {
                                  if (!trackPtr->setRawPcmSampleRate(*value, error)) {
                                      return std::optional<std::string>(error);
                                  }
                              }
                          }
                      }
                      setStatus("Raw PCM sample rate updated.");
                      // The cached images are keyed on track and view, not on
                      // the rate, and a rebuilt analysis can reach the same
                      // generation number, so they would otherwise be reused.
                      spectrogramSurfaces_.clear();
                      redraw();
                      return std::optional<std::string>();
                  });
    }

    /*****************************************************************************
     * applyFrameOperation
     *
     * Applies navigation to the requested panel. With starting-frame lock,
     * upper navigation then assigns its resulting start to down, clamped to the
     * down panel's valid range. Down navigation never drives upper.
     *
     *****************************************************************************/
    bool applyFrameOperation(const std::function<bool(Panel&, std::string&)>& operation,
                             bool requestedUpper,
                             std::string& error)
    {
        if (!requestedUpper) {
            setActivePanel(false);
            return operation(lowerPanel_, error);
        }

        Panel upperCopy = upperPanel_;
        Panel downCopy = lowerPanel_;
        if (upperCopy.openTrackCount() != 0 && !operation(upperCopy, error)) {
            return false;
        }
        if (lockStarts_ && downCopy.openTrackCount() != 0 &&
            !downCopy.setFrameBeginFrom(upperCopy, error)) {
            return false;
        }
        upperPanel_ = std::move(upperCopy);
        if (lockStarts_) {
            lowerPanel_ = std::move(downCopy);
        }
        setActivePanel(true);
        return true;
    }

    /*****************************************************************************
     * navigate
     *
     * Runs a frame-navigation command through the locked-panel rules. Errors
     * are shown as warnings while successful moves update status and repaint.
     *
     *****************************************************************************/
    void navigate(const std::function<bool(Panel&, std::string&)>& operation,
                  const std::string& success)
    {
        std::string error;
        const bool useUpper = upperPanel_.openTrackCount() != 0;
        if (!applyFrameOperation(operation, useUpper, error)) {
            showMessage("Navigation failed", error, Gtk::MessageType::WARNING);
            return;
        }
        setStatus(success);
        redraw();
    }

    /*****************************************************************************
     * navigateAll
     *
     * Applies a view operation to both panels as a single commit. Copying both
     * panels first prevents half-applied zoom or view-all changes.
     *
     *****************************************************************************/
    void navigateAll(const std::function<bool(Panel&, std::string&)>& operation,
                     const std::string& success)
    {
        Panel upperCopy = upperPanel_;
        Panel downCopy = lowerPanel_;
        std::string error;
        if (upperCopy.openTrackCount() != 0 && !operation(upperCopy, error)) {
            showMessage("View operation failed", error, Gtk::MessageType::WARNING);
            return;
        }
        if (downCopy.openTrackCount() != 0 && !operation(downCopy, error)) {
            showMessage("View operation failed", error, Gtk::MessageType::WARNING);
            return;
        }
        upperPanel_ = std::move(upperCopy);
        lowerPanel_ = std::move(downCopy);
        setStatus(success);
        redraw();
    }

    /*****************************************************************************
     * changeAmplitudeAll
     *
     * Applies amplitude scaling to every panel that actually contains
     * waveforms. Non-waveform panels are ignored because their limits have
     * different meanings.
     *
     *****************************************************************************/
    void changeAmplitudeAll(const std::function<bool(Panel&, std::string&)>& operation,
                            const std::string& success)
    {
        Panel upperCopy = upperPanel_;
        Panel downCopy = lowerPanel_;
        std::string error;
        if (panelHasWaveform(upperCopy) && !operation(upperCopy, error)) {
            showMessage("View operation failed", error, Gtk::MessageType::WARNING);
            return;
        }
        if (panelHasWaveform(downCopy) && !operation(downCopy, error)) {
            showMessage("View operation failed", error, Gtk::MessageType::WARNING);
            return;
        }
        upperPanel_ = std::move(upperCopy);
        lowerPanel_ = std::move(downCopy);
        setStatus(success);
        redraw();
    }

    /*****************************************************************************
     * lookMarkedBlock
     *
     * Zooms the active panel to the selected mark. The visible frame count is
     * at least one so a very short mark still produces a usable view.
     *
     *****************************************************************************/
    void lookMarkedBlock()
    {
        const auto range = activePanel().selectedSampleRange();
        if (!range.has_value() || range->empty()) {
            showMessage("Marked block", "Set a complete mark before looking at it.",
                        Gtk::MessageType::WARNING);
            return;
        }
        const Panel& source = activePanel();
        if (source.frameLength() == 0U) {
            return;
        }
        const std::size_t begin = static_cast<std::size_t>(std::max<int64_t>(0, range->begin)) /
                                  source.frameLength();
        const std::size_t count =
            std::max<std::size_t>(1U, static_cast<std::size_t>(range->length()) /
                                          source.frameLength());
        std::string error;
        if (!activePanel().setFrameBegin(begin, error) ||
            !activePanel().setVisibleFrameCount(count, error)) {
            showMessage("Marked block", error, Gtk::MessageType::WARNING);
            return;
        }
        setStatus("Showing marked block.");

        // Lock Starting Frame keeps the down panel on the same frame, so a jump
        // to a marked block has to carry it along too. The lock only ever pulls
        // the down panel after the up panel, never the other way round.
        if (lockStarts_ && activeUpper_ && lowerPanel_.openTrackCount() != 0) {
            std::string lockError;
            lowerPanel_.setFrameBeginFrom(upperPanel_, lockError);
        }

        redraw();
    }

    /*****************************************************************************
     * syncPanels
     *
     * Gives the down panel the same view as the up panel: frame length first,
     * then the frame count and starting frame. Unlike the starting-frame lock
     * this is a one-off alignment of all three settings.
     *
     *****************************************************************************/
    void syncPanels()
    {
        if (upperPanel_.openTrackCount() == 0 || lowerPanel_.openTrackCount() == 0) {
            showMessage("Sync panels", "Both panels need an open file before they can be synced.",
                        Gtk::MessageType::WARNING);
            return;
        }

        Panel downCopy = lowerPanel_;
        std::string error;

        // Frame length first: it decides how many frames a file holds, so the
        // frame window has to be applied against the new length.
        if (upperPanel_.frameLength() != 0U &&
            downCopy.frameLength() != upperPanel_.frameLength() &&
            !downCopy.setFrameLength(upperPanel_.frameLength(), error)) {
            showMessage("Sync panels", error, Gtk::MessageType::WARNING);
            return;
        }
        if (!downCopy.setVisibleFrameCount(upperPanel_.visibleFrameCount(), error) ||
            !downCopy.setFrameBegin(upperPanel_.frameBegin(), error)) {
            showMessage("Sync panels", error, Gtk::MessageType::WARNING);
            return;
        }

        lowerPanel_ = std::move(downCopy);
        setStatus("Down panel synced to up panel.");
        redraw();
    }

    /*****************************************************************************
     * reloadPanels
     *
     * Reloads every open track in both panels. Unavailable files retain their
     * in-memory data; readable files that fail validation are removed.
     *
     *****************************************************************************/
    void reloadPanels()
    {
        std::vector<std::string> preservedFailures;
        std::vector<std::string> emptiedFailures;
        const std::size_t reloaded =
            upperPanel_.reloadAll(preservedFailures, emptiedFailures) +
            lowerPanel_.reloadAll(preservedFailures, emptiedFailures);
        if (upperPanel_.openTrackCount() == 0) {
            pendingUpperMark_.reset();
        }
        if (lowerPanel_.openTrackCount() == 0) {
            pendingLowerMark_.reset();
        }
        refreshSelection();

        if (preservedFailures.empty() && emptiedFailures.empty()) {
            setStatus("Reloaded " + std::to_string(reloaded) + " file(s).");
        } else {
            std::string detail;
            if (!preservedFailures.empty()) {
                detail += "Unavailable files kept in memory:\n";
                for (const std::string& failure : preservedFailures) {
                    detail += failure + "\n";
                }
            }
            if (!emptiedFailures.empty()) {
                detail += "\nReadable files that failed validation; slots emptied:\n";
                for (const std::string& failure : emptiedFailures) {
                    detail += failure + "\n";
                }
            }
            showMessage("Reload All",
                        detail,
                        Gtk::MessageType::WARNING);
            setStatus("Reloaded " + std::to_string(reloaded) + " file(s), " +
                      std::to_string(preservedFailures.size()) + " kept, " +
                      std::to_string(emptiedFailures.size()) + " emptied.");
        }
        redraw();
    }

    /*****************************************************************************
     * playbackRange
     *
     * Chooses the marked range for playback or the whole track when no mark
     * exists. The range is checked against the source track before audio is
     * copied.
     *
     *****************************************************************************/
    std::optional<SampleRange> playbackRange(const Panel& source,
                                             const Track& trackPtr,
                                             std::string& error) const
    {
        if (source.selectedSampleRange().has_value()) {
            const SampleRange range = *source.selectedSampleRange();
            if (range.empty()) {
                error = "The selected mark is empty.";
                return std::nullopt;
            }
            if (range.end > static_cast<int64_t>(trackPtr.sampleCount())) {
                error = "The mark extends beyond the selected playback track.";
                return std::nullopt;
            }
            return range;
        }
        if (trackPtr.sampleCount() == 0U) {
            error = "The selected track contains no samples.";
            return std::nullopt;
        }
        return SampleRange{0, static_cast<int64_t>(trackPtr.sampleCount())};
    }

    /*****************************************************************************
     * stopPlayback
     *
     * Stops audio, clears queued playback and removes playback cursors. The
     * user-facing status differs between explicit stops and internal cleanup.
     *
     *****************************************************************************/
    void stopPlayback(bool user)
    {
        playbackStoppedByUser_ = user;
        playbackCommandFeedback_.clear();
        if (audioPlayer_.isPlaying()) {
            audioPlayer_.stop();
        }
        playbackJob_ = PlaybackJob{};
        playbackQueue_.clear();
        playbackQueueIndex_ = 0;
        playbackSamples_.store(0, std::memory_order_release);
        playbackNotice_.store(0, std::memory_order_release);
        handledPlaybackNotice_ = 0;
        handledPlaybackSamples_ = 0;
        upperPanel_.clearPlaybackCursor();
        lowerPanel_.clearPlaybackCursor();
        if (user) {
            setStatus("Playback stopped.");
        } else if (application_) {
            redraw();
        }
    }

    /*****************************************************************************
     * launchPlayback
     *
     * Starts one queued playback item and connects audio progress to the GTK
     * dispatcher. The audio callback stores atomics only and lets the UI thread
     * draw.
     *
     *****************************************************************************/
    bool launchPlayback(QueuedPlayback item)
    {
        playbackCommandFeedback_.clear();
        playbackJob_ =
            PlaybackJob{true, item.sampleRate, std::move(item.segments), item.description};
        playbackSamples_.store(0, std::memory_order_release);
        playbackNotice_.store(0, std::memory_order_release);
        handledPlaybackNotice_ = 0;
        handledPlaybackSamples_ = 0;
        auto callback = [this](std::size_t samplesPlayed, AudioPlayer::PlaybackEvent event) {
            playbackSamples_.store(samplesPlayed, std::memory_order_release);
            int notice = 1;
            switch (event) {
            case AudioPlayer::PlaybackEvent::Progress:
                notice = 1;
                break;
            case AudioPlayer::PlaybackEvent::Completed:
                notice = 2;
                break;
            case AudioPlayer::PlaybackEvent::Cancelled:
                notice = 3;
                break;
            case AudioPlayer::PlaybackEvent::Failed:
                notice = 4;
                break;
            }
            playbackNotice_.store(notice, std::memory_order_release);
            playbackDispatcher_.emit();
        };
        if (!audioPlayer_.play(item.samples, item.sampleRate, std::move(callback),
                               item.rightSamples.empty() ? nullptr : &item.rightSamples)) {
            playbackJob_ = PlaybackJob{};
            showMessage("Playback failed", audioPlayer_.lastError(), Gtk::MessageType::ERROR);
            return false;
        }
        setStatus(playbackJob_.description);
        return true;
    }

    /*****************************************************************************
     * startPlaybackQueue
     *
     * Stops current audio and starts a queue of whole-track playback items.
     * Empty queues report a warning rather than silently doing nothing.
     *
     *****************************************************************************/
    bool startPlaybackQueue(std::vector<QueuedPlayback> queue)
    {
        stopPlayback(false);
        if (queue.empty()) {
            showMessage("Playback failed", "No playable waveform tracks are open.",
                        Gtk::MessageType::WARNING);
            return false;
        }
        playbackQueue_ = std::move(queue);
        playbackQueueIndex_ = 0;
        return launchPlayback(std::move(playbackQueue_.front()));
    }

    /*****************************************************************************
     * rangePlayback
     *
     * Builds a queued playback item for part of one waveform or FFT amplitude
     * track. A stereo item plays the upper slot as the left channel and the
     * lower slot as the right one. Segment metadata lets the UI place the
     * playback cursor back in the source panel.
     *
     *****************************************************************************/
    std::optional<QueuedPlayback> rangePlayback(bool upper,
                                                std::size_t slot,
                                                SampleRange range,
                                                bool stereo,
                                                std::string& error) const
    {
        const Track* trackPtr = panel(upper).track(slot);
        if (trackPtr == nullptr || !trackPtr->isBinary()) {
            error = "Only waveform and FFT amplitude tracks can be played.";
            return std::nullopt;
        }
        const Track* rightTrack = nullptr;
        if (stereo) {
            if (!isStereoPairSlot(slot) ||
                upperPanel_.track(slot)->sampleCount() != lowerPanel_.track(slot)->sampleCount()) {
                error = "Both channels of the stereo file must be open to play it in stereo.";
                return std::nullopt;
            }
            trackPtr = upperPanel_.track(slot);
            rightTrack = lowerPanel_.track(slot);
        }
        range.end = std::min(range.end, static_cast<int64_t>(trackPtr->sampleCount()));
        if (range.begin < 0 || range.empty()) {
            error = "The selected track contains no samples.";
            return std::nullopt;
        }
        QueuedPlayback item;
        item.samples = trackPtr->copySamples(range);
        if (rightTrack != nullptr) {
            item.rightSamples = rightTrack->copySamples(range);
        }
        item.sampleRate = trackPtr->effectiveSampleRate(rawSampleRate_);
        item.segments.push_back(
            PlaybackSegment{upper, range.begin, item.samples.size(), 0, stereo});
        std::ostringstream description;
        description << "Playing ";
        if (stereo) {
            description << "stereo slot ";
        } else {
            description << (upper ? "upper" : "down") << " slot ";
        }
        description << slot + 1 << " (" << fileNameText(trackPtr->path()) << ") at "
                    << item.sampleRate << " Hz.";
        item.description = description.str();
        return item;
    }

    /*****************************************************************************
     * playTrackRange
     *
     * Plays part of one track, reporting a failure to build the item. Label
     * clicks, the file menu and Play Up/Down all come through here.
     *
     *****************************************************************************/
    bool playTrackRange(bool upper, std::size_t slot, SampleRange range, bool stereo)
    {
        std::string error;
        auto item = rangePlayback(upper, slot, range, stereo, error);
        if (!item.has_value()) {
            showMessage("Playback failed", error, Gtk::MessageType::WARNING);
            return false;
        }
        return startPlaybackQueue({std::move(*item)});
    }

    /*****************************************************************************
     * playWholeTrack
     *
     * Plays one labelled track from start to finish; playing either channel of
     * a stereo file plays the file in stereo. Label clicks use this path so
     * they do not depend on an existing mark.
     *
     *****************************************************************************/
    bool playWholeTrack(bool upper, std::size_t slot)
    {
        const Track* trackPtr = panel(upper).track(slot);
        if (trackPtr == nullptr || !trackPtr->isBinary()) {
            return false;
        }
        return playTrackRange(upper, slot,
                              SampleRange{0, static_cast<int64_t>(trackPtr->sampleCount())},
                              trackPtr->isStereoChannel());
    }

    /*****************************************************************************
     * playMarkedArea
     *
     * Plays the complete mark of one track, in stereo when asked and the track
     * is a stereo channel (WinSap PlayMarkedArea).
     *
     *****************************************************************************/
    bool playMarkedArea(bool upper, std::size_t slot, bool stereo)
    {
        const Panel& source = panel(upper);
        const Track* trackPtr = source.track(slot);
        const auto range = source.selectedSampleRange();
        if (trackPtr == nullptr || !trackPtr->isBinary() || !source.markedAreaHasData(slot) ||
            !range.has_value()) {
            return false;
        }
        return playTrackRange(upper, slot, *range, stereo && trackPtr->isStereoChannel());
    }

    /*****************************************************************************
     * playPanel
     *
     * Plays the marked area of the first file in the panel that has data there;
     * a stereo file plays both channels (WinSap PlayPanelMark).
     *
     *****************************************************************************/
    bool playPanel(bool upper)
    {
        const Panel& source = panel(upper);
        if (!source.isActive()) {
            return false;
        }
        const auto range = source.selectedSampleRange();
        if (!range.has_value() || range->empty()) {
            setStatus("Mark an area first: right-click the waveform at its start and its end.");
            return false;
        }
        const auto slot = source.firstMarkedDataSlot();
        if (!slot.has_value()) {
            setStatus("No open file has data in the marked area.");
            return false;
        }
        return playMarkedArea(upper, *slot, true);
    }

    /*****************************************************************************
     * playAll
     *
     * Queues every open waveform and FFT amplitude track in upper-then-down
     * order; a stereo file plays once, in stereo, from its upper slot. Each
     * item keeps its source panel and slot so cursor updates remain accurate.
     *
     *****************************************************************************/
    bool playAll()
    {
        std::vector<QueuedPlayback> queue;
        std::string error;
        for (const bool upper : {true, false}) {
            const Panel& source = panel(upper);
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                const Track* trackPtr = source.track(slot);
                if (trackPtr == nullptr || !trackPtr->isBinary() ||
                    (!upper && trackPtr->isStereoChannel())) {
                    continue;
                }
                auto item = rangePlayback(
                    upper, slot, SampleRange{0, static_cast<int64_t>(trackPtr->sampleCount())},
                    trackPtr->isStereoChannel(), error);
                if (!item.has_value()) {
                    showMessage("Playback failed", error, Gtk::MessageType::WARNING);
                    return false;
                }
                queue.push_back(std::move(*item));
            }
        }
        return startPlaybackQueue(std::move(queue));
    }

    /*****************************************************************************
     * updatePlaybackCursors
     *
     * Maps the combined playback position back onto the source panel segments.
     * Only the segment currently being heard receives a cursor.
     *
     *****************************************************************************/
    void updatePlaybackCursors(std::size_t played)
    {
        upperPanel_.clearPlaybackCursor();
        lowerPanel_.clearPlaybackCursor();
        for (const PlaybackSegment& segment : playbackJob_.segments) {
            if (played <= segment.combinedOffset) {
                break;
            }
            const std::size_t relative = played - segment.combinedOffset;
            const std::size_t used = std::min(relative, segment.sourceSampleCount);
            if (used == 0U) {
                continue;
            }
            std::string ignored;
            panel(segment.upper).setPlaybackCursor(segment.beginSample + static_cast<int64_t>(used) - 1, ignored);
            if (segment.stereo) {
                panel(!segment.upper).setPlaybackCursor(
                    segment.beginSample + static_cast<int64_t>(used) - 1, ignored);
            }
            if (relative < segment.sourceSampleCount) {
                break;
            }
        }
    }

    /*****************************************************************************
     * onPlaybackDispatch
     *
     * Handles playback progress, completion, cancellation and failure on GTK
     * main thread. It advances queued tracks and keeps cursors and status
     * current.
     *
     *****************************************************************************/
    void onPlaybackDispatch()
    {
        if (!playbackJob_.active) {
            return;
        }
        const int notice = playbackNotice_.load(std::memory_order_acquire);
        const std::size_t played = playbackSamples_.load(std::memory_order_acquire);
        if (notice == 1 || played != handledPlaybackSamples_) {
            handledPlaybackSamples_ = played;
            updatePlaybackCursors(played);
            updateStatusSegments();
            if (plotArea_) {
                plotArea_->queue_draw();
            }
        }
        if (notice == handledPlaybackNotice_ || notice == 0) {
            return;
        }
        handledPlaybackNotice_ = notice;
        if (notice == 2) {
            if (playbackQueueIndex_ + 1U < playbackQueue_.size()) {
                ++playbackQueueIndex_;
                upperPanel_.clearPlaybackCursor();
                lowerPanel_.clearPlaybackCursor();
                if (!launchPlayback(std::move(playbackQueue_[playbackQueueIndex_]))) {
                    playbackQueue_.clear();
                    playbackQueueIndex_ = 0;
                }
                return;
            }
            playbackJob_.active = false;
            playbackQueue_.clear();
            playbackQueueIndex_ = 0;
            upperPanel_.clearPlaybackCursor();
            lowerPanel_.clearPlaybackCursor();
            setStatus("Playback completed.");
        } else if (notice == 3) {
            playbackJob_.active = false;
            playbackQueue_.clear();
            playbackQueueIndex_ = 0;
            upperPanel_.clearPlaybackCursor();
            lowerPanel_.clearPlaybackCursor();
            if (!playbackStoppedByUser_) {
                setStatus("Playback cancelled.");
            }
            playbackStoppedByUser_ = false;
        } else if (notice == 4) {
            playbackJob_.active = false;
            playbackQueue_.clear();
            playbackQueueIndex_ = 0;
            upperPanel_.clearPlaybackCursor();
            lowerPanel_.clearPlaybackCursor();
            playbackStoppedByUser_ = false;
            showMessage("Playback failed", audioPlayer_.lastError(), Gtk::MessageType::ERROR);
        }
    }

    /*****************************************************************************
     * panelHasWaveform
     *
     * Reports whether a panel contains a waveform track. Drawing uses this to
     * decide when amplitude features such as marks and zero lines apply.
     *
     *****************************************************************************/
    bool panelHasWaveform(const Panel& source) const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr != nullptr && trackPtr->type() == TrackType::WaveformPcm16) {
                return true;
            }
        }
        return false;
    }

    /*****************************************************************************
     * valueToY
     *
     * Maps a data value into plot coordinates using the panel limits. Values
     * are clamped so out-of-range samples do not draw outside the panel.
     *
     *****************************************************************************/
    double valueToY(const Panel& source, const Rect& plot, double value) const
    {
        const double lower = source.lowerLimit();
        const double upper = source.upperLimit();
        const double clamped = std::clamp(value, lower, upper);
        const double ratio = (upper - clamped) / (upper - lower);
        return plot.y + ratio * std::max(0.0, plot.height - 1.0);
    }

    /*****************************************************************************
     * sampleToX
     *
     * Maps an absolute sample index into the current visible plot. The allow-
     * end flag lets marked ranges include their right edge without selecting
     * past it.
     *
     *****************************************************************************/
    std::optional<double> sampleToX(const Panel& source,
                                    const Rect& plot,
                                    int64_t sample,
                                    bool allowEnd) const
    {
        const SampleRange visible = source.visibleSampleRange();
        if (visible.empty() || sample < visible.begin ||
            (allowEnd ? sample > visible.end : sample >= visible.end)) {
            return std::nullopt;
        }
        const double offset = static_cast<double>(sample - visible.begin) /
                              static_cast<double>(visible.length());
        return plot.x + offset * plot.width;
    }

    /*****************************************************************************
     * drawWaveform
     *
     * Draws waveform data as a connected trace. The track reduces dense data
     * to pixel columns first because Cairo drawing dominates redraw cost.
     *
     *****************************************************************************/
    void drawWaveform(const Cairo::RefPtr<Cairo::Context>& cr,
                      const Panel& source,
                      const Track& trackPtr,
                      const Rect& plot,
                      const std::string& color)
    {
        const SampleRange visible = source.visibleSampleRange();
        if (visible.empty() || plot.width < 1.0 || plot.height < 1.0) {
            return;
        }
        std::string error;
        const auto points = trackPtr.computeConnectedPoints(
            visible, static_cast<std::size_t>(plot.width), static_cast<std::size_t>(plot.width) * 4U, error);
        setSource(cr, color);
        cr->set_line_width(1.25);
        if (!points.empty()) {
            cr->move_to(plot.x + points.front().x,
                        valueToY(source, plot, points.front().sample));
            for (std::size_t index = 1; index < points.size(); ++index) {
                cr->line_to(plot.x + points[index].x,
                            valueToY(source, plot, points[index].sample));
            }
            cr->stroke();
            return;
        }
        const auto columns =
            trackPtr.computeEnvelope(visible, static_cast<std::size_t>(plot.width), error);
        if (!error.empty()) {
            drawText(cr, plot.x + 8, plot.y + 20, error, 12.0, "#ff7788");
            return;
        }
        for (std::size_t index = 0; index < columns.size(); ++index) {
            const auto x = sampleToX(source, plot, columns[index].source.begin, false);
            if (!x.has_value()) {
                continue;
            }
            const double ymin = valueToY(source, plot, columns[index].minimum);
            const double ymax = valueToY(source, plot, columns[index].maximum);
            cr->move_to(*x, ymin);
            cr->line_to(*x, ymax);
        }
        cr->stroke();
    }

    // The single-frame line spectrum. Reduced to one vertical span per pixel
    // column before drawing: at ~1000 bins the old per-bin polyline rasterised a
    // near full height zigzag segment for every bin, which measured about
    // 5 ms a repaint against 21 us for the transform itself.
    /*****************************************************************************
     * drawSpectrumLine
     *
     * Draws the single-frame spectrum as one vertical span per pixel column.
     * Reducing bins before drawing avoids the expensive zigzag polyline path.
     *
     *****************************************************************************/
    void drawSpectrumLine(const Cairo::RefPtr<Cairo::Context>& cr,
                          const Panel& source,
                          const Track& trackPtr,
                          const Rect& plot,
                          const std::string& color)
    {
        // Honour the frame count: the window is every frame the user asked to
        // see, so more frames means better frequency resolution and worse time
        // resolution. Past kSpectrogramThreshold drawSpectrum switches modes.
        const float* db = trackPtr.spectrumDb(source.frameBegin(), source.frameLength(),
                                              source.visibleFrameCount());
        if (db == nullptr) {
            return;
        }

        const int bins = xsap::core::kFftCutOff;
        const int columns = std::max(1, std::min(static_cast<int>(plot.width), bins));

        setSource(cr, color);
        cr->save();
        cr->set_antialias(Cairo::ANTIALIAS_NONE);
        cr->set_line_width(1.0);

        const double columnWidth = plot.width / static_cast<double>(columns);

        int bin = 0;
        for (int column = 0; column < columns; ++column) {
            const int binEnd =
                static_cast<int>((static_cast<long long>(column) + 1) * bins / columns);
            float lo = db[bin];
            float hi = db[bin];
            for (; bin < binEnd && bin < bins; ++bin) {
                lo = std::min(lo, db[bin]);
                hi = std::max(hi, db[bin]);
            }
            const double x =
                std::floor(plot.x + static_cast<double>(column) * columnWidth) + 0.5;
            const double yTop = std::floor(valueToY(source, plot, static_cast<double>(hi))) + 0.5;
            const double yBottom =
                std::floor(valueToY(source, plot, static_cast<double>(lo))) + 0.5;
            cr->move_to(x, yTop);
            cr->line_to(x, yBottom);
        }
        cr->stroke();
        cr->restore();
    }

    // The grey scale spectrogram SAP shows once the visible span is wider than
    // one transform. Built into an image and blitted once, rather than the
    // original pen-per-grey-level, line-per-pixel-row drawing.
    //
    // Prefers the whole file cache, whose columns sit on a file anchored grid
    // so scrolling neither re-runs transforms nor shifts the picture. Until
    // that background analysis finishes, falls back to analysing the view.
    /*****************************************************************************
     * drawSpectrogram
     *
     * Draws the spectrogram as a cached image surface. Whole-file cached views
     * are preferred so scrolling does not recompute or shift the picture.
     *
     *****************************************************************************/
    void drawSpectrogram(const Cairo::RefPtr<Cairo::Context>& cr,
                         const Track& trackPtr,
                         const Rect& plot,
                         std::size_t firstSample,
                         std::size_t span)
    {
        const int pixelWidth = static_cast<int>(plot.width);
        const int pixelHeight = static_cast<int>(plot.height);
        if (pixelWidth <= 0 || pixelHeight <= 0) {
            return;
        }

        SpectrogramSurface* entry = nullptr;
        for (auto& candidate : spectrogramSurfaces_) {
            if (candidate.owner == &trackPtr) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            SpectrogramSurface fresh;
            fresh.owner = &trackPtr;
            spectrogramSurfaces_.push_back(fresh);
            entry = &spectrogramSurfaces_.back();
        }

        // Full band; Render() reads a non-positive width as the whole spectrum.
        const auto* view = trackPtr.spectrogramView(firstSample, span, pixelWidth, pixelHeight,
                                                    spectrogramRangeDb_, 0.0, 0.0);
        if (view != nullptr && !view->empty()) {
            const std::uint64_t generation = trackPtr.spectrogramCacheGeneration();
            const bool stale = !entry->surface || !entry->fromCache ||
                               entry->firstSample != firstSample || entry->span != span ||
                               entry->width != pixelWidth || entry->height != pixelHeight ||
                               entry->rangeDb != spectrogramRangeDb_ ||
                               entry->palette != spectrogramPalette_ ||
                               entry->generation != generation;
            if (stale) {
                xsap::core::SpectrogramColor palette[256];
                xsap::core::BuildSpectrogramPalette(spectrogramPalette_, palette);
                entry->surface = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24,
                                                             view->width, view->height);
                unsigned char* base = entry->surface->get_data();
                const int stride = entry->surface->get_stride();
                for (int row = 0; row < view->height; ++row) {
                    const std::uint8_t* source =
                        view->grey.data() + static_cast<std::size_t>(row) *
                                                static_cast<std::size_t>(view->width);
                    auto* pixels = reinterpret_cast<std::uint32_t*>(base + row * stride);
                    for (int column = 0; column < view->width; ++column) {
                        const std::uint32_t level = source[column];
                        const auto& rgb = palette[level];
                        pixels[column] = (static_cast<std::uint32_t>(rgb.r) << 16) |
                                         (static_cast<std::uint32_t>(rgb.g) << 8) |
                                         static_cast<std::uint32_t>(rgb.b);
                    }
                }
                entry->surface->mark_dirty();
                entry->firstSample = firstSample;
                entry->span = span;
                entry->width = pixelWidth;
                entry->height = pixelHeight;
                entry->columns = view->width;
                entry->rangeDb = spectrogramRangeDb_;
                entry->palette = spectrogramPalette_;
                entry->generation = generation;
                entry->fromCache = true;
            }

            // The cached view is already sized to the plot, so this is a
            // one to one blit with no scaling or filtering.
            cr->save();
            cr->rectangle(plot.x, plot.y, plot.width, plot.height);
            cr->clip();
            cr->set_source(entry->surface, plot.x, plot.y);
            cr->paint();
            cr->restore();

            if (trackPtr.spectrogramCacheBuilding()) {
                drawText(cr, plot.x + 8, plot.y + 20,
                         "analysing " + std::to_string(trackPtr.spectrogramCacheProgress()) + "%",
                         12.0, "#ff7788");
                notifySpectrogramBuilding();
            }
            return;
        }

        const auto* image = trackPtr.spectrogram(firstSample, span, pixelWidth);
        if (image == nullptr || image->empty()) {
            return;
        }

        const bool stale = !entry->surface || entry->fromCache ||
                           entry->firstSample != firstSample || entry->span != span ||
                           entry->width != pixelWidth || entry->columns != image->columns ||
                           entry->palette != spectrogramPalette_;
        if (stale) {
            xsap::core::SpectrogramColor palette[256];
            xsap::core::BuildSpectrogramPalette(spectrogramPalette_, palette);
            entry->surface = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24,
                                                         image->columns, image->bins);
            unsigned char* base = entry->surface->get_data();
            const int stride = entry->surface->get_stride();
            for (int row = 0; row < image->bins; ++row) {
                // Row 0 is the top of the image and must show the highest bin.
                const int binIndex = image->bins - 1 - row;
                auto* pixels = reinterpret_cast<std::uint32_t*>(base + row * stride);
                for (int column = 0; column < image->columns; ++column) {
                    const std::uint32_t level =
                        image->grey[static_cast<std::size_t>(column) *
                                        static_cast<std::size_t>(image->bins) +
                                    static_cast<std::size_t>(binIndex)];
                    const auto& rgb = palette[level];
                    pixels[column] = (static_cast<std::uint32_t>(rgb.r) << 16) |
                                     (static_cast<std::uint32_t>(rgb.g) << 8) |
                                     static_cast<std::uint32_t>(rgb.b);
                }
            }
            entry->surface->mark_dirty();
            entry->firstSample = firstSample;
            entry->span = span;
            entry->width = pixelWidth;
            entry->height = pixelHeight;
            entry->columns = image->columns;
            entry->palette = spectrogramPalette_;
            entry->fromCache = false;
        }

        cr->save();
        cr->rectangle(plot.x, plot.y, plot.width, plot.height);
        cr->clip();
        cr->translate(plot.x, plot.y);
        cr->scale(plot.width / static_cast<double>(image->columns),
                  plot.height / static_cast<double>(image->bins));
        auto pattern = Cairo::SurfacePattern::create(entry->surface);
        pattern->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
        cr->set_source(pattern);
        cr->paint();
        cr->restore();

        if (trackPtr.spectrogramCacheBuilding()) {
            drawText(cr, plot.x + 8, plot.y + 20,
                     "analysing " + std::to_string(trackPtr.spectrogramCacheProgress()) + "%",
                     12.0, "#ff7788");
            notifySpectrogramBuilding();
        }
    }

    // Nothing else wakes the UI when a background analysis finishes, so poll
    // while one is outstanding and stop as soon as no repaint needs it.
    /*****************************************************************************
     * notifySpectrogramBuilding
     *
     * Polls the UI while background spectrogram analysis is still building. It
     * stops the timeout automatically once no repaint reports pending work.
     *
     *****************************************************************************/
    void notifySpectrogramBuilding()
    {
        spectrogramBuildPending_ = true;
        if (spectrogramPoll_.connected()) {
            return;
        }
        spectrogramPoll_ = Glib::signal_timeout().connect(
            [this]() {
                const bool pending = spectrogramBuildPending_;
                spectrogramBuildPending_ = false;
                if (plotArea_ != nullptr) {
                    plotArea_->queue_draw();
                }
                return pending;
            },
            150);
    }

    // Steps to the next colour map. Display only: no analysis is repeated, and
    // the whole file cache is untouched.
    /*****************************************************************************
     * cycleSpectrogramPalette
     *
     * Steps to the next spectrogram colour map without touching the analysis
     * cache. Only the cached surface colours need to be repainted.
     *
     *****************************************************************************/
    void cycleSpectrogramPalette()
    {
        spectrogramPalette_ = xsap::core::NextSpectrogramPalette(spectrogramPalette_);
        setStatus(std::string("Spectrogram colour map: ") +
                  xsap::core::SpectrogramPaletteName(spectrogramPalette_));
        std::string error;
        if (!saveSettings(error, false, false, false, true)) {
            showMessage("Spectrogram preference not saved",
                        "Xsap could not save the colour map: " + error,
                        Gtk::MessageType::WARNING);
        }
        if (plotArea_ != nullptr) {
            plotArea_->queue_draw();
        }
    }

    // A narrower ramp means more contrast: the grey levels are spread over
    // fewer dB below the file peak.
    /*****************************************************************************
     * adjustSpectrogramRange
     *
     * Adjusts the spectrogram dynamic range within safe display bounds. Smaller
     * ranges increase contrast by spreading grey levels over fewer decibels.
     *
     *****************************************************************************/
    void adjustSpectrogramRange(float deltaDb)
    {
        const float updated = std::clamp(spectrogramRangeDb_ + deltaDb, 20.0f, 120.0f);
        spectrogramRangeDb_ = updated;
        setStatus(std::string("Spectrogram Contrast ") +
                  (deltaDb < 0.0f ? "Up: " : "Down: ") +
                  std::to_string(static_cast<int>(spectrogramRangeDb_)) +
                  " dB below peak.");
        std::string error;
        if (!saveSettings(error, false, false, false, true)) {
            showMessage("Spectrogram preference not saved",
                        "Xsap could not save the contrast range: " + error,
                        Gtk::MessageType::WARNING);
        }
        redraw();
    }

    /*****************************************************************************
     * drawSpectrum
     *
     * Chooses between the line spectrum and spectrogram displays. The threshold
     * follows the SAP drawing rule for switching from one transform to many.
     *
     *****************************************************************************/
    void drawSpectrum(const Cairo::RefPtr<Cairo::Context>& cr,
                      const Panel& source,
                      const Track& trackPtr,
                      const Rect& plot,
                      const std::string& color)
    {
        if (plot.width < 1.0 || plot.height < 1.0) {
            return;
        }

        // wsap DRAWAVE.CPP dispSpectrum switches display mode at this span.
        // WinSap and wsap2025 kept only the line plot; see core/SapSpectrogram.h.
        const std::size_t firstSample = source.frameBegin() * source.frameLength();
        const std::size_t span = source.visibleFrameCount() * source.frameLength();

        if (span > xsap::core::kSpectrogramThreshold) {
            drawSpectrogram(cr, trackPtr, plot, firstSample, span);
            return;
        }
        drawSpectrumLine(cr, source, trackPtr, plot, color);
    }

    /*****************************************************************************
     * panelHasParameter
     *
     * Reports whether a panel contains a parameter track. Layout uses this to
     * reserve the extra text strip only when it is needed.
     *
     *****************************************************************************/
    bool panelHasParameter(const Panel& source) const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr != nullptr && trackPtr->type() == TrackType::ParameterText) {
                return true;
            }
        }
        return false;
    }

    // Which quantities the panel is currently plotting. The spectrum window
    // shows two quite different pictures, and the axis labels and the hover
    // readout both have to follow whichever one is on screen.
    enum class HoverAxis { Amplitude,
                           SpectrumLine,
                           Spectrogram };

    // Reports the current display and, for the two spectrum displays, the
    // frequency belonging at the top of the plot and the file's sample rate.
    /*****************************************************************************
     * hoverAxis
     *
     * Describes the quantity currently plotted under the cursor. Spectrum
     * displays change both axes, so hover text and labels must follow the
     * active mode.
     *
     *****************************************************************************/
    HoverAxis hoverAxis(const Panel& source,
                        double& topHz,
                        unsigned int& sampleRate) const noexcept
    {
        topHz = 0.0;
        sampleRate = 0;
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr == nullptr || trackPtr->type() != TrackType::FftAmplitude) {
                continue;
            }
            const unsigned int rate = trackPtr->effectiveSampleRate(rawSampleRate_);
            sampleRate = rate;

            const std::size_t span = source.visibleFrameCount() * source.frameLength();
            if (span > xsap::core::kSpectrogramThreshold) {
                // The cache analyses to Nyquist.
                topHz = rate / 2.0;
                return HoverAxis::Spectrogram;
            }

            // The line plot draws bins 0 .. kFftCutOff-1 of a kFftLength point
            // transform, which deliberately stops short of Nyquist.
            topHz = static_cast<double>(rate) * xsap::core::kFftCutOff /
                    static_cast<double>(xsap::core::kFftLength);
            return HoverAxis::SpectrumLine;
        }
        return HoverAxis::Amplitude;
    }

    // True while the panel is drawing a spectrogram rather than a single frame
    // spectrum, in which case the vertical axis is frequency and this reports
    // the frequency that belongs at the top of it.
    /*****************************************************************************
     * spectrogramAxisActive
     *
     * Reports whether a panel is currently showing a spectrogram and returns
     * its top frequency. Amplitude labels use this to switch to hertz.
     *
     *****************************************************************************/
    bool spectrogramAxisActive(const Panel& source, double& topHz) const noexcept
    {
        double top = 0.0;
        unsigned int rate = 0;
        if (hoverAxis(source, top, rate) != HoverAxis::Spectrogram) {
            return false;
        }
        topHz = top;
        return true;
    }

    /*****************************************************************************
     * panelSampleRate
     *
     * Finds the effective sample rate for the panel first binary track. The raw
     * default is used only when no binary metadata is available.
     *
     *****************************************************************************/
    unsigned int panelSampleRate(const Panel& source) const noexcept
    {
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr != nullptr && trackPtr->isBinary()) {
                return trackPtr->effectiveSampleRate(rawSampleRate_);
            }
        }
        return rawSampleRate_;
    }

    /*****************************************************************************
     * makeLayout
     *
     * Computes the rectangles for one panel, including frame header, parameter
     * strip and plot. Upper and down panels mirror the header placement.
     *
     *****************************************************************************/
    PanelLayout makeLayout(double top,
                           double height,
                           int width,
                           bool upper,
                           bool hasParameter) const
    {
        constexpr double characterWidth = 8.4;
        constexpr double characterHeight = 16.0;
        constexpr double lineSpacing = 2.0;
        constexpr double frameHeight = characterHeight + lineSpacing + 1.0;
        constexpr double parameterHeight = 2.0 * (characterHeight + lineSpacing) + 1.0;
        PanelLayout result;
        result.panel = Rect{0.0, top, static_cast<double>(std::max(0, width)),
                            std::max(0.0, height)};
        const double contentTop = upper ? top + frameHeight : top;
        const double contentBottom = upper ? top + height : top + height - frameHeight;
        result.frameInfo = upper ? Rect{0.0, top, result.panel.width, frameHeight}
                                 : Rect{0.0, contentBottom, result.panel.width, frameHeight};
        if (hasParameter) {
            result.parameters =
                upper ? Rect{0.0, contentTop, result.panel.width, parameterHeight}
                      : Rect{0.0, contentBottom - parameterHeight, result.panel.width,
                             parameterHeight};
        }
        const double plotTop = upper ? contentTop + (hasParameter ? parameterHeight : 0.0)
                                     : contentTop;
        const double plotBottom =
            upper ? contentBottom
                  : contentBottom - (hasParameter ? parameterHeight : 0.0);
        const double gutterWidth = 10.0 * characterWidth;
        result.plot = Rect{gutterWidth, plotTop,
                           std::max(0.0, result.panel.width - gutterWidth),
                           std::max(0.0, plotBottom - plotTop)};
        return result;
    }

    /*****************************************************************************
     * drawAmplitudeLabels
     *
     * Draws the vertical scale labels for amplitude or spectrogram frequency.
     * Tick density is derived from available pixels to avoid overlapping text.
     *
     *****************************************************************************/
    void drawAmplitudeLabels(const Cairo::RefPtr<Cairo::Context>& cr,
                             const Panel& source,
                             const PanelLayout& layout)
    {
        constexpr double characterWidth = 8.4;
        constexpr double characterHeight = 16.0;
        constexpr double fontSize = 14.0;
        if (layout.plot.height < 3.0 * characterHeight) {
            return;
        }

        const double oneSidePixels =
            (layout.plot.height - 3.0 * characterHeight) / 2.0;

        // The spectrum window carries two different displays, so the vertical
        // axis has to say which one it is labelling. A single frame is
        // amplitude against frequency, so the axis is dB; a spectrogram is
        // frequency against time, so the axis is Hz, zero at the bottom and
        // Nyquist at the top.
        double axisTop = source.upperLimit();
        double axisBottom = source.lowerLimit();
        double nyquistHz = 0.0;
        if (spectrogramAxisActive(source, nyquistHz)) {
            axisTop = nyquistHz;
            axisBottom = 0.0;
        }

        const int tickCount = std::max(
            0, static_cast<int>((oneSidePixels - characterHeight) /
                                (2.0 * characterHeight)));
        const double lineSpacing =
            (oneSidePixels - static_cast<double>(tickCount) * characterHeight) /
            static_cast<double>(tickCount + 1);
        const double step =
            (axisTop - axisBottom) / static_cast<double>(2 * (tickCount + 1));
        auto drawLabel = [&](double value, double top) {
            std::ostringstream label;
            label.imbue(std::locale::classic());
            label << std::setw(8) << std::fixed << std::setprecision(0) << value;
            drawText(cr, layout.panel.x + characterWidth, top + 12.0, label.str(),
                     fontSize, "#00ffff");
        };

        drawLabel(axisTop, layout.plot.y + 1.0);
        drawLabel((axisTop + axisBottom) / 2.0,
                  layout.plot.y + characterHeight + oneSidePixels);
        drawLabel(axisBottom,
                  layout.plot.y + layout.plot.height - characterHeight - 1.0);

        double yOffset = lineSpacing;
        double value = axisTop - step;
        const double upperStart = layout.plot.y + characterHeight;
        for (int tick = 0; tick < tickCount; ++tick) {
            drawLabel(value, upperStart + yOffset);
            yOffset += characterHeight + lineSpacing;
            value -= step;
        }

        yOffset = lineSpacing;
        value = (axisTop + axisBottom) / 2.0 - step;
        const double lowerStart =
            layout.plot.y + 2.0 * characterHeight + oneSidePixels;
        for (int tick = 0; tick < tickCount; ++tick) {
            drawLabel(value, lowerStart + yOffset);
            yOffset += characterHeight + lineSpacing;
            value -= step;
        }
    }

    /*****************************************************************************
     * drawPanel
     *
     * Draws one complete panel: frame header, file labels, parameter text, plot
     * contents and borders. The sequence mirrors the old fixed-layout display.
     *
     *****************************************************************************/
    void drawPanel(const Cairo::RefPtr<Cairo::Context>& cr,
                   const Panel& source,
                   PanelLayout& layout,
                   bool upper)
    {
        if (!source.isActive()) {
            return;
        }
        static const std::array<std::string, Panel::kMaxTracks> colors = {
            "#ffff00", "#ff00ff", "#00ffff", "#ffffff", "#00ff00", "#0000ff"};
        layout.labels = {};

        setSource(cr, "#000000");
        cr->rectangle(layout.panel.x, layout.panel.y, layout.panel.width, layout.panel.height);
        cr->fill();
        strokeRect(cr, layout.panel, "#00ffff", 1.0);

        setSource(cr, "#00ffff");
        cr->set_line_width(1.0);
        const double frameSeparator = upper ? layout.frameInfo.y + layout.frameInfo.height
                                            : layout.frameInfo.y;
        cr->move_to(layout.panel.x, frameSeparator);
        cr->line_to(layout.panel.x + layout.panel.width, frameSeparator);
        if (layout.parameters.height > 0.0) {
            const double parameterSeparator =
                upper ? layout.parameters.y + layout.parameters.height : layout.parameters.y;
            cr->move_to(layout.panel.x, parameterSeparator);
            cr->line_to(layout.panel.x + layout.panel.width, parameterSeparator);
        }
        cr->move_to(layout.plot.x, layout.plot.y);
        cr->line_to(layout.plot.x, layout.plot.y + layout.plot.height);
        cr->stroke();

        constexpr double charWidth = 8.4;
        constexpr double fontSize = 14.0;
        const double baseline = layout.frameInfo.y + 14.0;
        drawText(cr, layout.panel.x + charWidth, baseline, "Frame=", fontSize, "#00ffff");
        drawText(cr, layout.panel.x + 18.0 * charWidth, baseline, "...", fontSize, "#00ffff");
        drawText(cr, layout.panel.x + 33.0 * charWidth, baseline, "FrameSize=", fontSize,
                 "#00ffff");
        drawText(cr, layout.panel.x + 64.0 * charWidth, baseline, "...", fontSize, "#00ffff");

        // Frame numbers take 10 columns (kMaxParameterFrames has 10 digits) and
        // times 13 (hhh:mm:ss.mmm), matching WinSAP DrawFrameInfo.
        std::ostringstream startFrame;
        startFrame << std::setw(10) << source.frameBegin();
        std::ostringstream endFrame;
        endFrame << std::setw(10) << source.frameBegin() + source.visibleFrameCount();
        std::ostringstream frameLength;
        frameLength << std::setw(5) << source.frameLength();
        drawText(cr, layout.panel.x + 7.0 * charWidth, baseline, startFrame.str(), fontSize,
                 "#ffffff");
        drawText(cr, layout.panel.x + 22.0 * charWidth, baseline, endFrame.str(), fontSize,
                 "#ffffff");
        drawText(cr, layout.panel.x + 43.0 * charWidth, baseline, frameLength.str(), fontSize,
                 "#ffffff");

        const unsigned int sampleRate = panelSampleRate(source);
        const int64_t firstSample =
            static_cast<int64_t>(source.frameBegin()) *
            static_cast<int64_t>(source.frameLength());
        const int64_t lastSample =
            static_cast<int64_t>(source.frameBegin() + source.visibleFrameCount()) *
            static_cast<int64_t>(source.frameLength());
        drawText(cr, layout.panel.x + 50.0 * charWidth, baseline,
                 formatPanelTime(firstSample, sampleRate), fontSize, "#ffffff");
        drawText(cr, layout.panel.x + 68.0 * charWidth, baseline,
                 formatPanelTime(lastSample, sampleRate), fontSize, "#ffffff");

        double fileX = layout.panel.x + 82.0 * charWidth;
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr == nullptr || fileX >= layout.panel.x + layout.panel.width - 20.0) {
                continue;
            }
            std::string name = fileNameText(trackPtr->path());
            if (trackPtr->isStereoChannel()) {
                name += trackPtr->channel() == 0 ? " [L]" : " [R]";
            }
            const double width = std::max(charWidth, static_cast<double>(name.size()) * charWidth);
            drawText(cr, fileX, baseline, name, fontSize, colors[slot]);
            layout.labels[slot] =
                Rect{fileX, layout.frameInfo.y, std::min(width, layout.panel.width - fileX),
                     layout.frameInfo.height};
            fileX += width + charWidth;
        }

        if (layout.parameters.height > 0.0) {
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                const Track* trackPtr = source.track(slot);
                if (trackPtr == nullptr || trackPtr->type() != TrackType::ParameterText) {
                    continue;
                }
                const auto parameter = trackPtr->currentParameterFrame(source);
                if (parameter.has_value()) {
                    drawText(cr, layout.parameters.x + 8.0, layout.parameters.y + 14.0,
                             parameter->header + "  " + parameter->line1, fontSize,
                             "#ffff00");
                    drawText(cr, layout.parameters.x + 8.0, layout.parameters.y + 32.0,
                             parameter->line2, fontSize, "#ffff00");
                }
                break;
            }
        }

        const Rect waveArea{layout.plot.x + 2.0,
                            layout.plot.y + 2.0,
                            std::max(0.0, layout.plot.width - 4.0),
                            std::max(0.0, layout.plot.height - 4.0)};
        setSource(cr, "#000000");
        cr->rectangle(waveArea.x, waveArea.y, waveArea.width, waveArea.height);
        cr->fill();
        const bool waveform = panelHasWaveform(source);
        if (waveform) {
            const auto mark = source.selectedSampleRange();
            if (mark.has_value()) {
                const SampleRange visible = source.visibleSampleRange();
                const int64_t begin = std::max(visible.begin, mark->begin);
                const int64_t end = std::min(visible.end, mark->end);
                if (end > begin) {
                    const auto x1 = sampleToX(source, waveArea, begin, true);
                    const auto x2 = sampleToX(source, waveArea, end, true);
                    if (x1.has_value() && x2.has_value()) {
                        setSource(cr, "#808080");
                        cr->rectangle(*x1, waveArea.y, *x2 - *x1, waveArea.height);
                        cr->fill();
                    }
                }
            }
        }

        cr->save();
        cr->rectangle(waveArea.x, waveArea.y, waveArea.width, waveArea.height);
        cr->clip();
        if (waveform && source.lowerLimit() < 0.0 && source.upperLimit() > 0.0) {
            const double zero = valueToY(source, layout.plot, 0.0);
            setSource(cr, "#00ffff");
            cr->set_line_width(1.0);
            cr->move_to(layout.plot.x, zero);
            cr->line_to(layout.plot.x + layout.plot.width, zero);
            cr->stroke();
        }
        for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
            const Track* trackPtr = source.track(slot);
            if (trackPtr == nullptr) {
                continue;
            }
            if (trackPtr->type() == TrackType::FftAmplitude) {
                drawSpectrum(cr, source, *trackPtr, waveArea, colors[slot]);
            } else if (trackPtr->isBinary()) {
                drawWaveform(cr, source, *trackPtr, waveArea, colors[slot]);
            }
        }
        if (upper && overlayLower_ && waveform && panelHasWaveform(lowerPanel_)) {
            const Track* overlayTrack = nullptr;
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                const Track* trackPtr = lowerPanel_.track(slot);
                if (trackPtr != nullptr && trackPtr->type() == TrackType::WaveformPcm16) {
                    overlayTrack = trackPtr;
                    break;
                }
            }
            if (overlayTrack != nullptr) {
                drawWaveform(cr, lowerPanel_, *overlayTrack, waveArea, "#ff0000");
            }
        }
        const bool spectrumFile = panelHasFft(source);
        const bool spectrogramView =
            spectrumFile && source.visibleFrameCount() * source.frameLength() >
                                xsap::core::kSpectrogramThreshold;
        if (spectrogramView) {
            // A spectrogram covers the grey mark background, so the marked block
            // is shown by its edges instead: white lines at the start and, once
            // complete, the end.
            const auto mark = source.selectedSampleRange();
            const auto& pending = upper ? pendingUpperMark_ : pendingLowerMark_;
            std::vector<int64_t> edges;
            if (mark.has_value()) {
                edges = {mark->begin, mark->end};
            } else if (pending.has_value()) {
                edges = {*pending};
            }
            setSource(cr, "#ffffff");
            cr->set_line_width(1.0);
            for (const int64_t edge : edges) {
                const auto x = sampleToX(source, waveArea, edge, true);
                if (x.has_value()) {
                    cr->move_to(*x, waveArea.y);
                    cr->line_to(*x, waveArea.y + waveArea.height);
                }
            }
            cr->stroke();
        }
        // A single-frame spectrum line has a frequency x-axis, so there is no
        // time cursor
        if (panelHasBinary(source) && (!spectrumFile || spectrogramView)) {
            const std::optional<int64_t> playback = source.playbackCursor();
            if (playback.has_value()) {
                const auto x = sampleToX(source, waveArea, *playback, false);
                if (x.has_value()) {
                    setSource(cr, "#ff8000");
                    cr->set_line_width(2.0);
                    cr->move_to(*x, waveArea.y);
                    cr->line_to(*x, waveArea.y + waveArea.height);
                    cr->stroke();
                }
            }
        }
        cr->restore();
        drawAmplitudeLabels(cr, source, layout);

        strokeRect(cr, layout.panel, "#00ffff", 1.0);
        setSource(cr, "#00ffff");
        cr->set_line_width(1.0);
        cr->move_to(layout.panel.x, frameSeparator);
        cr->line_to(layout.panel.x + layout.panel.width, frameSeparator);
        if (layout.parameters.height > 0.0) {
            const double parameterSeparator =
                upper ? layout.parameters.y + layout.parameters.height : layout.parameters.y;
            cr->move_to(layout.panel.x, parameterSeparator);
            cr->line_to(layout.panel.x + layout.panel.width, parameterSeparator);
        }
        cr->move_to(layout.plot.x, layout.plot.y);
        cr->line_to(layout.plot.x, layout.plot.y + layout.plot.height);
        cr->stroke();
    }

    /*****************************************************************************
     * draw
     *
     * Clears the drawing area, lays out the active panels and paints them. When
     * both panels are active the gap and heights are split deterministically.
     *
     *****************************************************************************/
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height)
    {
        setSource(cr, "#000000");
        cr->paint();
        upperLayout_ = {};
        lowerLayout_ = {};
        const bool upperActive = upperPanel_.isActive();
        const bool lowerActive = lowerPanel_.isActive();
        if (!upperActive && !lowerActive) {
            return;
        }
        if (upperActive && lowerActive) {
            const double panelHeight =
                std::max(0.0, (static_cast<double>(height) - kPanelGap) / 2.0);
            upperLayout_ = makeLayout(0.0, panelHeight, width, true,
                                      panelHasParameter(upperPanel_));
            lowerLayout_ = makeLayout(panelHeight + kPanelGap, panelHeight, width, false,
                                      panelHasParameter(lowerPanel_));
        } else if (upperActive) {
            upperLayout_ = makeLayout(0.0, static_cast<double>(height), width, true,
                                      panelHasParameter(upperPanel_));
        } else {
            lowerLayout_ = makeLayout(0.0, static_cast<double>(height), width, false,
                                      panelHasParameter(lowerPanel_));
        }
        drawPanel(cr, upperPanel_, upperLayout_, true);
        drawPanel(cr, lowerPanel_, lowerLayout_, false);
    }

    /*****************************************************************************
     * labelAt
     *
     * Finds the track label rectangle under the pointer. The rectangles come
     * from the last paint pass, so hit testing matches what is visible.
     *
     *****************************************************************************/
    std::optional<std::pair<bool, std::size_t>> labelAt(double x, double y) const
    {
        for (bool upper : {true, false}) {
            const PanelLayout& layout = upper ? upperLayout_ : lowerLayout_;
            for (std::size_t slot = 0; slot < Panel::kMaxTracks; ++slot) {
                if (layout.labels[slot].contains(x, y)) {
                    return std::make_pair(upper, slot);
                }
            }
        }
        return std::nullopt;
    }

    /*****************************************************************************
     * handleClick
     *
     * Handles label selection, whole-track playback, closing and right-click
     * marking. Hit testing uses the rectangles saved during the most recent
     * draw.
     *
     *****************************************************************************/
    void handleClick(double x, double y, unsigned int button)
    {
        if (playbackJob_.active) {
            if (button == 3U) {
                stopPlayback(true);
            } else {
                playbackCommandFeedback_ = "Stop playback before using commands.";
                updateStatusSegments();
            }
            return;
        }
        if (const auto label = labelAt(x, y); label.has_value()) {
            Track* trackPtr = panel(label->first).track(label->second);
            if (trackPtr == nullptr) {
                return;
            }
            if (button == 1U) {
                selectTrack(label->first, label->second);
                if (trackPtr->isBinary()) {
                    playWholeTrack(label->first, label->second);
                }
            } else if (button == 3U) {
                // Right-clicking a file name opens its context menu
                showFileMenu(label->first, label->second, x, y);
            }
            redraw();
            return;
        }
        for (bool upper : {true, false}) {
            const PanelLayout& layout = upper ? upperLayout_ : lowerLayout_;
            if (!layout.panel.contains(x, y)) {
                continue;
            }
            setActivePanel(upper);
            if (!layout.plot.contains(x, y)) {
                return;
            }
            Panel& source = panel(upper);
            if (!panelHasBinary(source)) {
                return;
            }
            const std::size_t pixel = static_cast<std::size_t>(
                std::clamp(x - layout.plot.x, 0.0, std::max(0.0, layout.plot.width - 1.0)));
            const auto sample = source.sampleIndexAtPixel(
                pixel, static_cast<std::size_t>(std::max(1.0, layout.plot.width)));
            if (!sample.has_value()) {
                return;
            }
            std::string error;
            if (button == 3U) {
                auto& pending = pendingMark(upper);
                if (source.selectedSampleRange().has_value()) {
                    source.clearMark();
                    pending.reset();
                    setStatus("Mark cleared.");
                } else if (!pending.has_value()) {
                    if (!source.setMarkStart(*sample, error)) {
                        showMessage("Mark", error, Gtk::MessageType::WARNING);
                        return;
                    }
                    pending = *sample;
                    setStatus("Mark start set at sample " + std::to_string(*sample) + ".");
                } else {
                    if (!source.setMarkEnd(*sample, error)) {
                        showMessage("Mark", error, Gtk::MessageType::WARNING);
                        return;
                    }
                    pending.reset();
                    setStatus("Mark end set.");
                }
                redraw();
            }
            return;
        }
    }

    /*****************************************************************************
     * showFileMenu
     *
     * Pops up the menu for a right-clicked file name (WinSap ShowFileMenu):
     * Close file and the marked-area playback commands, plus whole-channel
     * playback for a stereo file. Marked-area items are disabled when the
     * file has no data in a complete mark.
     *
     *****************************************************************************/
    void showFileMenu(bool upper, std::size_t slot, double x, double y)
    {
        const Track* trackPtr = panel(upper).track(slot);
        if (trackPtr == nullptr || !plotArea_) {
            return;
        }
        const bool stereo = trackPtr->isStereoChannel();
        const bool playable = trackPtr->isBinary();
        const bool marked = playable && panel(upper).markedAreaHasData(slot);
        const std::string channel = upper ? "left" : "right";

        if (!fileMenu_) {
            fileMenu_ = std::make_unique<Gtk::Popover>();
            fileMenu_->set_parent(*plotArea_);
            fileMenu_->set_has_arrow(false);
            fileMenu_->set_autohide(true);
            fileMenu_->set_position(Gtk::PositionType::BOTTOM);
        }
        auto items = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        items->add_css_class("xsap-menu");
        auto addItem = [this, items](const std::string& text, bool enabled,
                                     std::function<void()> command) {
            auto item = Gtk::make_managed<Gtk::Button>();
            item->set_has_frame(false);
            auto label = Gtk::make_managed<Gtk::Label>(text);
            label->set_xalign(0.0F);
            item->set_child(*label);
            item->set_sensitive(enabled);
            item->signal_clicked().connect([this, command = std::move(command)]() {
                fileMenu_->popdown();
                Glib::signal_idle().connect_once(command);
            });
            items->append(*item);
        };
        addItem("Close file", true, [this, upper, slot]() { closeFileAt(upper, slot); });
        items->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
        if (stereo) {
            addItem("Play this channel (" + channel + ")", playable, [this, upper, slot]() {
                const Track* current = panel(upper).track(slot);
                if (current != nullptr) {
                    playTrackRange(upper, slot,
                                   SampleRange{0, static_cast<int64_t>(current->sampleCount())},
                                   false);
                }
            });
            addItem("Play marked area (stereo)", marked,
                    [this, upper, slot]() { playMarkedArea(upper, slot, true); });
            addItem("Play marked area in this channel (" + channel + ")", marked,
                    [this, upper, slot]() { playMarkedArea(upper, slot, false); });
        } else {
            addItem("Play marked area", marked,
                    [this, upper, slot]() { playMarkedArea(upper, slot, false); });
        }
        fileMenu_->set_child(*items);
        Gdk::Rectangle point(static_cast<int>(x), static_cast<int>(y), 1, 1);
        fileMenu_->set_pointing_to(point);
        fileMenu_->popup();
    }

    /*****************************************************************************
     * closeFileAt
     *
     * Closes one file from its name's menu. The stereo sync closes the other
     * channel of a stereo file along with it.
     *
     *****************************************************************************/
    void closeFileAt(bool upper, std::size_t slot)
    {
        const Track* trackPtr = panel(upper).track(slot);
        if (trackPtr == nullptr) {
            return;
        }
        const std::string name = fileNameText(trackPtr->path());
        std::string error;
        if (!panel(upper).closeTrack(slot, error)) {
            showMessage("Close failed", error, Gtk::MessageType::ERROR);
            return;
        }
        refreshSelection();
        setStatus("Closed " + name + ".");
    }
    /*****************************************************************************
     * handleMotion
     *
     * Updates hover file information and coordinate readouts as the pointer
     * moves. The readout changes units for waveform, spectrum and spectrogram
     * views.
     *
     *****************************************************************************/
    void handleMotion(double x, double y)
    {
        hoverFileText_.clear();
        if (const auto label = labelAt(x, y); label.has_value()) {
            const Track* trackPtr = panel(label->first).track(label->second);
            if (trackPtr != nullptr) {
                const std::string fullPath = pathText(trackPtr->path());
                const unsigned int rate = trackPtr->sampleRate();
                if (trackPtr->type() == TrackType::ParameterText) {
                    hoverFileText_ = fullPath + "  |  Parameter file, Total frames = " +
                                     std::to_string(trackPtr->parameterFrameCount());
                } else {
                    // The rate is read from the header for a wav and comes from
                    // the Options menu setting for a raw stream
                    std::ostringstream info;
                    info.setf(std::ios::fixed);
                    info.precision(3);
                    info << fullPath << "  |  ";
                    if (trackPtr->isWav()) {
                        info << "WAV: " << rate << " Hz, "
                             << (!trackPtr->isStereoChannel()
                                     ? "Mono"
                                     : (trackPtr->channel() == 0 ? "Stereo (left channel)"
                                                                 : "Stereo (right channel)"))
                             << ", " << xsap::core::WavSampleFormatName(trackPtr->sampleFormat())
                             << ", ";
                    } else {
                        info << "Raw PCM: " << rate << " Hz, Mono, 16-bit PCM, ";
                    }
                    info << trackPtr->sampleCount() << " samples ("
                         << (rate == 0U ? 0.0
                                        : static_cast<double>(trackPtr->sampleCount()) /
                                              static_cast<double>(rate))
                         << " sec)";
                    hoverFileText_ = info.str();
                }
            }
        }
        coordinateText_.clear();
        for (bool upper : {true, false}) {
            const PanelLayout& layout = upper ? upperLayout_ : lowerLayout_;
            if (!layout.plot.contains(x, y)) {
                continue;
            }
            const Panel& source = panel(upper);
            double topHz = 0.0;
            unsigned int hoverRate = 0;
            const HoverAxis axis = hoverAxis(source, topHz, hoverRate);
            if (axis == HoverAxis::Amplitude && !panelHasWaveform(source)) {
                return;
            }
            const std::size_t pixel = static_cast<std::size_t>(
                std::clamp(x - layout.plot.x, 0.0, std::max(0.0, layout.plot.width - 1.0)));
            const auto sample = source.sampleIndexAtPixel(
                pixel, static_cast<std::size_t>(std::max(1.0, layout.plot.width)));
            if (sample.has_value()) {
                const SampleRange visible = source.visibleSampleRange();
                const int64_t xCoord = *sample - visible.begin;
                const double ratio = std::clamp(
                    (y - layout.plot.y) / std::max(1.0, layout.plot.height - 2.0), 0.0, 1.0);
                const int yCoord = static_cast<int>(
                    source.upperLimit() -
                    ratio * (source.upperLimit() - source.lowerLimit()));

                // The spectrum window plots two quite different pictures, so
                // the readout has to follow whichever is on screen. Sample
                // index against amplitude means nothing on either of them.
                if (axis == HoverAxis::SpectrumLine) {
                    // Amplitude against frequency: the horizontal axis is the
                    // spectrum itself.
                    const double across =
                        std::clamp((x - layout.plot.x) / std::max(1.0, layout.plot.width),
                                   0.0, 1.0);
                    const long hz = std::lround(across * topHz);
                    coordinateText_ = " (" + std::to_string(hz) + " Hz, " +
                                      std::to_string(yCoord) + " dB)";
                } else if (axis == HoverAxis::Spectrogram) {
                    // Frequency against time. Time is reported from the start
                    // of the file rather than the start of the view, because
                    // that is what the user is trying to locate.
                    const double up = std::clamp(
                        (layout.plot.y + layout.plot.height - y) /
                            std::max(1.0, layout.plot.height),
                        0.0, 1.0);
                    const long hz = std::lround(up * topHz);

                    const long totalMs =
                        hoverRate > 0
                            ? std::lround(static_cast<double>(*sample) * 1000.0 / hoverRate)
                            : 0L;
                    std::ostringstream stamp;
                    stamp.imbue(std::locale::classic());
                    stamp << std::setfill('0') << std::setw(2) << (totalMs / 3600000) << ':'
                          << std::setw(2) << ((totalMs / 60000) % 60) << ':' << std::setw(2)
                          << ((totalMs / 1000) % 60) << '.' << std::setw(3) << (totalMs % 1000);
                    coordinateText_ = " (" + stamp.str() + ", " + std::to_string(hz) + " Hz)";
                } else {
                    // Waveform: sample index against amplitude, as it always was.
                    coordinateText_ =
                        " (" + std::to_string(xCoord) + ", " + std::to_string(yCoord) + ")";
                }
            }
            updateStatusSegments();
            return;
        }
        updateStatusSegments();
    }

    /*****************************************************************************
     * handleKey
     *
     * Maps WinSap-compatible keyboard shortcuts to actions and navigation
     * helpers. Menu accelerators and direct plot focus therefore behave the
     * same way.
     *
     *****************************************************************************/
    bool handleKey(guint keyval, guint, Gdk::ModifierType state)
    {
        if ((state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{}) {
            return false;
        }
        const bool control = (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
        if (playbackJob_.active && !control &&
            (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
             keyval == GDK_KEY_Home || keyval == GDK_KEY_End)) {
            playbackCommandFeedback_ = "Stop playback before using commands.";
            updateStatusSegments();
            return true;
        }
        if (control) {
            if (keyval == GDK_KEY_F7) {
                application_->activate_action("zoom-in");
                return true;
            }
            if (keyval == GDK_KEY_F8) {
                application_->activate_action("zoom-out");
                return true;
            }
            if (keyval == GDK_KEY_F9) {
                application_->activate_action("view-all");
                return true;
            }
            if (keyval == GDK_KEY_l || keyval == GDK_KEY_L) {
                application_->activate_action("lock");
                return true;
            }
        }
        const auto moveByVisibleFrames = [this](int direction) {
            navigate(
                [direction](Panel& source, std::string& error) {
                    const std::size_t current = source.frameBegin();
                    const std::size_t step = source.visibleFrameCount();
                    const std::size_t target =
                        direction < 0
                            ? (current > step ? current - step : 0U)
                            : (current > std::numeric_limits<std::size_t>::max() - step
                                   ? std::numeric_limits<std::size_t>::max()
                                   : current + step);
                    return source.setFrameBegin(target, error);
                },
                direction < 0 ? "Moved backward by visible frames."
                              : "Moved forward by visible frames.");
        };
        const auto goToEnd = [this] {
            navigate(
                [](Panel& source, std::string& error) {
                    const std::size_t total = source.totalFrameCount();
                    const std::size_t visible = source.visibleFrameCount();
                    return source.setFrameBegin(total > visible ? total - visible : 0U, error);
                },
                "Moved to end.");
        };
        const char* action = nullptr;
        switch (keyval) {
        case GDK_KEY_F2:
            action = "save-as";
            break;
        case GDK_KEY_F3:
            action = "open-upper";
            break;
        case GDK_KEY_F4:
            action = "open-down";
            break;
        case GDK_KEY_F5:
            action = "play-all";
            break;
        case GDK_KEY_F6:
            action = "play-up";
            break;
        case GDK_KEY_F7:
            action = "play-down";
            break;
        case GDK_KEY_F8:
            action = "stop";
            break;
        case GDK_KEY_F9:
            action = "close-files";
            break;
        case GDK_KEY_Left:
            action = "backward";
            break;
        case GDK_KEY_Right:
            action = "forward";
            break;
        case GDK_KEY_Up:
            moveByVisibleFrames(-1);
            return true;
        case GDK_KEY_Down:
            moveByVisibleFrames(1);
            return true;
        case GDK_KEY_Home:
            navigate(
                [](Panel& source, std::string& error) {
                    return source.setFrameBegin(0, error);
                },
                "Moved to beginning.");
            return true;
        case GDK_KEY_End:
            goToEnd();
            return true;
        case GDK_KEY_Page_Up:
            action = "fast-backward";
            break;
        case GDK_KEY_Page_Down:
            action = "fast-forward";
            break;
        default:
            break;
        }
        if (action != nullptr) {
            application_->activate_action(action);
            return true;
        }
        return false;
    }
};

/*****************************************************************************
 * XsapApp
 *
 * Creates the private implementation that owns the GTK application state.
 * The public class stays small while the UI details remain in this file.
 *
 *****************************************************************************/
XsapApp::XsapApp()
    : impl_(std::make_unique<Impl>())
{
}

/*****************************************************************************
 * ~XsapApp
 *
 * Uses the default destruction path after the implementation has performed
 * its own cleanup.
 *
 *****************************************************************************/
XsapApp::~XsapApp() = default;

int XsapApp::run(int argc, char** argv)
{
    return impl_->run(argc, argv);
}

} // namespace xsap
