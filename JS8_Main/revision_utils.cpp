/**
 * @file revision_utils.cpp
 * @brief application versioning utility
 */

#include "revision_utils.h"

#include <QCoreApplication>

QString version() {
#if defined(CMAKE_BUILD)
    QString v{WSJTX_STRINGIZE(WSJTX_VERSION_MAJOR) "." WSJTX_STRINGIZE(
        WSJTX_VERSION_MINOR) "." WSJTX_STRINGIZE(WSJTX_VERSION_PATCH)};
#if 0
#if defined(WSJTX_RC)
    v += "-rc" WSJTX_STRINGIZE (WSJTX_RC)
#endif
#endif
#else
    QString v{"Not for Release"};
#endif

    return v;
}

QString program_title() {
#ifdef JS8_ASAN_BUILD
    // [asan] A sanitizer build must be unmistakable in the title -- it
    // is 2x slower and holds 2-3x the memory, so any timing or
    // resident-size number read from it means something different.
    return QString{"%1 \"Tranya\" (v4.1.0.479 ASAN) by WM8Q"}
        .arg(QCoreApplication::applicationName());
#else
    return QString{"%1 \"Tranya\" (v4.1.0.479 passband10) by WM8Q"}
        .arg(QCoreApplication::applicationName());
#endif
}

QString program_version() {
    return QString{"%1 v%2"}
        .arg(QCoreApplication::applicationName())
        .arg(QCoreApplication::applicationVersion());
}
