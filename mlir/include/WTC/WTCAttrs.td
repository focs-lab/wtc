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
def WTC_ImplMPSC       : I32EnumAttrCase<"mpsc", 3>;
def WTC_ImplSPMC       : I32EnumAttrCase<"spmc", 4>;
// Chosen only for a path's progress requirement (WTC/ProgressTable.h,
// shim/topology.hpp): the bounded rings of indices, lock-free (scq) and
// wait-free (wcq); the mpsc queue whose push takes its index from one
// counter (jiffy); and the settled bounded rings, whose single side
// publishes its counter only once its cell is filled or emptied. The tag a
// constructor is given for one of them is the one the runtime announces
// (WTCSpecialize.cpp, runtimeOffers), not the number here.
def WTC_ImplSCQ        : I32EnumAttrCase<"scq", 5>;
def WTC_ImplWCQ        : I32EnumAttrCase<"wcq", 6>;
def WTC_ImplJiffy      : I32EnumAttrCase<"jiffy", 7>;
def WTC_ImplSPSCSettled : I32EnumAttrCase<"spsc_settled", 8>;
def WTC_ImplMPSCSettled : I32EnumAttrCase<"mpsc_settled", 9>;
def WTC_ImplSPMCSettled : I32EnumAttrCase<"spmc_settled", 10>;

def WTC_ImplEnum : I32EnumAttr<"Impl", "the implementation a queue object runs",
    [WTC_ImplGeneric, WTC_ImplSPSC, WTC_ImplSPSCInline, WTC_ImplMPSC, WTC_ImplSPMC,
     WTC_ImplSCQ, WTC_ImplWCQ, WTC_ImplJiffy, WTC_ImplSPSCSettled, WTC_ImplMPSCSettled,
     WTC_ImplSPMCSettled]> {
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
