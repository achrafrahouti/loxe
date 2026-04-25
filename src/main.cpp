#include <QApplication>
#include <QCommandLineParser>
#include <QMetaType>

#include "ui/MainWindow.h"
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

    QCommandLineParser parser;
    parser.setApplicationDescription("High-performance XML editor");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption lineOpt("line", "Jump to line N on open.", "N");
    QCommandLineOption searchOpt("search", "Open and search for TERM.", "TERM");
    QCommandLineOption roOpt("ro", "Open file read-only.");
    parser.addOption(lineOpt);
    parser.addOption(searchOpt);
    parser.addOption(roOpt);
    parser.addPositionalArgument("file", "XML file to open.", "[file]");

    parser.process(app);

    MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openFile(args.first());

    return app.exec();
}
