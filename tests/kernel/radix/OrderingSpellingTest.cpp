//
// radix-tree — §11's static spelling check.
//
//   | State-word ordering | Not testable on x86 and invisible to TSan. Carried
//   | by a static check that each primitive is spelled with its named ordering
//   | constant. The ARMv8 stress can confirm a failure, never absence. |
//
// This file IS that check. It is a source scanner, not a behavioural test,
// because there is nothing to observe at runtime: x86-64's `lock or` is a full
// barrier so no execution can expose a missing acquire, and ARMv8 TSan does not
// model the ABSENCE of an acquire on a well-formed atomic. The named constants
// are the implementation; this is the verification.
//
// The rule it enforces: **every atomic operation in the radix headers passes an
// ordering argument, and that argument is a named constant from Ordering.h (or
// the framework's kProtectedLinkLoad).** Two failures it is specifically shaped
// to catch:
//
//   - a BARE ordering (`fetch_or(mask, ACQUIRE)`). Correct today, but it defeats
//     the whole point: the constant is where the ARGUMENT for the ordering
//     lives, and a bare enum carries none. The next person to touch it has no
//     way to know whether ACQUIRE was reasoned about or typed.
//   - a DEFAULTED ordering (`.load()`), which is SEQ_CST — correct, slow, and
//     invisible. §6.6 notes the check must cover state-word LOADS as well as
//     RMWs precisely because a defaulted load is otherwise not in the primitive
//     list at all.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace CroCOSTest;

namespace {

// Set by CMake to kernel/include/mem/radix. A source scanner needs the source.
#ifndef CROCOS_RADIX_HEADER_DIR
#error "CROCOS_RADIX_HEADER_DIR must be defined for the ordering spelling check"
#endif

const char* const kHeaders[] = {
    "Geometry.h", "Ordering.h", "SlotCodec.h", "Node.h",
    "Mapping.h", "DeferredRelease.h", "BucketCodec.h", "Dispatch.h", "CoreTree.h",
};

std::string readHeader(const char* name) {
    std::string path = std::string(CROCOS_RADIX_HEADER_DIR) + "/" + name;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw AssertionFailure("cannot open " + path);
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Comments are where the ordering names are DISCUSSED, so scanning them would
// produce nothing but false positives. Strip both forms; string literals too,
// since assert messages quote code.
std::string stripCommentsAndStrings(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    enum class St { Code, Line, Block, Str, Chr } st = St::Code;
    for (size_t i = 0; i < src.size(); i++) {
        const char c = src[i];
        const char d = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (st) {
        case St::Code:
            if (c == '/' && d == '/') { st = St::Line;  i++; out += ' '; }
            else if (c == '/' && d == '*') { st = St::Block; i++; out += ' '; }
            else if (c == '"')  { st = St::Str; out += ' '; }
            else if (c == '\'') { st = St::Chr; out += ' '; }
            else out += c;
            break;
        case St::Line:
            if (c == '\n') { st = St::Code; out += '\n'; }
            break;
        case St::Block:
            if (c == '*' && d == '/') { st = St::Code; i++; }
            break;
        case St::Str:
            if (c == '\\') i++;
            else if (c == '"') st = St::Code;
            break;
        case St::Chr:
            if (c == '\\') i++;
            else if (c == '\'') st = St::Code;
            break;
        }
    }
    return out;
}

// Split an argument list at top-level commas.
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0, angle = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == '<') angle++;
        if (c == '>') angle--;
        if (c == ',' && depth == 0 && angle <= 0) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

// The bare enumerators a named constant must be used INSTEAD of.
bool isBareOrdering(const std::string& a) {
    return a == "RELAXED" || a == "ACQUIRE" || a == "RELEASE" ||
           a == "ACQ_REL" || a == "SEQ_CST" || a == "CONSUME";
}

bool isNamedOrdering(const std::string& a) {
    // Ordering.h's constants, and the framework's own. Deliberately a prefix
    // rule rather than an enumeration: a new named constant added to Ordering.h
    // should pass without editing this file, while a bare enumerator never can.
    if (a.rfind("k", 0) != 0) return false;
    if (a.rfind("kernel::", 0) == 0) return false;
    return true;
}

struct Finding {
    std::string header;
    std::string call;
    std::string reason;
    size_t line;
};

// Every atomic operation whose last argument must be an ordering.
const char* const kPrimitives[] = {
    ".load(", ".store(", ".fetch_or(", ".fetch_and(",
    ".fetch_add(", ".fetch_sub(", ".fetch_xor(", ".exchange(",
    ".compare_exchange(", ".compare_exchange_weak(", ".compare_exchange_v(",
};

// How many arguments each primitive takes BEFORE its ordering argument(s).
size_t valueArgCount(const std::string& prim) {
    if (prim == ".load(") return 0;
    if (prim.rfind(".compare_exchange", 0) == 0) return 2;
    return 1;
}

void scanHeader(const char* name, std::vector<Finding>& out, size_t& primitivesSeen) {
    const std::string raw = readHeader(name);
    const std::string src = stripCommentsAndStrings(raw);

    for (const char* p : kPrimitives) {
        const std::string prim = p;
        size_t pos = 0;
        while ((pos = src.find(prim, pos)) != std::string::npos) {
            const size_t open = pos + prim.size() - 1;   // the '('
            // Find the matching ')'.
            int depth = 0;
            size_t i = open;
            for (; i < src.size(); i++) {
                if (src[i] == '(') depth++;
                else if (src[i] == ')') { depth--; if (depth == 0) break; }
            }
            if (i >= src.size()) { pos = open + 1; continue; }

            const std::string argsText = src.substr(open + 1, i - open - 1);
            const size_t line = 1 + static_cast<size_t>(
                std::count(src.begin(), src.begin() + static_cast<long>(pos), '\n'));

            auto args = splitArgs(argsText);
            for (auto& a : args) a = trim(a);
            // Drop a trailing empty from a trailing comma.
            while (!args.empty() && args.back().empty()) args.pop_back();

            const size_t values = valueArgCount(prim);
            if (args.size() <= values) {
                out.push_back(Finding{name, prim,
                    "ordering argument is DEFAULTED (SEQ_CST) — correct, slow, and invisible "
                    "to this check unless spelled", line});
            } else {
                for (size_t k = values; k < args.size(); k++) {
                    if (isBareOrdering(args[k])) {
                        out.push_back(Finding{name, prim,
                            "ordering is a BARE enumerator '" + args[k] +
                            "' — the named constant is where the argument for the ordering "
                            "lives, and a bare enum carries none", line});
                    } else if (!isNamedOrdering(args[k])) {
                        out.push_back(Finding{name, prim,
                            "ordering argument '" + args[k] +
                            "' is not a named constant from Ordering.h", line});
                    }
                }
            }
            primitivesSeen++;
            pos = i;
        }
    }
}

}  // namespace

TEST(radix_every_atomic_is_spelled_with_a_named_ordering) {
    std::vector<Finding> findings;
    size_t primitivesSeen = 0;
    for (const char* h : kHeaders) scanHeader(h, findings, primitivesSeen);

    // A scanner that matched NOTHING would pass this test vacuously and report
    // the codebase as clean — which is exactly the failure mode a static check
    // is prone to, since nothing about a green result distinguishes "no
    // violations" from "no scanning". So the scan asserts its own coverage: a
    // floor well below the real count, high enough that a broken matcher, a
    // renamed header or a moved directory fails loudly instead of quietly.
    if (primitivesSeen < 20) {
        throw AssertionFailure(
            "§11 ordering-spelling check scanned only " + std::to_string(primitivesSeen) +
            " atomic primitives across " + std::to_string(sizeof(kHeaders) / sizeof(*kHeaders)) +
            " headers — the scanner is not seeing the source, so its green result means "
            "nothing. Check CROCOS_RADIX_HEADER_DIR and kHeaders.");
    }

    if (!findings.empty()) {
        std::string msg = "§11 ordering-spelling check failed:\n";
        for (const auto& f : findings) {
            msg += "  " + f.header + ":" + std::to_string(f.line) + "  " + f.call +
                   "  " + f.reason + "\n";
        }
        msg += "\nThis is the ONLY detector for the claim->slot-read edge: x86-64's "
               "`lock or` is a full barrier so no execution can expose a missing acquire, "
               "and TSan does not model a missing acquire on a well-formed atomic.";
        throw AssertionFailure(msg);
    }
}

// The scanner has to be able to fail, or a green result means nothing. This
// drives it over deliberately-wrong source text — the same shape as Phase 0's
// gate tests, and for the same reason.
TEST(radix_ordering_spelling_check_actually_detects_violations) {
    std::vector<Finding> findings;

    // Bare enumerator.
    {
        const std::string src = "void f() { w.fetch_or(m, ACQUIRE); }";
        const std::string stripped = stripCommentsAndStrings(src);
        ASSERT_TRUE(stripped.find("fetch_or") != std::string::npos);
        auto args = splitArgs("m, ACQUIRE");
        ASSERT_EQ(size_t{2}, args.size());
        ASSERT_TRUE(isBareOrdering(trim(args[1])));
        ASSERT_FALSE(isNamedOrdering(trim(args[1])));
    }

    // Named constant.
    {
        auto args = splitArgs("m, kClaimAcquire");
        ASSERT_FALSE(isBareOrdering(trim(args[1])));
        ASSERT_TRUE(isNamedOrdering(trim(args[1])));
    }

    // Defaulted load: one argument short.
    {
        auto args = splitArgs("");
        while (!args.empty() && trim(args.back()).empty()) args.pop_back();
        ASSERT_EQ(size_t{0}, args.size());
        ASSERT_TRUE(args.size() <= valueArgCount(".load("));
    }

    // Comments and string literals must not be scanned — they are where the
    // orderings are DISCUSSED, and scanning them yields nothing but noise.
    {
        const std::string src =
            "// w.fetch_or(m, ACQUIRE) would be wrong\n"
            "assert(false, \"use .store(x, RELEASE)\");\n";
        const std::string stripped = stripCommentsAndStrings(src);
        ASSERT_TRUE(stripped.find("fetch_or") == std::string::npos);
        ASSERT_TRUE(stripped.find("RELEASE") == std::string::npos);
    }

    ASSERT_TRUE(findings.empty());
}
