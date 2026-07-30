#include "Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace {

Theme::Mode g_mode = Theme::Mode::System;

// The style and palette the platform handed us, kept so Mode::System can be
// restored byte for byte rather than approximated.
QPalette g_systemPalette;
QString  g_systemStyle;
bool     g_initialized = false;

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x2B, 0x2F, 0x36);
    const QColor base(0x1E, 0x21, 0x27);       // matches the viewport background
    const QColor alt(0x24, 0x28, 0x2F);
    const QColor text(0xD8, 0xD8, 0xD8);
    const QColor disabled(0x6A, 0x70, 0x78);

    p.setColor(QPalette::Window,          window);
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            base);
    p.setColor(QPalette::AlternateBase,   alt);
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          window);
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      QColor(0xFF, 0x55, 0x55));
    p.setColor(QPalette::ToolTipBase,     alt);
    p.setColor(QPalette::ToolTipText,     text);
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x8A, 0x96));
    p.setColor(QPalette::Link,            QColor(0x6C, 0xB6, 0xFF));
    p.setColor(QPalette::LinkVisited,     QColor(0xD8, 0x9A, 0xE6));
    p.setColor(QPalette::Highlight,       QColor(0x2D, 0x4F, 0x76));
    p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));

    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(0x33, 0x37, 0x3F));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QPalette lightPalette()
{
    QPalette p;
    const QColor window(0xF0, 0xF0, 0xF0);
    const QColor text(0x1A, 0x1A, 0x1A);
    const QColor disabled(0x9A, 0x9A, 0x9A);

    p.setColor(QPalette::Window,          window);
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::AlternateBase,   QColor(0xF7, 0xF7, 0xF7));
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          window);
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      QColor(0xCC, 0x00, 0x00));
    p.setColor(QPalette::ToolTipBase,     QColor(0xFF, 0xFF, 0xDC));
    p.setColor(QPalette::ToolTipText,     text);
    p.setColor(QPalette::PlaceholderText, QColor(0x90, 0x90, 0x90));
    p.setColor(QPalette::Link,            QColor(0x00, 0x55, 0xAA));
    p.setColor(QPalette::LinkVisited,     QColor(0x88, 0x00, 0x88));
    p.setColor(QPalette::Highlight,       QColor(0x30, 0x8C, 0xC6));
    p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));

    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

} // namespace

namespace Theme {

void initialize(QApplication& app)
{
    if (g_initialized) return;
    g_systemPalette = app.palette();
    g_systemStyle   = app.style() ? app.style()->objectName() : QString();
    g_initialized   = true;
}

void apply(Mode mode)
{
    g_mode = mode;

    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;

    switch (mode) {
    case Mode::System:
        // Put back exactly what the platform gave us. isDark() then reports
        // whatever the desktop chose, and the viewport follows it.
        if (!g_systemStyle.isEmpty()) {
            if (QStyle* style = QStyleFactory::create(g_systemStyle))
                app->setStyle(style);
        }
        app->setPalette(g_systemPalette);
        break;

    case Mode::Light:
    case Mode::Dark:
        // Fusion honours QPalette on every platform; the GTK and Windows styles
        // ignore much of it, which would leave the chrome unthemed while the
        // viewport changed colour.
        if (QStyle* style = QStyleFactory::create(QStringLiteral("Fusion")))
            app->setStyle(style);
        app->setPalette(mode == Mode::Dark ? darkPalette() : lightPalette());
        break;
    }
}

Mode currentMode()
{
    return g_mode;
}

bool isDark()
{
    // Judge the palette in force rather than the requested mode, so Mode::System
    // on a dark desktop is correctly reported as dark.
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor successColor()
{
    return isDark() ? QColor(0x7E, 0xD3, 0x21) : QColor(0x06, 0x7D, 0x17);
}

QColor errorColor()
{
    return isDark() ? QColor(0xFF, 0x7B, 0x72) : QColor(0xCC, 0x00, 0x00);
}

QColor elementColor()
{
    return isDark() ? QColor(0x6C, 0xB6, 0xFF) : QColor(0x00, 0x55, 0xAA);
}

QString toKey(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark:  return QStringLiteral("dark");
    case Mode::System:
    default:          return QStringLiteral("system");
    }
}

Mode fromKey(const QString& key, Mode fallback)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("light")) return Mode::Light;
    if (k == QLatin1String("dark"))  return Mode::Dark;
    if (k == QLatin1String("system")) return Mode::System;
    return fallback;
}

} // namespace Theme
