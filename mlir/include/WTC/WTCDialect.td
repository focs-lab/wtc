#ifndef WTC_DIALECT_TD
#define WTC_DIALECT_TD

include "mlir/IR/DialectBase.td"

def WTC_Dialect : Dialect {
    let name = "wtc";
    let summary = "First-class concurrency primitives for the Well-tempered Compiler";
    let description = [{A dialect that gives MLIR first-class knowledge of concurrency semantics: queues, locks and the threads that own them.}];
    let cppNamespace = "::mlir::wtc";
    let useDefaultTypePrinterParser = 1;
    let extraClassDeclaration = [{
      void registerTypes();
    }];
}

#endif // WTC_DIALECT_TD