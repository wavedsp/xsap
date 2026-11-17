/************************************************************************
 *
 *  XSap - Speech Analysis Program for Linux, GTKMM 4 port of WinSap
 *
 *  FILE:      main.cpp
 *
 *  PURPOSE:   Process entry point. Applies the environment workarounds a
 *             WSLg session needs before GTK initialises, then hands control
 *             to XsapApp.
 *
 *  Update:    2026.1
 *
 ************************************************************************/

#include "XsapApp.h"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace {

void configureWslGraphics()
{
    const bool runningUnderWsl = std::getenv("WSL_DISTRO_NAME") != nullptr ||
                                 std::getenv("WSL_INTEROP") != nullptr;
    if (!runningUnderWsl) {
        return;
    }
    if (std::getenv("GDK_BACKEND") == nullptr &&
        ::setenv("GDK_BACKEND", "x11", 0) != 0) {
        std::cerr << "xsap: warning: unable to select the WSL-safe X11 backend (errno "
                  << errno << ").\n";
    }
    if (std::getenv("GSK_RENDERER") == nullptr &&
        ::setenv("GSK_RENDERER", "cairo", 0) != 0) {
        std::cerr << "xsap: warning: unable to select the WSL-safe Cairo renderer (errno "
                  << errno << ").\n";
    }
    if (std::getenv("LIBGL_ALWAYS_SOFTWARE") == nullptr &&
        ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0) != 0) {
        std::cerr << "xsap: warning: unable to disable WSL hardware GL probing (errno "
                  << errno << ").\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    configureWslGraphics();
    try {
        xsap::XsapApp app;
        return app.run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "xsap: " << error.what() << '\n';
        return 1;
    }
}
