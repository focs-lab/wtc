#include "WTC/Transforms/EscapeAnalysis.h"
#include "WTC/Transforms/LiftNames.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cstdlib>
#include <vector>

using namespace mlir;
using namespace mlir::wtc;

namespace {

constexpr unsigned kNone = ~0u;

// Union-find over value classes, each with the class of what its values
// point to and the functions whose address it holds.
class Classes {
public:
    Classes() {
        // The outside: points to itself.
        unsigned o = fresh();
        ext[o] = true;
        pts[o] = o;
        vpt[o] = o;
    }
    static constexpr unsigned outside = 0;

    unsigned fresh() {
        parent.push_back(parent.size());
        pts.push_back(kNone);
        ext.push_back(false);
        opq.push_back(false);
        vpt.push_back(kNone);
        size.push_back(1);
        funcs.emplace_back();
        return parent.size() - 1;
    }
    unsigned find(unsigned x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    bool external(unsigned x) { return ext[find(x)]; }
    // Whether the class may hold the address of a function the module
    // does not have: a virtual function of a class whose vtable is only
    // declared here.
    bool opaque(unsigned x) { return opq[find(x)]; }
    void setOpaque(unsigned x) { opq[find(x)] = true; }
    unsigned outsideSize() { return size[find(outside)]; }
    // With WTC_ESCAPE_DEBUG set, every join is kept, labelled with the
    // operation that made it, to explain a verdict.
    bool debug = getenv("WTC_ESCAPE_DEBUG") != nullptr;
    Operation *current = nullptr;
    std::vector<std::tuple<unsigned, unsigned, Operation *>> edges;
    void join(unsigned a, unsigned b) {
        llvm::SmallVector<std::pair<unsigned, unsigned>, 8> work{{a, b}};
        while (!work.empty()) {
            auto [x, y] = work.pop_back_val();
            if (debug) edges.push_back({x, y, current});
            x = find(x);
            y = find(y);
            if (x == y) continue;
            if (size[x] < size[y]) std::swap(x, y);
            parent[y] = x;
            size[x] += size[y];
            ext[x] = ext[x] || ext[y];
            opq[x] = opq[x] || opq[y];
            for (Operation *f : funcs[y]) funcs[x].insert(f);
            funcs[y].clear();
            const unsigned px = pts[x], py = pts[y];
            if (px == kNone) pts[x] = py;
            else if (py != kNone) work.push_back({px, py});
            const unsigned vx = vpt[x], vy = vpt[y];
            if (vx == kNone) vpt[x] = vy;
            else if (vy != kNone) work.push_back({vx, vy});
        }
    }
    // The class of what the values of x's class point to.
    unsigned content(unsigned x) {
        unsigned r = find(x);
        if (pts[r] == kNone) {
            unsigned c = fresh();
            pts[r] = c;
        }
        return find(pts[r]);
    }
    llvm::SmallVector<Operation *, 4> functions(unsigned x) {
        auto &s = funcs[find(x)];
        return llvm::SmallVector<Operation *, 4>(s.begin(), s.end());
    }
    void addFunction(unsigned x, Operation *f) { funcs[find(x)].insert(f); }
    // What x's class points to, and its vptr's class, if any.
    unsigned pointee(unsigned x) {
        unsigned p = pts[find(x)];
        return p == kNone ? kNone : find(p);
    }
    unsigned vpointee(unsigned x) {
        unsigned p = vpt[find(x)];
        return p == kNone ? kNone : find(p);
    }
    // The class of the address of the vptr of the objects x's values point to.
    unsigned vptrOf(unsigned x) {
        unsigned r = find(x);
        if (vpt[r] == kNone) {
            unsigned c = fresh();
            vpt[r] = c;
        }
        return find(vpt[r]);
    }

private:
    std::vector<unsigned> parent, pts, vpt, size;
    std::vector<bool> ext, opq;
    std::vector<llvm::SmallSetVector<Operation *, 2>> funcs;
};

// What a library function with no body here does with the pointers it is
// handed. The list is deny-by-default: a function not on it, and any
// function of the list's name that a linked file defines (the program's
// own sem_* of another file, say), may keep or hand on what it is
// handed, and what it returns is the outside's. Each entry says what the
// C library or libstdc++ does with its pointers, and nothing more is
// assumed of it.
enum class Lib {
    Unknown,
    Fresh,      // returns new storage, keeps nothing (malloc, fopen)
    Inert,      // reads and writes what it is handed, keeps nothing, hands nothing back
    FirstArg,   // as Inert, and returns its first argument (memset, strchr)
    Copy,       // copies the second argument's memory into the first's, returns the first
    EndPtr,     // strto*: stores into *arg1 a pointer into arg0, and returns the number arg0 holds
    FromString, // ato*: returns the number its argument's memory holds
    PutValue,   // putchar, putc, fputc: writes argument `arg` itself to a file
    Insert,     // a stream insertion: writes argument 1, and what it points to, to a stream
    Structure,  // links its arguments' memory into each other (libstdc++'s tree and list nodes, strings)
    Print,      // formats its variadic arguments into a file: what it formats reaches the outside
    PrintTo,    // formats them into the memory of its first argument
    Scan,       // parses a file into what its variadic arguments point to
    ScanFrom,   // parses its first argument's memory into them
    WriteOut,   // writes the memory of argument `arg` to a file (write, fwrite, puts)
    ReadIn,     // reads a file into the memory of argument `arg` (read, fread, fgets)
    Callback,   // registers a function, called later with argument `arg` as parameter `param`
    ThreadStart,
    ThreadJoin,  // pthread_join: stores a thread's result into *arg1
    ThreadExit,  // pthread_exit: the argument is the thread's result
};

struct LibModel {
    Lib kind = Lib::Unknown;
    unsigned fn = 0, arg = 0, param = 0;  // Callback; WriteOut and ReadIn use `arg`; formats use `arg` for the format
    bool hasArg = false;
};

LibModel libraryModel(cir::FuncOp g) {
    const StringRef n = g.getSymName();
    LibModel m;
    auto is = [&](std::initializer_list<const char *> names) {
        for (const char *x : names)
            if (n == x) return true;
        return false;
    };
    auto kind = [&](Lib k, unsigned arg = 0) {
        m.kind = k;
        m.arg = arg;
        return m;
    };
    auto callback = [&](unsigned fn, std::optional<unsigned> arg, unsigned param = 0) {
        m.kind = Lib::Callback;
        m.fn = fn;
        m.hasArg = arg.has_value();
        m.arg = arg.value_or(0);
        m.param = param;
        return m;
    };
    // Threads and exit handlers: the callback has the argument as a
    // parameter (on_exit's second, after the exit status).
    if (n == "pthread_create") return callback(2, 3);
    if (is({"__cxa_atexit", "__cxa_thread_atexit", "__cxa_thread_atexit_impl"})) return callback(0, 1);
    if (n == "on_exit") return callback(0, 1, 1);
    if (is({"atexit", "at_quick_exit"})) return callback(0, std::nullopt);
    if (n == "pthread_once") return callback(1, std::nullopt);
    if (n == "pthread_join") return kind(Lib::ThreadJoin);
    if (n == "pthread_exit") return kind(Lib::ThreadExit);
    if (n == "_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE")
        return kind(Lib::ThreadStart);
    // Fresh storage.
    if (isAllocator(g) || is({"__cxa_allocate_exception", "fopen", "fdopen", "tmpfile", "__errno_location",
                              "__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc", "strerror", "getenv"}))
        return kind(Lib::Fresh);
    if (is({"strdup", "strndup"})) return kind(Lib::Copy);  // the copy's memory is the argument's
    // Memory and strings.
    if (is({"memcpy", "memmove", "strcpy", "strncpy", "strcat", "strncat", "stpcpy", "__memcpy_chk",
            "__memmove_chk", "__strcpy_chk", "__strcat_chk"}))
        return kind(Lib::Copy);
    if (is({"memset", "__memset_chk", "strchr", "strrchr", "strstr", "memchr", "strpbrk", "realloc"}))
        return kind(Lib::FirstArg);
    if (is({"atoi", "atol", "atoll", "atof"})) return kind(Lib::FromString);
    if (is({"putchar"})) return kind(Lib::PutValue, 0);
    if (is({"putc", "fputc"})) return kind(Lib::PutValue, 0);
    if (is({"strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "strtold", "__isoc23_strtol",
            "__isoc23_strtoul", "__isoc23_strtoll", "__isoc23_strtoull"}))
        return kind(Lib::EndPtr);
    // Formatted input and output, and files.
    if (n == "printf" || n == "vprintf") return kind(Lib::Print, 0);
    if (is({"fprintf", "dprintf", "__printf_chk"})) return kind(Lib::Print, 1);
    if (n == "__fprintf_chk") return kind(Lib::Print, 2);
    if (n == "sprintf") return kind(Lib::PrintTo, 1);
    if (n == "snprintf") return kind(Lib::PrintTo, 2);
    if (n == "__sprintf_chk") return kind(Lib::PrintTo, 3);
    if (n == "__snprintf_chk") return kind(Lib::PrintTo, 4);
    if (is({"scanf", "__isoc99_scanf", "__isoc23_scanf"})) return kind(Lib::Scan, 0);
    if (is({"fscanf", "__isoc99_fscanf", "__isoc23_fscanf"})) return kind(Lib::Scan, 1);
    if (is({"sscanf", "__isoc99_sscanf", "__isoc23_sscanf"})) return kind(Lib::ScanFrom, 1);
    if (is({"puts", "fputs", "perror"})) return kind(Lib::WriteOut, 0);
    if (is({"fwrite"})) return kind(Lib::WriteOut, 0);
    if (is({"write"})) return kind(Lib::WriteOut, 1);
    if (is({"read", "fgets"})) return kind(Lib::ReadIn, n == "read" ? 1 : 0);
    if (is({"fread"})) return kind(Lib::ReadIn, 0);
    // libstdc++'s strings and node-based containers link and copy the
    // memory of their arguments; its streams format what they are handed.
    if (n.starts_with("_ZNSt7__cxx1112basic_string") || n.starts_with("_ZNKSt7__cxx1112basic_string") ||
        n.starts_with("_ZSt28_Rb_tree_insert_and_rebalance") || n.starts_with("_ZSt29_Rb_tree_rebalance_for_erase") ||
        n.starts_with("_ZSt18_Rb_tree_increment") || n.starts_with("_ZSt18_Rb_tree_decrement") ||
        n.starts_with("_ZNSt8__detail15_List_node_base"))
        return kind(Lib::Structure);
    if (n.starts_with("_ZNSolsE") || n.starts_with("_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_") ||
        n == "_ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_l" ||
        n == "_ZNSo5writeEPKcl" || n == "_ZNSo3putEc" || n == "_ZNSo5flushEv")
        return kind(Lib::Insert);  // the stream, then what it writes
    // Functions that keep and hand back nothing of what they are handed.
    if (is({"free", "rte_free", "__cxa_free_exception", "memcmp", "strcmp", "strncmp", "strlen", "strnlen",
            "strcasecmp", "strncasecmp", "strspn", "strcspn", "fflush", "fclose", "fileno", "feof", "ferror", "open", "close", "lseek", "pipe", "unlink",
            "abort", "exit", "_exit", "quick_exit", "__assert_fail", "__stack_chk_fail", "time", "clock",
            "clock_gettime", "gettimeofday", "sleep", "usleep", "nanosleep", "sched_yield", "sched_getcpu",
            "sysconf", "get_nprocs", "getpid", "rand", "srand", "rand_r", "random", "srandom", "sqrt", "sqrtf",
            "pow", "exp", "log", "log2", "log10", "floor", "ceil", "fabs", "fmod", "sin", "cos", "abs", "labs",
            "llabs", "toupper", "tolower", "isalpha", "isdigit", "isspace", "isalnum", "isupper", "islower",
            "isprint", "isxdigit", "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_trylock",
            "pthread_mutex_unlock", "pthread_mutex_destroy", "pthread_cond_init", "pthread_cond_wait",
            "pthread_cond_timedwait", "pthread_cond_clockwait", "pthread_cond_signal", "pthread_cond_broadcast", "pthread_cond_destroy",
            "pthread_mutexattr_init", "pthread_mutexattr_settype", "pthread_mutexattr_destroy",
            "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setdetachstate", "pthread_attr_setstacksize",
            "pthread_spin_init", "pthread_spin_lock", "pthread_spin_unlock", "pthread_spin_destroy",
            "pthread_rwlock_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
            "pthread_rwlock_destroy", "pthread_barrier_init", "pthread_barrier_wait", "pthread_barrier_destroy",
            "pthread_self", "pthread_equal", "pthread_detach", "pthread_setaffinity_np", "pthread_getaffinity_np",
            "sem_init", "sem_wait", "sem_trywait", "sem_post", "sem_destroy", "sem_getvalue", "__cxa_guard_acquire",
            "__cxa_guard_release", "__cxa_guard_abort", "__cxa_pure_virtual", "__cxa_end_catch",
            "_Unwind_Resume", "__gxx_personality_v0",
            // libstdc++: condition variables, threads, time, the throw helpers.
            "_ZNSt18condition_variableC1Ev", "_ZNSt18condition_variableD1Ev",
            "_ZNSt18condition_variable10notify_oneEv", "_ZNSt18condition_variable10notify_allEv",
            "_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE", "_ZNSt6thread4joinEv",
            "_ZNSt6thread6detachEv", "_ZNSt6thread20hardware_concurrencyEv", "_ZNSt6thread6_StateD0Ev",
            "_ZNSt6thread6_StateD1Ev", "_ZNSt6thread6_StateD2Ev",
            "_ZSt9terminatev", "_ZNSt6chrono3_V212system_clock3nowEv", "_ZNSt6chrono3_V212steady_clock3nowEv",
            "_ZNSt8ios_base4InitC1Ev", "_ZNSt8ios_base4InitD1Ev", "_ZdlPv", "_ZdlPvm", "_ZdaPv", "_ZdaPvm",
            "_ZdlPvSt11align_val_t", "_ZdlPvmSt11align_val_t", "_ZdaPvSt11align_val_t", "_ZdaPvmSt11align_val_t",
            "_ZSt21__glibcxx_assert_failPKciS0_S0_", "_ZNSt9exceptionD2Ev"}) ||
        n.starts_with("_ZNSt11this_thread11__sleep_for") ||
        (n.starts_with("_ZSt") && n.contains("__throw_") && !n.contains("system_error")))
        return kind(Lib::Inert);
    return m;
}

// Whether an operation comes from the system's headers (the C library's,
// libstdc++'s, the compiler's own).
bool systemCode(Location loc) {
    while (auto f = dyn_cast<FusedLoc>(loc)) {
        if (f.getLocations().empty()) return false;
        loc = f.getLocations().front();
    }
    auto fl = dyn_cast<FileLineColLoc>(loc);
    if (!fl) return false;
    StringRef file = fl.getFilename().getValue();
    return file.starts_with("/usr/include/") || file.starts_with("/usr/lib/gcc/") ||
           file.starts_with("/usr/local/include/") || file.contains("/include/c++/") || file.contains("/lib/clang/");
}

// The string a format argument names, when it is a literal of the module.
std::optional<std::string> literalString(Value v) {
    v = traceToLocalRoot(v);
    auto gg = v.getDefiningOp<cir::GetGlobalOp>();
    if (!gg) return std::nullopt;
    auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
    if (!g || !g.getConstant() || !g.getInitialValue()) return std::nullopt;
    auto ca = dyn_cast<cir::ConstArrayAttr>(*g.getInitialValue());
    auto str = ca ? dyn_cast<StringAttr>(ca.getElts()) : StringAttr();
    if (!str) return std::nullopt;
    return str.getValue().str();
}

// What each conversion of a printf or scanf format consumes: 's' for a
// string (its memory), 'v' for a value (a number, a pointer printed with
// %p), 'n' for a count written back, in order; nothing when the format is
// not a literal.
std::optional<std::string> conversions(Value fmt) {
    auto f = literalString(fmt);
    if (!f) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < f->size(); ++i) {
        if ((*f)[i] != '%') continue;
        ++i;
        if (i < f->size() && (*f)[i] == '%') continue;
        for (; i < f->size(); ++i) {
            const char ch = (*f)[i];
            if (ch == '*') {
                out += 'v';
                continue;
            }
            if (strchr("-+ #0123456789.'hlLqjztI", ch)) continue;
            if (ch == '[') {  // a scan set, read as a string
                while (i < f->size() && (*f)[i] != ']') ++i;
                out += 's';
            } else
                out += ch == 's' ? 's' : ch == 'n' ? 'n' : 'v';
            break;
        }
    }
    return out;
}

// An alias's aliasee, as a call of it runs (lookupCallee); a weak alias
// stays itself, the link may replace it.
cir::FuncOp throughAliases(ModuleOp module, cir::FuncOp f) {
    for (unsigned hop = 0; f && f.getAliasee() && !cir::isInterposableLinkage(f.getLinkage()) && hop < 8; ++hop)
        f = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(module, f.getAliaseeAttr());
    return f;
}

struct Indirect {
    Operation *call;
    unsigned callee;
    llvm::SmallVector<unsigned, 4> args, results;
    llvm::SmallPtrSet<Operation *, 4> linked;
    bool exposed = false;
};

struct Root {
    unsigned node;
    std::string why;
};

class Analysis {
public:
    Analysis(ModuleOp module, const EscapeQuery &q) : module(module), q(q) {}

    std::optional<std::string> run() {
        for (cir::GlobalOp g : module.getOps<cir::GlobalOp>()) global(g);
        module.walk([&](cir::GlobalOp g) {
            for (Region &r : g->getRegions()) r.walk([&](Operation *op) { visit(op, cir::FuncOp()); });
        });
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            // An alias another file names is its aliasee to that file.
            if (f.getAliasee() && !cir::isLocalLinkage(f.getLinkage()) && q.namesFunction(f)) {
                cir::FuncOp target = throughAliases(module, f);
                if (target && !target.getBody().empty() && !q.own.contains(target)) {
                    const std::string why = "a file linked with this one names " + q.nameOf(f) + ", an alias of " +
                                            q.nameOf(target) + ", which may be handed it or hand it back";
                    for (BlockArgument a : target.getBody().front().getArguments()) roots.push_back({node(a), why});
                    roots.push_back({ret(target), why});
                }
                cls.join(globalNode(f.getSymName()), globalNode(target ? target.getSymName() : f.getSymName()));
            }
            if (f.getBody().empty() || q.own.contains(f) || runtime(f)) continue;
            if (!cir::isLocalLinkage(f.getLinkage()) && q.namesFunction(f)) {
                const std::string why = "a file linked with this one names " + q.nameOf(f) +
                                        ", which may be handed it or hand it back";
                for (BlockArgument a : f.getBody().front().getArguments()) roots.push_back({node(a), why});
                roots.push_back({ret(f), why});
            }
            f.getBody().walk([&](Operation *op) { visit(op, f); });
        }
        if (objects.empty()) return std::nullopt;
        settle("");
        if (auto w = escaped()) return w;
        for (size_t i = 0; i < roots.size(); ++i) {
            const Root r = roots[i];  // settle may add roots
            if (auto w = expose(r.node, r.why)) return w;
            if (auto w = settle(r.why)) return w;
        }
        return std::nullopt;
    }

private:
    ModuleOp module;
    const EscapeQuery &q;
    Classes cls;
    llvm::DenseMap<Value, unsigned> nodes;
    llvm::StringMap<unsigned> globals;
    llvm::DenseMap<Operation *, unsigned> rets, varargs;
    std::vector<Indirect> indirect;
    std::vector<Root> roots;
    llvm::SmallVector<unsigned, 8> objects;  // what calls of the own functions are handed as the object
    unsigned elements = kNone;               // what goes into and out of them otherwise
    unsigned threads = kNone;                // what threads return, to pthread_join
    llvm::DenseSet<Operation *> exposedFns;
    // The casts to a pointer type outside the own functions, for the reads
    // of the object as another type.
    std::vector<std::pair<cir::CastOp, cir::FuncOp>> casts;
    std::vector<std::pair<cir::GetMemberOp, cir::FuncOp>> unionSteps;
    std::vector<std::tuple<Value, Value, cir::FuncOp, Operation *>> copies;  // memcpy's destination and source

public:
    // A cast that makes a value that may point to the object a pointer to
    // another type than the object's, a record that holds it, void or a
    // byte: the program reads or writes the object's storage as that type
    // (a pointer to its first member, offsetof arithmetic from another
    // member), which the rewrite changes.
    std::optional<std::string> reread() {
        auto isObject = [&](unsigned n) {
            return llvm::any_of(objects, [&](unsigned o) { return !cls.external(o) && cls.find(o) == cls.find(n); });
        };
        auto isByte = [](Type t) {
            auto i = dyn_cast_or_null<cir::IntType>(t);
            return i && i.getWidth() == 8;
        };
        // The object's own type, or what holds it, or void.
        auto allowed = [&](Type t) { return t && q.allowedPointee && q.allowedPointee(t); };
        auto typedAsObject = [&](Type t) { return t && !isa<cir::VoidType>(t) && allowed(t); };
        // The program's own code, and libstdc++'s pointer casts, which
        // cast what the program hands them.
        auto program = [&](Operation *op, cir::FuncOp f) {
            if (!systemCode(op->getLoc())) return true;
            if (!f) return false;
            Name n = nameOf(f.getSymName());
            return StringRef(n.base).ends_with("pointer_cast");
        };
        auto typeName = [](Type to) {
            std::string t;
            llvm::raw_string_ostream os(t);
            if (auto r = dyn_cast<cir::RecordType>(to); r && r.getName()) os << r.getName().getValue();
            else if (auto i = dyn_cast<cir::IntType>(to))
                os << (i.isSigned() ? "a signed " : "an unsigned ") << i.getWidth() << "-bit integer";
            else if (auto p = dyn_cast<cir::PointerType>(to); p && isa<cir::RecordType>(p.getPointee()) &&
                                                             cast<cir::RecordType>(p.getPointee()).getName())
                os << cast<cir::RecordType>(p.getPointee()).getName().getValue() << " *";
            else os << to;
            return t;
        };
        for (auto &[c, f] : casts) {
            auto fromPtr = dyn_cast<cir::PointerType>(c.getSrc().getType());
            Type from = fromPtr ? fromPtr.getPointee() : Type();
            Type to = cast<cir::PointerType>(c.getResult().getType()).getPointee();
            // Storage an allocator has just returned, typed for what is
            // built in it, is not the object read as another type.
            if (auto call = c.getSrc().getDefiningOp<cir::CallOp>(); call && isAllocator(lookupCallee(call))) continue;
            // The value may point to the object: typed as it, or as what
            // holds it, wherever the cast is; by the classes only in the
            // program's own code, since the system's headers make storage
            // they allocated a typed pointer, and a class that holds a
            // container of elements pointing back to it is one class with
            // them.
            if (typedAsObject(from) || (program(c, f) && isObject(node(c.getSrc())))) {
                if (isByte(to)) {
                    if (!wholeBytes(c.getResult(), 0)) {
                        if (cls.debug) explain(node(c.getSrc()));
                        return where(f) + " reads a value that may point to it as bytes";
                    }
                } else if (!allowed(to)) {
                    if (cls.debug) explain(node(c.getSrc()));
                    return where(f) + " makes a value that may point to it a pointer to " + typeName(to);
                }
            }
            // The value may point to a slot that holds a pointer to the
            // object (__builtin_bit_cast of the slot): what is loaded
            // through the cast is the object, as another type.
            auto to1 = dyn_cast<cir::PointerType>(to);
            auto from1 = from ? dyn_cast<cir::PointerType>(from) : cir::PointerType();
            if (to1 && !allowed(to1.getPointee()) &&
                ((from1 && typedAsObject(from1.getPointee())) ||
                 (program(c, f) && isObject(cls.content(node(c.getSrc()))))))
                return where(f) + " reads a pointer that may point to it as a pointer to " + typeName(to1.getPointee());
        }
        // A union's member read as another pointer type than the one stored.
        for (auto &[gm, f] : unionSteps) {
            auto r = dyn_cast<cir::PointerType>(gm.getResult().getType());
            auto inner = r ? dyn_cast<cir::PointerType>(r.getPointee()) : cir::PointerType();
            if (inner && !allowed(inner.getPointee()) && program(gm, f) && isObject(cls.content(node(gm.getResult()))))
                return where(f) + " reads a pointer that may point to it through a member of a union, as a pointer to " +
                       typeName(inner.getPointee());
        }
        // A pointer's bytes copied into a pointer of another type.
        for (auto &[dst, src, f, op] : copies) {
            auto d = dyn_cast<cir::PointerType>(traceToLocalRoot(dst).getType());
            auto inner = d ? dyn_cast<cir::PointerType>(d.getPointee()) : cir::PointerType();
            if (inner && !allowed(inner.getPointee()) && program(op, f) && isObject(cls.content(node(src))))
                return where(f) + " copies a pointer that may point to it into a pointer to " +
                       typeName(inner.getPointee());
        }
        return std::nullopt;
    }

    // Whether a byte pointer to the object only clears, copies or frees it
    // whole: handed to memset, memcpy, memmove or a deallocation, compared,
    // or made void * for one of those.
    bool wholeBytes(Value v, unsigned depth) {
        if (depth > 4) return false;
        for (OpOperand &u : v.getUses()) {
            Operation *o = u.getOwner();
            if (isa<cir::CmpOp, cir::MemSetOp, cir::MemCpyOp, cir::MemMoveOp, cir::LifetimeStartOp,
                    cir::LifetimeEndOp>(o))
                continue;
            if (auto cv = dyn_cast<cir::CastOp>(o); cv && isa<cir::PointerType>(cv.getResult().getType())) {
                auto p = cast<cir::PointerType>(cv.getResult().getType());
                if (isa<cir::VoidType>(p.getPointee()) && wholeBytes(cv.getResult(), depth + 1)) continue;
                return false;
            }
            if (auto call = dyn_cast<cir::CallOp>(o)) {
                cir::FuncOp g = lookupCallee(call);
                StringRef n = g ? g.getSymName() : StringRef();
                if (n == "memset" || n == "memcpy" || n == "memmove" || n == "free" || n.starts_with("_ZdlPv") ||
                    n.starts_with("_ZdaPv"))
                    continue;
            }
            return false;
        }
        return true;
    }

private:

    unsigned node(Value v) {
        auto [it, fresh] = nodes.try_emplace(v, 0);
        if (fresh) it->second = cls.fresh();
        return it->second;
    }
    unsigned globalNode(StringRef sym) {
        auto [it, fresh] = globals.try_emplace(sym, 0);
        if (fresh) {
            it->second = cls.fresh();
            if (auto f = throughAliases(module, module.lookupSymbol<cir::FuncOp>(sym)))
                cls.addFunction(it->second, f);
        }
        return it->second;
    }
    unsigned ret(cir::FuncOp f) {
        auto [it, fresh] = rets.try_emplace(f, 0);
        if (fresh) it->second = cls.fresh();
        return it->second;
    }
    unsigned vararg(cir::FuncOp f) {
        auto [it, fresh] = varargs.try_emplace(f, 0);
        if (fresh) it->second = cls.fresh();
        return it->second;
    }
    unsigned elementNode() {
        if (elements == kNone) elements = cls.fresh();
        return elements;
    }
    std::string where(cir::FuncOp f) { return f ? q.nameOf(f) : std::string("a global's initialiser"); }

    void global(cir::GlobalOp g) {
        const unsigned at = globalNode(g.getSymName());
        // A global alias is its aliasee.
        if (auto a = g->getAttrOfType<FlatSymbolRefAttr>("aliasee")) cls.join(at, globalNode(a.getValue()));
        // A global placed in a section of its own, or kept by llvm.used,
        // is found by code that walks the section or the list, in any file.
        auto section = g->getAttrOfType<StringAttr>("section");
        if ((section && section.getValue() != "llvm.metadata") || g.getSymName() == "llvm.used" ||
            g.getSymName() == "llvm.compiler.used")
            roots.push_back({at, "the global " + g.getSymName().str() +
                                     (section && section.getValue() != "llvm.metadata"
                                          ? ", in the section " + section.getValue().str() + ", may hold it"
                                          : ", which the program keeps for the linker, may hold it")});
        // The C++ ABI's read-only data, a vtable, a type_info and its name,
        // holds functions and other such data, never the program's
        // objects: a declared one is no root. A declared vtable's
        // functions are not the module's, and a virtual call that may
        // reach only those reaches code the pass does not see.
        const StringRef sym = g.getSymName();
        const bool abiData = sym.starts_with("_ZTV") || sym.starts_with("_ZTI") || sym.starts_with("_ZTS") ||
                             sym.starts_with("_ZTT");
        if (g.isDeclaration() && sym.starts_with("_ZTV")) cls.setOpaque(cls.content(at));
        if (g.isDeclaration() && !abiData)
            roots.push_back({at, "the global " + g.getSymName().str() + ", which is only declared here, may hold it"});
        else if (!cir::isLocalLinkage(g.getLinkage()) && q.namesSymbol(g.getSymName()))
            roots.push_back({at, "the global " + g.getSymName().str() +
                                     ", which a file linked with this one names, may hold it"});
        if (auto init = g.getInitialValue())
            init->walk([&](cir::GlobalViewAttr gv) { cls.join(cls.content(at), globalNode(gv.getSymbol().getValue())); });
    }

    void joinAll(Operation *op) {
        std::optional<unsigned> first;
        auto add = [&](Value v) {
            if (!first) first = node(v);
            else cls.join(*first, node(v));
        };
        for (Value v : op->getOperands()) add(v);
        for (Value v : op->getResults()) add(v);
    }

    void visit(Operation *op, cir::FuncOp f) {
        cls.current = op;
        if (auto k = dyn_cast<cir::ConstantOp>(op)) {
            // An address made a constant (constexpr, an aggregate that
            // holds one) is the global's.
            k.getValue().walk([&](cir::GlobalViewAttr gv) {
                cls.join(node(k.getResult()), globalNode(gv.getSymbol().getValue()));
            });
            if (auto gv = dyn_cast<cir::GlobalViewAttr>(k.getValue()))
                cls.join(node(k.getResult()), globalNode(gv.getSymbol().getValue()));
            return;
        }
        if (isa<cir::AllocaOp, cir::CmpOp, cir::LifetimeStartOp, cir::LifetimeEndOp, cir::ConditionOp>(op))
            return;
        if (isa<cir::MemSetOp>(op)) return;
        // Inline assembly does what it likes with its operands and memory.
        if (isa<cir::InlineAsmOp>(op)) {
            for (Value v : op->getOperands())
                roots.push_back({node(v), where(f) + " hands a value that may point to it to inline assembly"});
            for (Value v : op->getResults())
                roots.push_back({node(v), where(f) + " takes a value that may point to it from inline assembly"});
            return;
        }
        // A va_list holds the variadic arguments of the function that
        // started it, wherever it is handed.
        if (isa<cir::VAStartOp>(op) && op->getNumOperands() > 0) {
            if (f) cls.join(cls.content(node(op->getOperand(0))), vararg(f));
            return;
        }
        if (isa<cir::VACopyOp>(op) && op->getNumOperands() >= 2)
            return cls.join(cls.content(node(op->getOperand(0))), cls.content(node(op->getOperand(1))));
        if (isa<cir::VAEndOp>(op)) return;
        // A bit-field is read from and written into what its address
        // points to.
        if (isa<cir::SetBitfieldOp>(op) && op->getNumOperands() >= 2) {
            cls.join(cls.content(node(op->getOperand(0))), node(op->getOperand(1)));
            for (Value v : op->getResults()) cls.join(node(v), node(op->getOperand(1)));
            return;
        }
        if (isa<cir::GetBitfieldOp>(op) && op->getNumOperands() >= 1) {
            for (Value v : op->getResults()) cls.join(node(v), cls.content(node(op->getOperand(0))));
            return;
        }
        if (auto cast = dyn_cast<cir::CastOp>(op); cast && cast.getKind() == cir::CastKind::ptr_to_bool) return;
        // The casts that make a pointer of another type (an array decaying
        // to its first element keeps the type).
        if (auto cast = dyn_cast<cir::CastOp>(op); cast && isa<cir::PointerType>(cast.getResult().getType()) &&
                                                   (cast.getKind() == cir::CastKind::bitcast ||
                                                    cast.getKind() == cir::CastKind::int_to_ptr))
            casts.push_back({cast, f});
        if (auto gg = dyn_cast<cir::GetGlobalOp>(op)) return cls.join(node(gg.getResult()), globalNode(gg.getName()));
        if (auto vt = dyn_cast<cir::VTableAddrPointOp>(op)) return cls.join(node(vt.getResult()), globalNode(vt.getName()));
        if (auto ld = dyn_cast<cir::LoadOp>(op)) return cls.join(node(ld.getResult()), cls.content(node(ld.getAddr())));
        if (auto st = dyn_cast<cir::StoreOp>(op)) return cls.join(cls.content(node(st.getAddr())), node(st.getValue()));
        if (auto cp = dyn_cast<cir::CopyOp>(op))
            return cls.join(cls.content(node(cp.getDst())), cls.content(node(cp.getSrc())));
        if (auto gm = dyn_cast<cir::GetMemberOp>(op))
            if (auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType()))
                if (auto r = dyn_cast<cir::RecordType>(pt.getPointee()); r && r.isUnion()) unionSteps.push_back({gm, f});
        if (isa<cir::MemCpyOp, cir::MemMoveOp>(op) && op->getNumOperands() >= 2)
            copies.push_back({op->getOperand(0), op->getOperand(1), f, op});
        if (isa<cir::MemCpyOp, cir::MemMoveOp>(op)) {
            std::optional<unsigned> first;
            for (Value v : op->getOperands())
                if (isa<cir::PointerType>(v.getType())) {
                    if (!first) first = cls.content(node(v));
                    else cls.join(*first, cls.content(node(v)));
                }
            return;
        }
        if (isa<cir::AtomicFetchOp, cir::AtomicXchgOp, cir::AtomicCmpXchgOp>(op) && op->getNumOperands() > 0) {
            const unsigned c = cls.content(node(op->getOperand(0)));
            for (Value v : op->getOperands().drop_front()) cls.join(c, node(v));
            for (Value v : op->getResults()) cls.join(c, node(v));
            return;
        }
        // The address of a virtual function's slot is an address into the
        // vtable the vptr points into.
        if (isa<cir::VTableGetVirtualFnAddrOp>(op) && op->getNumOperands() > 0)
            return cls.join(node(op->getResult(0)), node(op->getOperand(0)));
        // An object's vptr is a field of its own, kept apart from the rest
        // of the object: field-insensitive, the vtables' functions and the
        // object's other members would be one class, and every virtual call
        // on such an object could call any function a member points to.
        if (auto vp = dyn_cast<cir::VTableGetVPtrOp>(op))
            return cls.join(node(vp.getResult()), cls.vptrOf(node(vp.getSrc())));
        if (auto r = dyn_cast<cir::ReturnOp>(op)) {
            if (f)
                for (Value v : r.getOperands()) cls.join(ret(f), node(v));
            return;
        }
        if (auto y = dyn_cast<cir::YieldOp>(op)) {
            Operation *parent = op->getParentOp();
            if (parent && parent->getNumResults() == y->getNumOperands())
                for (auto [a, b] : llvm::zip(y->getOperands(), parent->getResults())) cls.join(node(a), node(b));
            return;
        }
        if (auto br = dyn_cast<BranchOpInterface>(op)) {
            for (unsigned i = 0; i < op->getNumSuccessors(); ++i) {
                OperandRange ops = br.getSuccessorOperands(i).getForwardedOperands();
                Block *dest = op->getSuccessor(i);
                for (auto [a, b] : llvm::zip(ops, dest->getArguments())) cls.join(node(a), node(b));
            }
            return;
        }
        if (isa<cir::VAArgOp>(op)) {
            for (Value v : op->getResults()) {
                if (op->getNumOperands() > 0) cls.join(cls.content(node(op->getOperand(0))), node(v));
                if (f) cls.join(vararg(f), node(v));
            }
            return;
        }
        if (isa<cir::ThrowOp>(op)) {
            for (Value v : op->getOperands())
                roots.push_back({node(v), where(f) + " throws a value that may point to it, and the handler may be "
                                                     "another file's"});
            return;
        }
        if (auto call = dyn_cast<cir::CIRCallOpInterface>(op)) return this->call(call, f);
        if (op->getNumRegions() > 0 && op->getNumOperands() == 0 && op->getNumResults() == 0) return;
        joinAll(op);
    }

    cir::FuncOp calleeOf(cir::CIRCallOpInterface call) {
        if (auto c = dyn_cast<cir::CallOp>(call.getOperation())) return c.isIndirect() ? cir::FuncOp() : lookupCallee(c);
        if (call.isIndirect()) return {};
        auto sym = llvm::dyn_cast_if_present<SymbolRefAttr>(call.getCallableForCallee());
        return sym ? module.lookupSymbol<cir::FuncOp>(sym.getLeafReference()) : cir::FuncOp();
    }

    void call(cir::CIRCallOpInterface call, cir::FuncOp f) {
        llvm::SmallVector<unsigned, 4> args, results;
        for (unsigned i = 0; i < call.getNumArgOperands(); ++i) args.push_back(node(call.getArgOperand(i)));
        for (Value r : call->getResults()) results.push_back(node(r));
        if (call.isIndirect()) {
            Indirect ic;
            ic.call = call;
            ic.callee = node(call.getIndirectCall());
            ic.args = args;
            ic.results = results;
            indirect.push_back(std::move(ic));
            return;
        }
        cir::FuncOp g = calleeOf(call);
        if (!g) return unknownCall(args, results, where(f), "a function the pass cannot find");
        link(g, args, results, f, call);
    }

    // A call of g, direct or resolved from an indirect one.
    void link(cir::FuncOp g, llvm::ArrayRef<unsigned> args, llvm::ArrayRef<unsigned> results, cir::FuncOp f,
              Operation *call) {
        g = throughAliases(module, g);
        if (!g) return unknownCall(args, results, where(f), "a function the pass cannot find");
        // A constructor or destructor of the class: handed the object, and
        // walked, since the rewrite replaces only the container's
        // construction or destruction in it.
        if (q.objectFns.contains(g)) {
            std::optional<unsigned> self = q.self(g);
            if (self && *self < args.size()) objects.push_back(args[*self]);
        }
        if (q.own.contains(g)) {
            std::optional<unsigned> self = q.self(g);
            // The elements go in and come out by value, or through what an
            // argument points to when it points to an element (a pop's
            // out-parameter, a push by reference); with the element's type
            // unknown, both.
            auto call0 = dyn_cast_or_null<cir::CIRCallOpInterface>(call);
            for (auto [i, a] : llvm::enumerate(args)) {
                if (self && i == *self) {
                    objects.push_back(a);
                    continue;
                }
                Type t = call0 && i < call0.getNumArgOperands() ? call0.getArgOperand(i).getType() : Type();
                auto pt = dyn_cast_or_null<cir::PointerType>(t);
                const bool byRef = q.elementType && pt && pt.getPointee() == q.elementType;
                if (!q.elementType || !byRef) cls.join(elementNode(), a);
                if (!q.elementType || byRef) cls.join(elementNode(), cls.content(a));
            }
            for (unsigned r : results) cls.join(elementNode(), r);
            return;
        }
        // The shim's runtime is the compiler's own code: what it is handed
        // it may hand back from any of its functions, and nothing else.
        if (runtime(g)) {
            if (runtimeNode == kNone) runtimeNode = cls.fresh();
            for (unsigned a : args) {
                cls.join(runtimeNode, a);
                cls.join(runtimeNode, cls.content(a));
            }
            for (unsigned r : results) cls.join(runtimeNode, r);
            return;
        }
        if (!g.getBody().empty() && leaf(g)) return instantiate(g, args, results);
        if (!g.getBody().empty()) {
            Block &entry = g.getBody().front();
            for (auto [i, a] : llvm::enumerate(args))
                cls.join(a, i < entry.getNumArguments() ? node(entry.getArgument(i)) : vararg(g));
            for (unsigned r : results) cls.join(r, ret(g));
            return;
        }
        library(g, args, results, f, call);
    }

    void unknownCall(llvm::ArrayRef<unsigned> args, llvm::ArrayRef<unsigned> results, const std::string &at,
                     const std::string &target) {
        for (unsigned a : args) roots.push_back({a, at + " hands a value that may point to it to " + target});
        for (unsigned r : results) roots.push_back({r, at + " takes a value that may point to it from " + target});
    }

    void library(cir::FuncOp g, llvm::ArrayRef<unsigned> args, llvm::ArrayRef<unsigned> results, cir::FuncOp f,
                 Operation *call) {
        const std::string name = pretty(g.getSymName());
        // A linked file's function of a library's name is the program's.
        const LibModel m = q.definedElsewhere(g.getSymName()) ? LibModel() : libraryModel(g);
        auto c = cast<cir::CIRCallOpInterface>(call);
        const std::string at = where(f);
        // What a format formats reaches `into`: a value itself, a string's
        // memory; a format that is not a literal formats any of them.
        auto format = [&](unsigned fmtArg, unsigned into, bool scan) {
            std::optional<std::string> conv =
                fmtArg < c.getNumArgOperands() ? conversions(c.getArgOperand(fmtArg)) : std::nullopt;
            for (unsigned i = fmtArg + 1, k = 0; i < args.size(); ++i, ++k) {
                const char what = conv && k < conv->size() ? (*conv)[k] : 'x';
                if (scan) {
                    // Parsed text lands in what the argument points to.
                    if (what != 'n') cls.join(cls.content(args[i]), into);
                    continue;
                }
                if (what == 'n') continue;
                if (what == 's' || what == 'x') cls.join(cls.content(args[i]), into);
                if (what == 'v' || what == 'x') cls.join(args[i], into);
            }
        };
        switch (m.kind) {
        case Lib::Fresh:
        case Lib::Inert:
            return;
        case Lib::FirstArg:
            for (unsigned r : results)
                if (!args.empty()) cls.join(r, args[0]);
            return;
        case Lib::Copy:
            if (g.getSymName() == "strdup" || g.getSymName() == "strndup") {
                for (unsigned r : results)
                    if (!args.empty()) cls.join(cls.content(r), cls.content(args[0]));
                return;
            }
            if (args.size() >= 2) cls.join(cls.content(args[0]), cls.content(args[1]));
            for (unsigned r : results)
                if (!args.empty()) cls.join(r, args[0]);
            if (c.getNumArgOperands() >= 2) copies.push_back({c.getArgOperand(0), c.getArgOperand(1), f, call});
            return;
        case Lib::EndPtr:
            if (args.size() >= 2) cls.join(cls.content(args[1]), args[0]);
            for (unsigned r : results)
                if (!args.empty()) cls.join(r, cls.content(args[0]));
            return;
        case Lib::FromString:
            for (unsigned r : results)
                if (!args.empty()) cls.join(r, cls.content(args[0]));
            return;
        case Lib::PutValue:
            if (m.arg < args.size()) toFiles(args[m.arg], at + " writes a value that may point to it with " + name);
            return;
        case Lib::Insert:
            if (args.size() >= 2) {
                toFiles(args[1], at + " writes a value that may point to it with " + name);
                toFiles(cls.content(args[1]), at + " writes what may point to it with " + name);
            }
            return;
        case Lib::Structure: {
            llvm::SmallVector<unsigned, 8> all(args.begin(), args.end());
            all.append(results.begin(), results.end());
            for (unsigned a : all) {
                cls.join(all.front(), a);
                cls.join(all.front(), cls.content(a));
            }
            return;
        }
        case Lib::Print:
            if (g.getSymName() == "vprintf") {
                if (args.size() >= 2) toFiles(cls.content(args[1]), at + " prints what may point to it with " + name);
                return;
            }
            formatRoots(c, m.arg, at, name, /*scan=*/false);
            return;
        case Lib::PrintTo:
            if (!args.empty()) format(m.arg, cls.content(args[0]), false);
            return;
        case Lib::Scan:
            for (unsigned i = m.arg + 1; i < args.size(); ++i)
                fromFiles(cls.content(args[i]), at + " reads what may point to it with " + name);
            return;
        case Lib::ScanFrom:
            if (!args.empty()) format(m.arg, cls.content(args[0]), true);
            return;
        case Lib::WriteOut:
            if (m.arg < args.size()) toFiles(cls.content(args[m.arg]), at + " writes memory that may hold it with " + name);
            return;
        case Lib::ReadIn:
            if (m.arg < args.size()) fromFiles(cls.content(args[m.arg]), at + " reads what may point to it with " + name);
            for (unsigned r : results)
                if (!args.empty() && g.getSymName() == "fgets") cls.join(r, args[0]);
            return;
        case Lib::ThreadJoin:
            if (args.size() >= 2) cls.join(cls.content(args[1]), threadResults());
            return;
        case Lib::ThreadExit:
            if (!args.empty()) cls.join(args[0], threadResults());
            return;
        case Lib::Callback: {
            if (m.fn >= c.getNumArgOperands()) break;
            cir::FuncOp cb = resolveFunctionOperand(c.getArgOperand(m.fn));
            if (!cb) break;
            cb = throughAliases(module, cb);
            if (!cb || (cb.getBody().empty() && !q.own.contains(cb))) break;
            // The callback runs with that argument as the parameter the
            // model names; a thread's result goes to pthread_join.
            llvm::SmallVector<unsigned, 2> cbArgs;
            for (unsigned i = 0; i < m.param; ++i) cbArgs.push_back(cls.fresh());
            if (m.hasArg && m.arg < args.size()) cbArgs.push_back(args[m.arg]);
            llvm::SmallVector<unsigned, 1> res;
            if (g.getSymName() == "pthread_create") res.push_back(threadResults());
            link(cb, cbArgs, res, f, call);
            return;
        }
        case Lib::ThreadStart: {
            // std::thread's start runs the state it is handed, through its
            // virtual _M_run: every std::thread::_State_impl's of the module.
            if (args.size() < 2) break;
            bool any = false;
            for (cir::FuncOp run : module.getOps<cir::FuncOp>()) {
                if (run.getBody().empty() || run.getNumArguments() == 0) continue;
                Name n = nameOf(run.getSymName());
                if (n.base != "_M_run" || !StringRef(n.context).starts_with("std::thread::_State_impl<")) continue;
                cls.join(cls.content(args[1]), node(run.getBody().front().getArgument(0)));
                any = true;
            }
            if (any) return;
            break;
        }
        case Lib::Unknown:
            break;
        }
        unknownCall(args, results, at, name + ", which has no body here");
    }

    // A printf's variadic arguments, formatted into a file: a value printed
    // (%p, a number) and a string's memory (%s) reach the outside, which
    // may read them back.
    void formatRoots(cir::CIRCallOpInterface c, unsigned fmtArg, const std::string &at, const std::string &name,
                     bool) {
        std::optional<std::string> conv =
            fmtArg < c.getNumArgOperands() ? conversions(c.getArgOperand(fmtArg)) : std::nullopt;
        const std::string why = at + " prints a value that may point to it with " + name;
        for (unsigned i = fmtArg + 1, k = 0; i < c.getNumArgOperands(); ++i, ++k) {
            const char what = conv && k < conv->size() ? (*conv)[k] : 'x';
            const unsigned a = node(c.getArgOperand(i));
            if (what == 'n') continue;
            if (what == 's' || what == 'x') toFiles(cls.content(a), why);
            if (what == 'v' || what == 'x') toFiles(a, why);
        }
    }

    // A function of the lifted runtime, by where it is defined: the
    // shim's lifted.hpp, queue.hpp, bounded.hpp and c_queue.h, and
    // runtime/*.hpp, in the directories wtc's own headers are (a shim
    // directory holds lifted.hpp, its sibling runtime/ the rings). Never
    // by its name, which a program may use, and never one a linked file
    // defines.
    llvm::StringMap<bool> runtimeFiles;
    bool runtime(cir::FuncOp g) {
        if (q.definedElsewhere && q.definedElsewhere(g.getSymName())) return false;
        Location loc = g.getLoc();
        while (auto f = dyn_cast<FusedLoc>(loc)) {
            if (f.getLocations().empty()) return false;
            loc = f.getLocations().front();
        }
        auto fl = dyn_cast<FileLineColLoc>(loc);
        if (!fl) return false;
        StringRef file = fl.getFilename().getValue();
        auto [it, fresh] = runtimeFiles.try_emplace(file, false);
        if (!fresh) return it->second;
        llvm::SmallString<256> real;
        if (llvm::sys::fs::real_path(file, real)) return false;
        StringRef dir = llvm::sys::path::parent_path(real), base = llvm::sys::path::filename(real);
        auto exists = [](const llvm::Twine &p) { return llvm::sys::fs::exists(p); };
        bool yes = false;
        if (llvm::sys::path::filename(dir) == "shim" && exists(dir + "/lifted.hpp"))
            yes = base == "lifted.hpp" || base == "queue.hpp" || base == "bounded.hpp" || base == "c_queue.h";
        else if (llvm::sys::path::filename(dir) == "runtime" &&
                 exists(llvm::sys::path::parent_path(dir) + "/shim/lifted.hpp"))
            yes = base.ends_with(".hpp");
        runtimeFiles[file] = yes;
        return yes;
    }
    unsigned runtimeNode = kNone;

    // What the program writes to files and may read back. With a file
    // linked, that file's code may read them too: they are the outside's.
    // With none, they come back only to this module, through its reads.
    // What the program writes to files reaches the outside: a library
    // function off the list may read the files back, and another process
    // or file's code may read them.
    void toFiles(unsigned n, const std::string &why) { roots.push_back({n, why}); }
    // What the program reads from files lands in n.
    void fromFiles(unsigned n, const std::string &why) { roots.push_back({n, why}); }

    // A small function of the system's headers that calls nothing (an
    // accessor such as std::_Any_data::_M_access, an iterator's step): its
    // effect is summarised once, by the classes of its own body, and the
    // summary copied at each call, so that its callers' objects are not one
    // class through its parameters. Its body is walked as well, for the
    // calls the analysis cannot see (through its address).
    llvm::DenseMap<Operation *, bool> leaves;
    llvm::DenseMap<Operation *, std::unique_ptr<Analysis>> summaries;
    bool leaf(cir::FuncOp g) {
        auto [it, fresh] = leaves.try_emplace(g, false);
        if (!fresh) return it->second;
        if (g.getBody().empty() || q.own.contains(g) || q.objectFns.contains(g) || runtime(g) ||
            !systemCode(g.getLoc()))
            return false;
        unsigned n = 0;
        bool ok = true;
        g.walk([&](Operation *op) {
            ++n;
            if (isa<cir::CIRCallOpInterface, cir::InlineAsmOp, cir::ThrowOp, cir::VAStartOp, cir::VAArgOp,
                    cir::VACopyOp>(op))
                ok = false;
        });
        return leaves[g] = ok && n <= 400;
    }
    void instantiate(cir::FuncOp g, llvm::ArrayRef<unsigned> args, llvm::ArrayRef<unsigned> results) {
        std::unique_ptr<Analysis> &sum = summaries[g];
        if (!sum) {
            sum = std::make_unique<Analysis>(module, q);
            g.getBody().walk([&](Operation *op) { sum->visit(op, g); });
        }
        Analysis &sub = *sum;
        llvm::DenseMap<unsigned, unsigned> inst;
        llvm::SmallVector<unsigned, 16> work;
        auto get = [&](unsigned n) -> unsigned {
            const unsigned r = sub.cls.find(n);
            if (r == sub.cls.find(Classes::outside)) return Classes::outside;
            auto [it, fresh] = inst.try_emplace(r, 0);
            if (fresh) {
                it->second = cls.fresh();
                for (Operation *f : sub.cls.functions(r)) cls.addFunction(it->second, f);
                if (sub.cls.opaque(r)) cls.setOpaque(it->second);
                work.push_back(r);
            }
            return it->second;
        };
        Block &entry = g.getBody().front();
        for (auto [i, a] : llvm::enumerate(args))
            cls.join(a, get(i < entry.getNumArguments() ? sub.node(entry.getArgument(i)) : sub.vararg(g)));
        for (unsigned r : results) cls.join(r, get(sub.ret(g)));
        for (auto &[sym, n] : sub.globals) cls.join(get(n), globalNode(sym));
        while (!work.empty()) {
            const unsigned r = work.pop_back_val();
            const unsigned mine = inst.lookup(r);
            if (unsigned p = sub.cls.pointee(r); p != kNone) cls.join(cls.content(mine), get(p));
            if (unsigned p = sub.cls.vpointee(r); p != kNone) cls.join(cls.vptrOf(mine), get(p));
        }
    }

    unsigned threadResults() {
        if (threads == kNone) threads = cls.fresh();
        return threads;
    }

    // The classes reaching the outside: the indirect calls whose callee
    // may be one of the module's functions, the functions whose address
    // reaches it and the indirect calls whose callee may be the outside's,
    // until nothing changes.
    std::optional<std::string> settle(const std::string &cause) {
        unsigned seenSize = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < indirect.size(); ++i) {
                for (Operation *fn : cls.functions(indirect[i].callee)) {
                    if (!indirect[i].linked.insert(fn).second) continue;
                    changed = true;
                    auto g = cast<cir::FuncOp>(fn);
                    auto caller = indirect[i].call->getParentOfType<cir::FuncOp>();
                    llvm::SmallVector<unsigned, 4> args = indirect[i].args, results = indirect[i].results;
                    link(g, args, results, caller, indirect[i].call);
                }
            }
            if (cls.outsideSize() == seenSize && !changed) break;
            seenSize = cls.outsideSize();
            for (auto &[sym, at] : globals) {
                auto g = module.lookupSymbol<cir::FuncOp>(sym);
                if (!g || g.getBody().empty() || q.own.contains(g) || runtime(g) || exposedFns.contains(g) ||
                    !cls.external(at))
                    continue;
                exposedFns.insert(g);
                changed = true;
                const std::string why = q.nameOf(g) + ", whose address may reach code the pass does not see" +
                                        (cause.empty() ? std::string() : " (" + cause + ")") +
                                        ", may be handed it or hand it back";
                for (BlockArgument a : g.getBody().front().getArguments())
                    if (auto w = expose(node(a), why)) return w;
                if (auto w = expose(ret(g), why)) return w;
            }
            for (Indirect &ic : indirect) {
                const bool unknownOnly = cls.opaque(ic.callee) && cls.functions(ic.callee).empty();
                if (ic.exposed || (!cls.external(ic.callee) && !unknownOnly)) continue;
                ic.exposed = true;
                changed = true;
                const std::string at = where(ic.call->getParentOfType<cir::FuncOp>());
                const std::string target = "an indirect call that may reach code the pass does not see" +
                                           (cause.empty() ? std::string() : " (" + cause + ")");
                for (unsigned a : ic.args)
                    if (auto w = expose(a, at + " hands a value that may point to it to " + target)) return w;
                for (unsigned r : ic.results)
                    if (auto w = expose(r, at + " takes a value that may point to it from " + target)) return w;
            }
            // Roots found while linking are joined by the caller's loop.
        }
        if (escaped()) return cause;
        return std::nullopt;
    }

    std::optional<std::string> expose(unsigned n, const std::string &why) {
        cls.join(n, Classes::outside);
        if (escaped()) {
            if (cls.debug) {
                llvm::errs() << "escape: " << why << "\n";
                explain(n);
            }
            return why;
        }
        return std::nullopt;
    }

    // The joins from n to an object, for WTC_ESCAPE_DEBUG.
    void explain(unsigned n) {
        llvm::DenseMap<unsigned, llvm::SmallVector<std::pair<unsigned, Operation *>, 4>> adj;
        for (auto &[a, b, op] : cls.edges) {
            adj[a].push_back({b, op});
            adj[b].push_back({a, op});
        }
        llvm::DenseMap<unsigned, std::pair<unsigned, Operation *>> from;
        llvm::SmallVector<unsigned, 64> work{n};
        from[n] = {n, nullptr};
        llvm::DenseSet<unsigned> goals(objects.begin(), objects.end());
        while (!work.empty()) {
            unsigned x = work.front();
            work.erase(work.begin());
            if (goals.contains(x)) {
                for (unsigned y = x; y != n; y = from[y].first) {
                    llvm::errs() << "  join " << y << " via ";
                    if (Operation *op = from[y].second) {
                        op->getLoc().print(llvm::errs());
                        llvm::errs() << " " << op->getName();
                        if (auto c = dyn_cast<cir::CallOp>(op))
                            if (auto g = lookupCallee(c)) llvm::errs() << " " << g.getSymName();
                    } else
                        llvm::errs() << "?";
                    llvm::errs() << "\n";
                }
                return;
            }
            for (auto &[y, op] : adj[x])
                if (from.try_emplace(y, std::make_pair(x, op)).second) work.push_back(y);
        }
    }

    std::optional<std::string> escaped() {
        for (unsigned o : objects)
            if (cls.external(o)) return std::string("");
        return std::nullopt;
    }
};

}  // namespace

EscapeResult mlir::wtc::escapes(ModuleOp module, const EscapeQuery &q) {
    Analysis a(module, q);
    EscapeResult r;
    r.escape = a.run();
    if (r.escape && r.escape->empty()) r.escape = std::string("the module's own code hands it out");
    r.reread = a.reread();
    return r;
}
