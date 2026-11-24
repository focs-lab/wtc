#ifndef WTC_INTERFACES_TD
#define WTC_INTERFACES_TD

include "mlir/IR/OpBase.td"

// What an operation promises about progress as the implementation chosen
// for it runs it (decisions/0004, docs/progress.md): a specification under
// the thread counts proved for its object, and, read from it under no
// counts at all, the two facts the progress check and the batching passes
// started with -- whether the operation's own algorithm can wait for
// another thread, and whether it may call the allocator, which can.
def WTC_ProgressOpInterface : OpInterface<"ProgressOpInterface"> {
  let cppNamespace = "::mlir::wtc";
  let description = [{
    Implemented by every wtc operation, from one table
    (WTC/ProgressTable.h). The answers depend on the
    implementation recorded on the operation (#wtc.impl); without one, the
    general-purpose implementation is assumed, which is the answer that is
    never too optimistic.
  }];
  let methods = [
    InterfaceMethod<"What the operation guarantees under these thread counts "
                    "(WTC/Progress.h): its level, its bound, whether it "
                    "allocates, enters the kernel or runs the element's code.",
                    "::mlir::wtc::ProgressSpec", "getProgressSpec",
                    (ins "const ::mlir::wtc::ThreadCounts &":$counts)>,
    InterfaceMethod<"The progress the operation's own algorithm guarantees, "
                    "with nothing known of the counts.",
                    "::mlir::wtc::Progress", "getProgress", (ins), [{}], [{
      ::mlir::wtc::ProgressSpec s =
          $_op.getProgressSpec(::mlir::wtc::ThreadCounts::unknown());
      if (s.blocks()) return ::mlir::wtc::Progress::blocking;
      return s.level == ::mlir::wtc::Level::wait_free
                 ? ::mlir::wtc::Progress::wait_free
                 : ::mlir::wtc::Progress::lock_free;
    }]>,
    InterfaceMethod<"Whether the operation may allocate or free memory.",
                    "bool", "mayCallAllocator", (ins), [{}], [{
      return $_op.getProgressSpec(::mlir::wtc::ThreadCounts::unknown())
          .allocates();
    }]>,
    InterfaceMethod<"In words, why the operation may block, for a diagnostic; "
                    "empty when it cannot.",
                    "std::string", "getBlockingReason", (ins), [{}], [{
      return $_op.getProgressSpec(::mlir::wtc::ThreadCounts::unknown()).reason;
    }]>,
  ];
}

#endif // WTC_INTERFACES_TD
