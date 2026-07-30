find_package(Qt6 6.4 REQUIRED COMPONENTS Test Widgets)

# Engine tests: link only the Qt-free engine plus QtTest.
function(add_loxe_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE Qt6::Test loxe_engine)
    add_test(NAME ${name} COMMAND ${name})
    # Qt still needs a platform plugin; CI runners have no display.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_loxe_test(tst_MmapBuffer)
add_loxe_test(tst_PieceTable)
add_loxe_test(tst_SparseLineIndex)
add_loxe_test(tst_FormatEngine)
add_loxe_test(tst_SearchEngine)
add_loxe_test(tst_XmlScanner)
add_loxe_test(tst_Encoding)

# UI tests compile in the widget sources they drive.
function(add_loxe_ui_test name)
    qt_add_executable(${name}
        ${name}.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/ViewportRenderer.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/IncrementalHighlighter.cpp
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Widgets loxe_engine)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_loxe_ui_test(tst_ViewportEditing)
