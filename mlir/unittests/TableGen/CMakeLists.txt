set(LLVM_TARGET_DEFINITIONS enums.td)
mlir_tablegen(EnumsGenTest.h.inc -gen-enum-decls)
mlir_tablegen(EnumsGenTest.cpp.inc -gen-enum-defs)
add_public_tablegen_target(MLIRTableGenEnumsIncGen)

set(LLVM_TARGET_DEFINITIONS passes.td)
mlir_tablegen(PassGenTest.h.inc -gen-pass-decls -name TableGenTest)
mlir_tablegen(PassGenTest.cpp.inc -gen-pass-defs -name TableGenTest)
add_public_tablegen_target(MLIRTableGenTestPassIncGen)

add_mlir_unittest(MLIRTableGenTests
  EnumsGenTest.cpp
  FormatTest.cpp
  OpBuildGen.cpp
  PassGenTest.cpp
)

add_dependencies(MLIRTableGenTests MLIRTableGenEnumsIncGen)
add_dependencies(MLIRTableGenTests MLIRTableGenTestPassIncGen)
add_dependencies(MLIRTableGenTests MLIRTestDialect)

target_include_directories(MLIRTableGenTests
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../test/lib/Dialect/Test
  ${CMAKE_CURRENT_BINARY_DIR}/../../test/lib/Dialect/Test
)

target_link_libraries(MLIRTableGenTests
  PRIVATE MLIRTableGen MLIRIR
  PUBLIC MLIRTestDialect
)
