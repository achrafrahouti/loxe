#include <QtTest>
#include <QApplication>
#include <QPalette>
#include <QStyle>

#include "ui/Theme.h"

// The bug this guards: the theme used to be a viewport-only flag, so the tree,
// docks, menus and status bar kept the desktop's colours and the window came up
// half dark. Everything must now follow one application palette.
class tst_Theme : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void keys_roundTrip();
    void keys_unknownFallsBack();

    void dark_appliesADarkApplicationPalette();
    void light_appliesALightApplicationPalette();
    void switchingBackAndForth_isStable();
    void system_restoresTheStartingPalette();

    void statusColours_flipWithTheTheme();
    void elementColour_readableInBothThemes();

private:
    // Contrast ratio between two colours, per WCAG. Used to assert that status
    // text stays legible against the background it is drawn on.
    static double contrast(const QColor& a, const QColor& b)
    {
        auto luminance = [](const QColor& c) {
            auto channel = [](double v) {
                v /= 255.0;
                return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(c.red())
                 + 0.7152 * channel(c.green())
                 + 0.0722 * channel(c.blue());
        };
        const double la = luminance(a), lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }
};

void tst_Theme::initTestCase()
{
    Theme::initialize(*qApp);
}

void tst_Theme::keys_roundTrip()
{
    for (auto mode : {Theme::Mode::System, Theme::Mode::Light, Theme::Mode::Dark})
        QCOMPARE(Theme::fromKey(Theme::toKey(mode)), mode);
}

void tst_Theme::keys_unknownFallsBack()
{
    QCOMPARE(Theme::fromKey(QString()), Theme::Mode::System);
    QCOMPARE(Theme::fromKey("nonsense", Theme::Mode::Dark), Theme::Mode::Dark);
    // Whatever case the settings file happens to use.
    QCOMPARE(Theme::fromKey(" DARK "), Theme::Mode::Dark);
}

void tst_Theme::dark_appliesADarkApplicationPalette()
{
    Theme::apply(Theme::Mode::Dark);
    QCOMPARE(Theme::currentMode(), Theme::Mode::Dark);
    QVERIFY(Theme::isDark());

    // The whole application, not just one widget.
    const QPalette pal = qApp->palette();
    QVERIFY(pal.color(QPalette::Window).lightness() < 128);
    QVERIFY(pal.color(QPalette::Base).lightness() < 128);
    QVERIFY(pal.color(QPalette::WindowText).lightness() > 128);
    QVERIFY(pal.color(QPalette::Text).lightness() > 128);
}

void tst_Theme::light_appliesALightApplicationPalette()
{
    Theme::apply(Theme::Mode::Light);
    QCOMPARE(Theme::currentMode(), Theme::Mode::Light);
    QVERIFY(!Theme::isDark());

    const QPalette pal = qApp->palette();
    QVERIFY(pal.color(QPalette::Window).lightness() > 128);
    QVERIFY(pal.color(QPalette::Base).lightness() > 128);
    QVERIFY(pal.color(QPalette::WindowText).lightness() < 128);
}

void tst_Theme::switchingBackAndForth_isStable()
{
    for (int i = 0; i < 3; ++i) {
        Theme::apply(Theme::Mode::Dark);
        QVERIFY(Theme::isDark());
        Theme::apply(Theme::Mode::Light);
        QVERIFY(!Theme::isDark());
    }
}

void tst_Theme::system_restoresTheStartingPalette()
{
    Theme::apply(Theme::Mode::Dark);
    QVERIFY(Theme::isDark());

    Theme::apply(Theme::Mode::System);
    QCOMPARE(Theme::currentMode(), Theme::Mode::System);
    // Back to whatever the platform gave us at startup, dark or light.
    QCOMPARE(Theme::isDark(),
             qApp->palette().color(QPalette::Window).lightness() < 128);
}

void tst_Theme::statusColours_flipWithTheTheme()
{
    Theme::apply(Theme::Mode::Dark);
    const QColor darkOk  = Theme::successColor();
    const QColor darkBad = Theme::errorColor();

    Theme::apply(Theme::Mode::Light);
    QVERIFY(Theme::successColor() != darkOk);
    QVERIFY(Theme::errorColor()   != darkBad);
}

void tst_Theme::elementColour_readableInBothThemes()
{
    // Element names are painted by the tree itself, so a fixed colour would be
    // unreadable in one theme or the other. 3:1 is the WCAG large-text floor.
    for (auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::apply(mode);
        const QColor bg = qApp->palette().color(QPalette::Base);
        const double ratio = contrast(Theme::elementColor(), bg);
        QVERIFY2(ratio >= 3.0,
                 qPrintable(QStringLiteral("contrast %1 in %2 mode")
                                .arg(ratio, 0, 'f', 2).arg(Theme::toKey(mode))));

        const double ok  = contrast(Theme::successColor(), bg);
        const double bad = contrast(Theme::errorColor(), bg);
        QVERIFY2(ok  >= 3.0, qPrintable(QStringLiteral("success contrast %1").arg(ok)));
        QVERIFY2(bad >= 3.0, qPrintable(QStringLiteral("error contrast %1").arg(bad)));
    }
}

QTEST_MAIN(tst_Theme)
#include "tst_Theme.moc"
