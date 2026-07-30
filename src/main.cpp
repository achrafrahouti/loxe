#include <QApplication>
#include <QCommandLineParser>
#include <QMetaType>
#include <QSettings>
#include <QTimer>

#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/AsyncLoader.h"
#include "ui/VirtualTreeModel.h"
#include "engine/MmapBuffer.h"
#include "engine/PieceTable.h"
#include "engine/SparseLineIndex.h"

int main(int argc, char* argv[])
{
    // Register custom types for cross-thread signal/slot connections
    qRegisterMetaType<MmapBuffer*>("MmapBuffer*");
    qRegisterMetaType<PieceTable*>("PieceTable*");
    qRegisterMetaType<SparseLineIndex*>("SparseLineIndex*");
    qRegisterMetaType<VirtualTreeModel*>("VirtualTreeModel*");

    QApplication app(argc, argv);
    app.setApplicationName("loxe");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("loxe");
    app.setOrganizationDomain("loxe.app");

    // Theme before any window exists, so the first frame is already consistent.
    // Applying it later is what produced a dark shell around a light editor.
    Theme::initialize(app);
    {
        QSettings settings;
        QString key = settings.value("theme").toString();
        if (key.isEmpty()) {
            // Migrate the old on/off setting.
            key = settings.value("darkTheme").toBool()
                ? Theme::toKey(Theme::Mode::Dark)
                : Theme::toKey(Theme::Mode::System);
        }
        Theme::apply(Theme::fromKey(key));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("High-performance XML editor");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption lineOpt("line", "Jump to line N on open.", "N");
    QCommandLineOption searchOpt("search", "Open and search for TERM.", "TERM");
    QCommandLineOption roOpt("ro", "Open file read-only.");
    // Renders the window to PATH and exits. Used by the UI smoke tests.
    QCommandLineOption shotOpt("screenshot", "Save a screenshot to PATH and exit.", "PATH");
    QCommandLineOption shotDelayOpt("screenshot-delay",
                                    "Milliseconds to wait before the screenshot (default 1500).",
                                    "MS");
    parser.addOption(lineOpt);
    parser.addOption(searchOpt);
    parser.addOption(roOpt);
    parser.addOption(shotOpt);
    parser.addOption(shotDelayOpt);
    parser.addPositionalArgument("file", "XML file to open.", "[file]");

    parser.process(app);

    MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openFile(args.first());

    // --line and --search act once the loader has produced a line index.
    if (parser.isSet(lineOpt)) {
        bool ok = false;
        const qulonglong line = parser.value(lineOpt).toULongLong(&ok);
        if (ok && line > 0)
            QTimer::singleShot(600, &window, [&window, line] { window.gotoLine(line - 1); });
    }
    if (parser.isSet(searchOpt)) {
        const QString term = parser.value(searchOpt);
        QTimer::singleShot(700, &window, [&window, term] { window.searchFor(term); });
    }

    if (parser.isSet(shotOpt)) {
        const QString path = parser.value(shotOpt);
        int delay = 1500;
        if (parser.isSet(shotDelayOpt)) {
            bool ok = false;
            const int v = parser.value(shotDelayOpt).toInt(&ok);
            if (ok && v >= 0) delay = v;
        }
        QTimer::singleShot(delay, &window, [&window, path] {
            const bool ok = window.grab().save(path);
            qInfo("screenshot %s: %s", ok ? "written" : "FAILED", qUtf8Printable(path));
            QCoreApplication::exit(ok ? 0 : 1);
        });
    }

    return app.exec();
}
