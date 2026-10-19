#ifndef WTC_ATTRS_TD
#define WTC_ATTRS_TD

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"
include "WTC/WTCDialect.td"

// What the ownership analysis proved about a queue object, or failed to.
// `unknown` is a verdict too, and the important one: it is what every
// unresolved case maps to, and it means the general-purpose implementation.
def WTC_TopologyUnknown : I32EnumAttrCase<"unknown", 0>;
def WTC_TopologySPSC    : I32EnumAttrCase<"spsc", 1>;
def WTC_TopologyMPSC    : I32EnumAttrCase<"mpsc", 2>;
def WTC_TopologySPMC    : I32EnumAttrCase<"spmc", 3>;
def WTC_TopologyMPMC    : I32EnumAttrCase<"mpmc", 4>;

def WTC_TopologyEnum : I32EnumAttr<"Topology", "producer/consumer topology of a queue",
    [WTC_TopologyUnknown, WTC_TopologySPSC, WTC_TopologyMPSC,
     WTC_TopologySPMC, WTC_TopologyMPMC]> {
  let cppNamespace = "::mlir::wtc";
  let genSpecializedAttr = 0;
}

// Printed as #wtc.topology<spsc>.
def WTC_TopologyAttr : EnumAttr<WTC_Dialect, WTC_TopologyEnum, "topology"> {
  let assemblyFormat = "`<` $value `>`";
}

// Which implementation a queue object runs: the shim's topology tag, as its
// constructor receives it (shim/topology.hpp). An operation carries the
// implementation of the one queue it acts on; without the attribute it is
// the general-purpose one, which is the safe reading for any pass.
def WTC_ImplGeneric    : I32EnumAttrCase<"generic", 0>;
def WTC_ImplSPSC       : I32EnumAttrCase<"spsc", 1>;
def WTC_ImplSPSCInline : I32EnumAttrCase<"spsc_inline", 2>;

def WTC_ImplEnum : I32EnumAttr<"Impl", "the implementation a queue object runs",
    [WTC_ImplGeneric, WTC_ImplSPSC, WTC_ImplSPSCInline]> {
  let cppNamespace = "::mlir::wtc";
  let genSpecializedAttr = 0;
}

// What an operation's algorithm guarantees about progress
// (ProgressOpInterface, decisions/0004).
def WTC_ProgressBlocking : I32EnumAttrCase<"blocking", 0>;
def WTC_ProgressLockFree : I32EnumAttrCase<"lock_free", 1>;
def WTC_ProgressWaitFree : I32EnumAttrCase<"wait_free", 2>;

def WTC_ProgressEnum : I32EnumAttr<"Progress", "the progress an operation guarantees",
    [WTC_ProgressBlocking, WTC_ProgressLockFree, WTC_ProgressWaitFree]> {
  let cppNamespace = "::mlir::wtc";
  let genSpecializedAttr = 0;
}

// Printed as #wtc.impl<spsc>.
def WTC_ImplAttr : EnumAttr<WTC_Dialect, WTC_ImplEnum, "impl"> {
  let assemblyFormat = "`<` $value `>`";
}

#endif // WTC_ATTRS_TD
