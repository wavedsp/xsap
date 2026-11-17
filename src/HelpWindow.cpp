/************************************************************************
 *
 *  WaveDSP - Help Topics window (GTK4)
 *
 *  FILE:      HelpWindow.cpp
 *
 *  PURPOSE:   Turns the help markup (see HelpWindow.h) into labels in a
 *             scrolled column: sized, coloured headings, bullets, notes
 *             and two-column shortcut tables whose every other row is
 *             shaded. Text wraps and tables reflow when the window is
 *             resized. Escape closes the window.
 *
 *  Update:    2026.11 Created
 *
 ************************************************************************/

#include "HelpWindow.h"

#include <memory>
#include <vector>

namespace wavedsp {

namespace {

std::unique_ptr<Gtk::Window> helpWindow;
Gtk::Box* helpColumn = nullptr;
Glib::RefPtr<Gtk::CssProvider> helpCss;

std::string CssColor(const Gdk::RGBA& color)
{
    return color.to_string();
}

// Escapes one line for Pango and turns **text** into bold.
Glib::ustring Inline(const std::string& text)
{
    Glib::ustring out;
    bool bold = false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t mark = text.find("**", start);
        out += Glib::Markup::escape_text(text.substr(start, mark == std::string::npos ? mark : mark - start));
        if (mark == std::string::npos) {
            break;
        }
        out += bold ? "</b>" : "<b>";
        bold = !bold;
        start = mark + 2;
    }
    if (bold) {
        out += "</b>";
    }
    return out;
}

Gtk::Label* MakeLabel(const std::string& text, const char* cssClass)
{
    auto* label = Gtk::make_managed<Gtk::Label>();
    label->set_markup(Inline(text));
    label->set_wrap(true);
    label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    label->set_xalign(0.0F);
    label->set_hexpand(true);
    label->set_selectable(false);
    label->add_css_class(cssClass);
    return label;
}

void ApplyTheme(const HelpTheme& theme)
{
    if (!helpCss) {
        helpCss = Gtk::CssProvider::create();
        Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), helpCss,
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    std::string css;
    css += "window.xhelp, window.xhelp scrolledwindow, window.xhelp viewport, .xhelp-page { background-color: " +
           CssColor(theme.background) + "; color: " + CssColor(theme.text) + "; }\n";
    css += ".xhelp-page label { color: " + CssColor(theme.text) + "; font-size: 11pt; }\n";
    css += ".xhelp-page label.xhelp-title { color: " + CssColor(theme.title) +
           "; font-size: 22pt; font-weight: bold; margin-bottom: 8px; }\n";
    css += ".xhelp-page label.xhelp-h2 { color: " + CssColor(theme.heading) +
           "; font-size: 15pt; font-weight: bold; margin-top: 16px; margin-bottom: 4px; }\n";
    css += ".xhelp-page label.xhelp-h3 { font-size: 12pt; font-weight: bold; margin-top: 10px; margin-bottom: 2px; }\n";
    css += ".xhelp-page label.xhelp-note { color: " + CssColor(theme.muted) + "; font-size: 10pt; font-style: italic; }\n";
    css += ".xhelp-page label.xhelp-key { color: " + CssColor(theme.heading) + "; font-weight: bold; font-size: 10pt; }\n";
    css += ".xhelp-page label.xhelp-value { font-size: 10pt; }\n";
    css += ".xhelp-cell { padding: 3px 6px; }\n";
    css += ".xhelp-shaded { background-color: " + CssColor(theme.rule) + "; }\n";
    helpCss->load_from_string(css);
}

// Rebuilds the page for the markup, replacing what the column held.
void Build(const std::string& markup)
{
    while (auto* child = helpColumn->get_first_child()) {
        helpColumn->remove(*child);
    }

    Gtk::Grid* table = nullptr;
    int row = 0;
    bool first = true;
    std::size_t start = 0;
    while (start < markup.size()) {
        std::size_t end = markup.find('\n', start);
        if (end == std::string::npos) {
            end = markup.size();
        }
        std::string line = markup.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            if (!table) {
                table = Gtk::make_managed<Gtk::Grid>();
                table->set_margin_top(4);
                table->set_margin_bottom(4);
                helpColumn->append(*table);
                row = 0;
            }
            auto* key = MakeLabel(line.substr(0, tab), "xhelp-key");
            auto* value = MakeLabel(line.substr(tab + 1), "xhelp-value");
            key->set_hexpand(false);
            key->set_width_chars(22);
            key->set_max_width_chars(28);
            value->set_valign(Gtk::Align::START);
            key->set_valign(Gtk::Align::START);
            for (Gtk::Label* label : {key, value}) {
                label->add_css_class("xhelp-cell");
                if (row % 2 == 1) {
                    label->add_css_class("xhelp-shaded");
                }
            }
            table->attach(*key, 0, row);
            table->attach(*value, 1, row);
            ++row;
            first = false;
            continue;
        }
        table = nullptr;

        Gtk::Widget* item = nullptr;
        if (line.compare(0, 2, "# ") == 0) {
            item = MakeLabel(line.substr(2), "xhelp-title");
        } else if (line.compare(0, 3, "## ") == 0) {
            item = MakeLabel(line.substr(3), "xhelp-h2");
            if (first) {
                item->set_margin_top(0);
            }
        } else if (line.compare(0, 4, "### ") == 0) {
            item = MakeLabel(line.substr(4), "xhelp-h3");
        } else if (line.compare(0, 2, "- ") == 0) {
            auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* bullet = Gtk::make_managed<Gtk::Label>(Glib::ustring("\u2022"));
            bullet->set_valign(Gtk::Align::START);
            bullet->set_margin_start(8);
            box->append(*bullet);
            box->append(*MakeLabel(line.substr(2), "xhelp-body"));
            box->set_margin_bottom(4);
            item = box;
        } else if (line.compare(0, 2, "> ") == 0) {
            item = MakeLabel(line.substr(2), "xhelp-note");
        } else {
            item = MakeLabel(line, "xhelp-body");
            item->set_margin_bottom(6);
        }
        helpColumn->append(*item);
        first = false;
    }
}

} // namespace

Gdk::RGBA HelpColor(const std::string& css)
{
    Gdk::RGBA color;
    color.set(css);
    return color;
}

void ShowHelp(Gtk::Window& owner, const Glib::ustring& caption, const std::string& markup, const HelpTheme& theme,
              const Glib::ustring& iconName)
{
    ApplyTheme(theme);
    if (!helpWindow) {
        helpWindow = std::make_unique<Gtk::Window>();
        helpWindow->add_css_class("xhelp");
        helpWindow->set_default_size(720, 800);
        helpWindow->set_size_request(380, 260);
        helpWindow->set_hide_on_close(true);

        auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroller->set_focusable(true);
        auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        column->add_css_class("xhelp-page");
        column->set_margin_start(24);
        column->set_margin_end(24);
        column->set_margin_top(16);
        column->set_margin_bottom(24);
        helpColumn = column;
        scroller->set_child(*column);
        helpWindow->set_child(*scroller);

        auto keys = Gtk::EventControllerKey::create();
        keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        keys->signal_key_pressed().connect(
            [](guint key, guint, Gdk::ModifierType) {
                if (key == GDK_KEY_Escape) {
                    helpWindow->set_visible(false);
                    return true;
                }
                return false;
            },
            false);
        helpWindow->add_controller(keys);
    }

    helpWindow->set_transient_for(owner);
    helpWindow->set_title(caption);
    if (!iconName.empty()) {
        helpWindow->set_icon_name(iconName);
    }
    Build(markup);
    helpWindow->present();
    if (auto* scroller = dynamic_cast<Gtk::ScrolledWindow*>(helpWindow->get_child())) {
        scroller->get_vadjustment()->set_value(0);
        scroller->grab_focus();
    }
}

} // namespace wavedsp
