#ifndef WTC_TYPES_TD
#define WTC_TYPES_TD

include "mlir/IR/AttrTypeBase.td"
include "WTC/WTCDialect.td"

class WTC_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<WTC_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def WTC_MutexType : WTC_Type<"Mutex", "mutex"> {
  let summary = "An abstract, compiler-visible mutex handle";
  let description = [{
    Represents a mutex whose acquire/release operations are visible to the
    compiler as first-class IR operations.
  }];
}

def WTC_QueueType : WTC_Type<"Queue", "queue"> {
    let summary = "A WTC queue handle";
    let parameters = (ins "Type":$elementType);
    let assemblyFormat = "`<` $elementType `>`";
}

#endif // WTC_TYPES_TD