/**
 * @file jtty_source_test.cpp
 * @brief Gate for the JTTY source grammar port against reference vectors.
 *
 * vectors/jtty_source_vectors.txt is written by the reference Fortran
 * compiled standalone (see vectors/README.md). Every record is exact:
 *   ATOMIN / ATOMOUT  packJttyAtom -> frame -> unpackJttyAtom -> render,
 *                     for every atom kind, valid and invalid
 *   WORD              unpackJttyAtom + render on raw 34-bit words: the
 *                     specification's golden vectors, mutations, noise
 *   CALL / N28        pack28, unpack28, standardCall
 *   MSGIN / MSGNORM   normalizeJttyMessage with 80-character field semantics
 *   ASM / ASMOUT      unpackJtty message assembly, including frames the
 *                     grammar must reject mid-message
 *
 * Text fields are delimited by '@' (not in the JTTY alphabet).
 * Build: tests_jtty/build.sh
 */

#include "JS8_Mode/jtty_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace JTTY;

namespace {

int g_failures = 0;
int g_checks = 0;

void fail(const std::string &what) {
    ++g_failures;
    if (g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) fail(what);
}

// A line split into tokens outside '@' pairs and the fields inside them, in order.
struct Record {
    std::vector<std::string> tokens;   // outside tokens
    std::vector<std::string> fields;   // inside '@...@'
};

Record parse(const std::string &line) {
    Record r;
    std::string outside;
    bool inside = false;
    std::string field;
    for (char c : line) {
        if (c == '@') {
            if (inside) {
                r.fields.push_back(field);
                field.clear();
            } else {
                std::istringstream ss(outside);
                std::string t;
                while (ss >> t) r.tokens.push_back(t);
                outside.clear();
            }
            inside = !inside;
            continue;
        }
        if (inside)
            field += c;
        else
            outside += c;
    }
    std::istringstream ss(outside);
    std::string t;
    while (ss >> t) r.tokens.push_back(t);
    return r;
}

std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Fortran's blank-padded 13-character text -> the C++ convention.
std::string atomText(int kind, const std::string &field13) {
    if (kind == ATOM_TEXT5) {
        std::string t = field13.substr(0, 5);
        while (t.size() < 5) t += ' ';
        return t;
    }
    return rtrim(field13);
}

std::string frameToString(const SourceFrame &f) {
    std::string s;
    for (int i = 0; i < PAYLOAD_BITS; ++i) s += f.bits[i] ? '1' : '0';
    return s;
}

SourceFrame frameFromString(const std::string &s) {
    SourceFrame f{};
    for (int i = 0; i < PAYLOAD_BITS && i < static_cast<int>(s.size()); ++i)
        f.bits[i] = s[i] == '1' ? 1 : 0;
    return f;
}

bool flag(const std::string &t) { return t == "T"; }

std::string describe(const SourceAtom &a) {
    return "kind " + std::to_string(a.kind) + " subtype " + std::to_string(a.subtype) + " role " +
           std::to_string(a.role) + " value " + std::to_string(a.value) + " value2 " +
           std::to_string(a.value2) + " text '" + a.text + "'";
}

void compareAtom(const std::string &where, const SourceAtom &got, const SourceAtom &want) {
    check(got.kind == want.kind && got.subtype == want.subtype && got.role == want.role &&
              got.value == want.value && got.value2 == want.value2 && got.text == want.text,
          where + ": got " + describe(got) + " want " + describe(want));
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "vectors/jtty_source_vectors.txt";
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::string line;
    std::getline(in, line);
    check(line == "JTTY_SOURCE_VECTORS 1", "header: " + line);

    int atoms = 0, words = 0, calls = 0, n28s = 0, msgs = 0, asms = 0, packs = 0;
    bool expectPack = false;
    static const int SOURCE_SYNC_CHECK[13] = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};
    SourceAtom pendingAtom;
    bool pendingEom = false;
    std::string pendingLabel;
    std::string pendingRaw;
    int lineNo = 1;

    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;
        Record r = parse(line);
        if (r.tokens.empty()) continue;
        const std::string &tag = r.tokens[0];
        std::string where = tag + " line " + std::to_string(lineNo);

        if (tag == "ATOMIN") {
            pendingAtom = SourceAtom{};
            pendingAtom.kind = std::stoi(r.tokens[1]);
            pendingAtom.subtype = std::stoi(r.tokens[2]);
            pendingAtom.role = std::stoi(r.tokens[3]);
            pendingAtom.value = std::stoi(r.tokens[4]);
            pendingAtom.value2 = std::stoi(r.tokens[5]);
            pendingAtom.text = atomText(pendingAtom.kind, r.fields.at(0));
            pendingEom = flag(r.tokens[6]);
            pendingLabel = describe(pendingAtom);
            continue;
        }
        if (tag == "ATOMOUT") {
            ++atoms;
            where += " [" + pendingLabel + "]";
            SourceFrame frame{};
            bool packed = packJttyAtom(pendingAtom, frame, pendingEom);
            bool wantPacked = flag(r.tokens[1]);
            check(packed == wantPacked, where + " pack valid got " + (packed ? "T" : "F"));
            if (!wantPacked || !packed) continue;
            check(frameToString(frame) == r.tokens[2],
                  where + " frame got " + frameToString(frame) + " want " + r.tokens[2]);
            SourceAtom out;
            bool eom = false;
            bool uv = unpackJttyAtom(frame, out, eom);
            check(uv == flag(r.tokens[3]), where + " unpack valid");
            if (!uv) continue;
            check(eom == flag(r.tokens[4]), where + " eom");
            SourceAtom want;
            want.kind = std::stoi(r.tokens[5]);
            want.subtype = std::stoi(r.tokens[6]);
            want.role = std::stoi(r.tokens[7]);
            want.value = std::stoi(r.tokens[8]);
            want.value2 = std::stoi(r.tokens[9]);
            want.text = atomText(want.kind, r.fields.at(0));
            compareAtom(where + " unpacked atom", out, want);
            std::string rendered;
            bool rv = renderJttyAtom(out, rendered);
            check(rv == flag(r.tokens[10]), where + " render valid");
            if (rv) check(rtrim(rendered) == r.fields.at(1),
                          where + " rendered got '" + rendered + "' want '" + r.fields.at(1) + "'");
            continue;
        }
        if (tag == "WORD") {
            ++words;
            SourceFrame frame = frameFromString(r.tokens[1]);
            SourceAtom out;
            bool eom = false;
            bool uv = unpackJttyAtom(frame, out, eom);
            bool wantValid = flag(r.tokens[2]);
            check(uv == wantValid, where + " " + r.tokens[1] + " valid got " + (uv ? "T" : "F"));
            if (!wantValid || !uv) continue;
            check(eom == flag(r.tokens[3]), where + " eom");
            SourceAtom want;
            want.kind = std::stoi(r.tokens[4]);
            want.subtype = std::stoi(r.tokens[5]);
            want.role = std::stoi(r.tokens[6]);
            want.value = std::stoi(r.tokens[7]);
            want.value2 = std::stoi(r.tokens[8]);
            want.text = atomText(want.kind, r.fields.at(0));
            compareAtom(where + " " + r.tokens[1], out, want);
            std::string rendered;
            bool rv = renderJttyAtom(out, rendered);
            check(rv == flag(r.tokens[9]), where + " render valid");
            if (rv) check(rtrim(rendered) == r.fields.at(1),
                          where + " rendered got '" + rendered + "' want '" + r.fields.at(1) + "'");
            continue;
        }
        if (tag == "CALL") {
            ++calls;
            std::string call = rtrim(r.fields.at(0));
            int n28 = pack28(call);
            check(n28 == std::stoi(r.tokens[1]),
                  where + " '" + call + "' pack28 got " + std::to_string(n28) + " want " + r.tokens[1]);
            std::string unp;
            bool success = unpack28(n28, unp);
            std::string wantUnp = rtrim(r.fields.at(1));
            // The reference generator's pack28 committed every nonstandard
            // call to its hash table, so its unpack28 recalled "<CALL>". This
            // port keeps no table and answers "<...>", exactly as the reference
            // does from an empty table; the JTTY grammar rejects both forms.
            bool bracketedRecall = wantUnp.size() > 5 && wantUnp.front() == '<' &&
                                   wantUnp.back() == '>' && wantUnp != "<...>";
            if (bracketedRecall) {
                check(rtrim(unp) == "<...>",
                      where + " '" + call + "' unpack28 (hashed) got '" + unp + "' want '<...>'");
                check(!standardCall(wantUnp), where + " '" + wantUnp + "' must not be a standard call");
            } else {
                check(rtrim(unp) == wantUnp,
                      where + " '" + call + "' unpack28 got '" + unp + "' want '" + wantUnp + "'");
            }
            check(success == flag(r.tokens[2]), where + " '" + call + "' unpack28 success");
            check(standardCall(call) == flag(r.tokens[3]), where + " '" + call + "' standardCall");
            continue;
        }
        if (tag == "N28") {
            ++n28s;
            int n28 = std::stoi(r.tokens[1]);
            std::string unp;
            bool success = unpack28(n28, unp);
            check(rtrim(unp) == rtrim(r.fields.at(0)),
                  where + " " + r.tokens[1] + " got '" + unp + "' want '" + rtrim(r.fields.at(0)) + "'");
            check(success == flag(r.tokens[2]), where + " " + r.tokens[1] + " success");
            continue;
        }
        if (tag == "MSGIN") {
            pendingRaw = r.fields.at(0);
            continue;
        }
        if (tag == "MSGNORM") {
            ++msgs;
            std::string norm = normalizeJttyMessage(pendingRaw);
            check(rtrim(norm) == rtrim(r.fields.at(0)),
                  where + " normalize of '" + rtrim(pendingRaw) + "' got '" + rtrim(norm) + "' want '" +
                      rtrim(r.fields.at(0)) + "'");
            expectPack = true;   // the next ASM record is the reference's packing of this message
            continue;
        }
        if (tag == "ASM") {
            int n = std::stoi(r.tokens[1]);
            if (expectPack) {
                expectPack = false;
                ++packs;
                std::vector<SourceFrame> mine;
                std::string normalized;
                int got = packJtty(pendingRaw, mine, true, nullptr, &normalized);
                std::string w = "PACK line " + std::to_string(lineNo) + " '" + rtrim(pendingRaw) + "'";
                check(got == n, w + " frame count got " + std::to_string(got) + " want " + std::to_string(n));
                if (got == n && n >= 1) {
                    for (int i = 0; i < n; ++i)
                        check(frameToString(mine[i]) == r.tokens.at(2 + i),
                              w + " frame " + std::to_string(i + 1) + " got " + frameToString(mine[i]) +
                                  " want " + r.tokens.at(2 + i));
                    std::vector<int> tones(static_cast<std::size_t>(59) * n);
                    int ntones = genJttyFrames(mine.data(), n, tones.data());
                    check(ntones == 59 * n, w + " tone count");
                    for (int k = 0; k < 13; ++k) check(tones[k] == SOURCE_SYNC_CHECK[k], w + " sync tone " + std::to_string(k));
                }
            }
            if (n < 1) continue;
            std::vector<SourceFrame> frames;
            for (int i = 0; i < n; ++i) frames.push_back(frameFromString(r.tokens.at(2 + i)));
            // the ASMOUT record follows immediately
            std::string outLine;
            std::getline(in, outLine);
            ++lineNo;
            Record o = parse(outLine);
            ++asms;
            AssembledMessage m = unpackJtty(frames.data(), n);
            std::string w = "ASMOUT line " + std::to_string(lineNo);
            check(rtrim(m.text) == rtrim(o.fields.at(0)),
                  w + " text got '" + rtrim(m.text) + "' want '" + rtrim(o.fields.at(0)) + "'");
            check(m.trailingSeparator == flag(o.tokens[1]), w + " trailingSeparator");
            check(m.isLastFrame == flag(o.tokens[2]), w + " isLastFrame");
            check(m.sourceValid == flag(o.tokens[3]), w + " sourceValid");
            continue;
        }
        fail(where + ": unknown record");
    }

    std::printf("vectors: %d atoms, %d words, %d calls, %d n28 values, %d messages, %d packings, %d assemblies\n",
                atoms, words, calls, n28s, msgs, packs, asms);
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
