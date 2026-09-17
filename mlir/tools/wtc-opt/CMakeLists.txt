add_mlir_tool(wtc-opt
  wtc-opt.cpp
)

target_link_libraries(wtc-opt
  PRIVATE
  MLIRWTC
  MLIRWTCTransforms

  # ClangIR, exported by the Clang package
  MLIRCIR
  MLIRCIRInterfaces

  clangAST
  clangBasic
  clangLex

  MLIRIR
  MLIRParser
  MLIRSupport
  MLIRPass

  MLIRFuncDialect
  MLIRArithDialect
  MLIRSCFDialect
  MLIRMemRefDialect

  MLIRTransforms
  MLIRMlirOptMain
)