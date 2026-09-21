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

#endif // WTC_ATTRS_TD
