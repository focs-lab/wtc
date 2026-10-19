#ifndef WTC_INTERFACES_TD
#define WTC_INTERFACES_TD

include "mlir/IR/OpBase.td"

// What an operation promises about progress as the implementation chosen
// for it runs it (decisions/0004). Two facts, kept apart because they fail
// differently: whether the operation's own algorithm can wait for another
// thread, and whether it may call the allocator, which can.
def WTC_ProgressOpInterface : OpInterface<"ProgressOpInterface"> {
  let cppNamespace = "::mlir::wtc";
  let description = [{
    Implemented by every wtc operation. The answers depend on the
    implementation recorded on the operation (#wtc.impl); without one, the
    general-purpose implementation is assumed, which is the answer that is
    never too optimistic.
  }];
  let methods = [
    InterfaceMethod<"The progress the operation's own algorithm guarantees.",
                    "::mlir::wtc::Progress", "getProgress">,
    InterfaceMethod<"Whether the operation may allocate or free memory.",
                    "bool", "mayCallAllocator">,
    InterfaceMethod<"In words, why the operation may block, for a diagnostic; "
                    "empty when it cannot.",
                    "std::string", "getBlockingReason">,
  ];
}

#endif // WTC_INTERFACES_TD
