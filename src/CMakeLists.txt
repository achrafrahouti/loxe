# Engine compiled as a static library so tests can link against it without Qt.
add_library(loxe_engine STATIC
    engine/Encoding.cpp
    engine/FormatEngine.cpp
    engine/MmapBuffer.cpp
    engine/PieceTable.cpp
    engine/SearchEngine.cpp
    engine/SparseLineIndex.cpp
    engine/Validator.cpp
    engine/XmlScanner.cpp
)
target_include_directories(loxe_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(loxe_engine PUBLIC LibXml2::LibXml2)

qt_add_executable(loxe
    main.cpp
    ui/AsyncLoader.cpp
    ui/FindBar.cpp
    ui/IncrementalHighlighter.cpp
    ui/MainWindow.cpp
    ui/ViewportRenderer.cpp
    ui/VirtualTreeModel.cpp
    ui/XmlContext.cpp
)

target_link_libraries(loxe PRIVATE
    loxe_engine
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
)

set_target_properties(loxe PROPERTIES
    WIN32_EXECUTABLE FALSE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist"
)
