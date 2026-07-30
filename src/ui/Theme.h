#pragma once

#include <QColor>
#include <QString>

class QApplication;

// Application-wide light / dark theming.
//
// The editor viewport paints itself, so it has to be told which palette is in
// force. Everything else (tree, docks, menus, status bar) is themed by Qt from
// the application palette. Both must be driven from one place, or the window
// ends up half dark — which is what happens by default when the desktop theme
// is dark but the viewport is not told about it.
namespace Theme {

enum class Mode {
    System, // follow the desktop; the viewport matches whatever that turns out to be
    Light,
    Dark,
};

// Captures the style and palette the platform started with, so Mode::System can
// be restored exactly. Call once, right after the QApplication is constructed
// and before any window is created.
void initialize(QApplication& app);

// Applies `mode` to the whole application.
//
// Light and Dark force the Fusion style: platform styles (notably the GTK one)
// ignore most QPalette roles, so an explicit palette would be silently dropped
// and only the viewport would change colour.
void apply(Mode mode);

Mode currentMode();

// True when the palette actually in force is dark. Under Mode::System this is
// the only way to know what the desktop gave us, and it is what the viewport
// must follow.
bool isDark();

// Status colours with enough contrast against the current background.
QColor successColor();
QColor errorColor();
// Element names in the tree.
QColor elementColor();

// QSettings round-tripping.
QString  toKey(Mode mode);
Mode     fromKey(const QString& key, Mode fallback = Mode::System);

} // namespace Theme
