/************************************************************************
 *
 *  XSap - Main Application Header
 *
 *  FILE:      XsapApp.h
 *
 *  PURPOSE:   The application class that owns the main window, the panels
 *             and the UI. Counterpart of WinSap src/x64/WinsapApp.h.
 *
 *             Everything is hidden behind a pimpl so that the GTKMM headers
 *             stay out of the rest of the program.
 *
 *  Update:    2026.1
 *
 ************************************************************************/

#pragma once

#include <memory>

namespace xsap {

class XsapApp {
public:
    XsapApp();
    ~XsapApp();

    XsapApp(const XsapApp&) = delete;
    XsapApp& operator=(const XsapApp&) = delete;

    int run(int argc, char** argv);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xsap
