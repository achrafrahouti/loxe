find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

function(add_loxe_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Test loxe_engine)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_loxe_test(tst_MmapBuffer)
add_loxe_test(tst_PieceTable)
add_loxe_test(tst_SparseLineIndex)
add_loxe_test(tst_FormatEngine)
