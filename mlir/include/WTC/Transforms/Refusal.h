#ifndef WTC_TRANSFORMS_REFUSAL_H
#define WTC_TRANSFORMS_REFUSAL_H

// What a lifting pass decided about one candidate, and why, in one form for
// every idiom it lifts (docs/lifting.md). A candidate lifts when no reason
// refuses it; each reason names the condition of the proof it fails, (a)
// to (f), and says what the program does that the substitute would not.
// Every reason found is kept, so that a refusal says everything a program
// would have to change, not only the first thing the pass met.
//
// The remarks:
//
//   wtc: <idiom> <label> uses <feature> <count>, ...
//   wtc: <idiom> <label> not lifted: (<cond>) <text>        one per reason
//
// with idiom "locked queue" for a C++ class (LiftLockedQueues.cpp) and "C
// queue" for a C struct (LiftCQueues.cpp).

#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <map>
#include <string>
#include <vector>

namespace mlir::wtc {

struct Reason {
    // The condition's letter, "a" to "f", or empty for a reason outside
    // the conditions (a container type that cannot be named, say).
    std::string cond;
    std::string text;

    std::string str() const { return cond.empty() ? text : "(" + cond + ") " + text; }
    bool operator==(const Reason &o) const { return cond == o.cond && text == o.text; }
};

struct Verdict {
    std::string idiom;  // "locked queue", "C queue"
    std::string label;  // "q_ in LQ<int>", "struct queue"
    std::vector<Reason> reasons;
    // What the candidate was seen to use, counted: container operations,
    // wait forms (the "uses" remark).
    std::map<std::string, unsigned> features;

    bool refused() const { return !reasons.empty(); }

    // A reason, written "(x) text" when it names a condition; a reason
    // already recorded is not recorded again.
    void refuse(const llvm::Twine &why) {
        std::string s = why.str();
        Reason r;
        if (s.size() > 4 && s[0] == '(' && s[2] == ')' && s[3] == ' ' && s[1] >= 'a' && s[1] <= 'f') {
            r.cond = s.substr(1, 1);
            r.text = s.substr(4);
        } else {
            r.text = s;
        }
        if (!llvm::is_contained(reasons, r)) reasons.push_back(r);
    }

    // The first reason, as written; empty when nothing refuses.
    std::string first() const { return reasons.empty() ? std::string() : reasons.front().str(); }

    void emitUses(Location loc) const {
        if (features.empty()) return;
        InFlightDiagnostic d = emitRemark(loc) << "wtc: " << idiom << " " << label << " uses ";
        llvm::interleaveComma(features, d, [&](const auto &kv) { d << kv.first << " " << kv.second; });
    }
    void emitRefusals(Location loc) const {
        for (const Reason &r : reasons) emitRemark(loc) << "wtc: " << idiom << " " << label << " not lifted: " << r.str();
    }
    // A refusal outside the recorded reasons, found after the proof (when
    // the substitute itself cannot be built), in the same form.
    void emitRefusal(Location loc, const llvm::Twine &why) const {
        emitRemark(loc) << "wtc: " << idiom << " " << label << " not lifted: " << why;
    }
    void emitLifts(Location loc) const { emitRemark(loc) << "wtc: " << idiom << " " << label << " lifts"; }
};

}  // namespace mlir::wtc

#endif
