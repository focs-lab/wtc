add_mlir_tool(wtc-link
  wtc-link.cpp
)

target_link_libraries(wtc-link
  PRIVATE
  MLIRWTC

  MLIRCIR
  MLIRCIRInterfaces

  clangAST
  clangBasic
  clangLex

  MLIRIR
  MLIRParser
  MLIRSupport

  MLIRFuncDialect
  MLIRArithDialect
  MLIRSCFDialect
  MLIRMemRefDialect
  MLIRRegisterAllDialects
)
