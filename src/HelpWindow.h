/************************************************************************
 *
 *  WaveDSP - Help Topics window (GTK4)
 *
 *  FILE:      HelpWindow.h
 *
 *  PURPOSE:   A resizable, modeless window that shows an application's help
 *             with sized, coloured headings and shortcut tables. xmdv, xsap,
 *             xplayer and xvista carry identical copies; it is the GTK
 *             counterpart of helpwindow.h in WinMDV, WinSap, WPlayer and
 *             WVista.
 *
 *  Update:    2026.11 Created
 *
 ************************************************************************/

#pragma once

#include <gtkmm.h>

#include <string>

namespace wavedsp {

struct HelpTheme {
    Gdk::RGBA background;
    Gdk::RGBA text;
    Gdk::RGBA title;   // the "# " title
    Gdk::RGBA heading; // "## " sections and the keys in shortcut tables
    Gdk::RGBA muted;   // "> " notes
    Gdk::RGBA rule;    // shading of every other shortcut row
    bool dark = false;
};

// Parses "#rrggbb" or any other CSS colour.
Gdk::RGBA HelpColor(const std::string& css);

// Opens the help window, or brings the open one forward with the new text and
// colours. The markup has one item per line:
//   "# text"        title                 "## text"   section heading
//   "### text"      subheading            "- text"    bullet
//   "> text"        note                  "keys\ttext" shortcut row; adjacent rows form a table
//   anything else is a paragraph; blank lines are ignored. "**text**" is bold.
// Escape closes the window.
void ShowHelp(Gtk::Window& owner, const Glib::ustring& caption, const std::string& markup, const HelpTheme& theme,
              const Glib::ustring& iconName = {});

} // namespace wavedsp
