/**
 * @file jtty_source.cpp
 * @brief JTTY source grammar: atoms, the call28 codec, message assembly.
 *
 * Direct port of WSJT-X (tag v3.3.0-beta1):
 *   lib/jtty/jtty_source_codec.f90   -> packJttyAtom, unpackJttyAtom, renderJttyAtom
 *   lib/jtty/jtty_mod.f90            -> normalizeJttyMessage, unpackJtty, standardCall
 *   lib/77bit/packjt77.f90           -> pack28, unpack28, callok, ihashcall
 *   lib/77bit/packjt77_grammar.f90   -> ARRL_SECTION
 *   lib/ft2_deps/chkcall.f90         -> chkcall
 *
 * The call28 hash table is deliberately absent: JTTY can only carry standard
 * calls, a hashed call unpacks to "<...>" as it does from an empty table, and
 * the grammar rejects it. Every rule here is held to the reference by
 * tests_jtty/jtty_source_test.cpp.
 */

#ifdef JS8_ENABLE_JTTY

#include "jtty_source.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace JTTY {

const char SOURCE_ALPHABET[65] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";

const char *const CONTROL_TEXT[CONTROL_PHRASES] = {
    "AGN?",  "CALL?",    "AGN CALL", "NR?",   "AGN NR", "EXCH?",
    "STATE?", "SECTION?", "ZONE?",   "GRID?", "RPRT?",  "QSL TU",
    "TU",    "QRZ?",     "QSO B4",   "WAIT",  "NIL?",   "OK?"};

const char *const ARRL_SECTION[ARRL_SECTIONS] = {
    "AB",  "AK",  "AL",  "AR",  "AZ",  "BC",  "CO",  "CT",  "DE",  "EB",
    "EMA", "ENY", "EPA", "EWA", "GA",  "GH",  "IA",  "ID",  "IL",  "IN",
    "KS",  "KY",  "LA",  "LAX", "NS",  "MB",  "MDC", "ME",  "MI",  "MN",
    "MO",  "MS",  "MT",  "NC",  "ND",  "NE",  "NFL", "NH",  "NL",  "NLI",
    "NM",  "NNJ", "NNY", "TER", "NTX", "NV",  "OH",  "OK",  "ONE", "ONN",
    "ONS", "OR",  "ORG", "PAC", "PR",  "QC",  "RI",  "SB",  "SC",  "SCV",
    "SD",  "SDG", "SF",  "SFL", "SJV", "SK",  "SNJ", "STX", "SV",  "TN",
    "UT",  "VA",  "VI",  "VT",  "WCF", "WI",  "WMA", "WNY", "WPA", "WTX",
    "WV",  "WWA", "WY",  "DX",  "PE",  "NB"};

namespace {

constexpr int NTOKENS = 2063592;
constexpr int MAX22 = 4194304;
constexpr int CALL_CHARS = 13;   // Fortran character*13

const char BASE36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char A1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char A4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isLetter(char c) { return c >= 'A' && c <= 'Z'; }

// Fortran index(set, c): 1-based position or 0.
inline int findex(const char *set, char c) {
    if (c == '\0') return 0;
    const char *p = std::strchr(set, c);
    return p ? static_cast<int>(p - set) + 1 : 0;
}

inline int sourceIndex(char c) { return findex(SOURCE_ALPHABET, c) - 1; }
inline char sourceChar(int i) { return SOURCE_ALPHABET[i]; }

std::string pad(const std::string &s, std::size_t n) {
    std::string r = s.substr(0, n);
    r.resize(n, ' ');
    return r;
}

std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// adjustl for a fixed-width field: leading blanks move to the end.
std::string adjustl(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return pad(s.substr(i), s.size());
}

inline std::uint64_t ibits(std::uint64_t v, int pos, int len) {
    return (v >> pos) & ((std::uint64_t{1} << len) - 1);
}

std::uint64_t pow36(int n) {
    std::uint64_t p = 1;
    for (int i = 0; i < n; ++i) p *= 36;
    return p;
}

bool validNumber(int kind, int value) {
    bool ok = kind >= 0 && kind <= 7 && value >= 0 && value <= 131071;
    if (!ok) return false;
    if (kind == NUM_CQ_ZONE) ok = value >= 1 && value <= 40;
    if (kind == NUM_ITU_ZONE) ok = value >= 1 && value <= 90;
    if (kind == NUM_LICENSE_YEAR) ok = value <= 9999;
    return ok;
}

bool validRole(int role) { return role == ROLE_FIELD_ONLY || role == ROLE_FULL; }

bool packBase36(const std::string &token, std::uint64_t &value) {
    value = 0;
    if (token.size() != 2 && token.size() != 3) return false;
    for (char c : token) {
        int digit = findex(BASE36, c) - 1;
        if (digit < 0) return false;
        value = 36 * value + static_cast<std::uint64_t>(digit);
    }
    if (token.size() == 3 && value < 36 * 36) return false;
    return true;
}

std::string unpackBase36(std::uint64_t value, int n) {
    std::string token(static_cast<std::size_t>(n), '0');
    std::uint64_t work = value;
    for (int i = n - 1; i >= 0; --i) {
        token[i] = BASE36[work % 36];
        work /= 36;
    }
    return token;
}

bool grid4ToIndex(const std::string &gridIn, int &index) {
    index = 0;
    std::string grid = rtrim(gridIn);
    if (grid.size() != 4) return false;
    int a = grid[0] - 'A', b = grid[1] - 'A', c = grid[2] - '0', d = grid[3] - '0';
    bool valid = a >= 0 && a < 18 && b >= 0 && b < 18 && c >= 0 && c < 10 && d >= 0 && d < 10;
    if (valid) index = ((a * 18 + b) * 10 + c) * 10 + d;
    return valid;
}

std::string indexToGrid4(int index) {
    int work = index;
    int d = work % 10;
    work /= 10;
    int c = work % 10;
    work /= 10;
    int b = work % 18;
    int a = work / 18;
    std::string g;
    g += static_cast<char>('A' + a);
    g += static_cast<char>('A' + b);
    g += static_cast<char>('0' + c);
    g += static_cast<char>('0' + d);
    return g;
}

inline std::uint64_t finishStruct(std::uint64_t body, int family) {
    return 4 * (8 * body + static_cast<std::uint64_t>(family)) + 2;
}

std::string renderNumber(int kind, int value) {
    char buf[32];
    switch (kind) {
    case NUM_SERIAL:
        std::snprintf(buf, sizeof buf, value < 1000 ? "%03d" : "%d", value);
        break;
    case NUM_CQ_ZONE:
    case NUM_ITU_ZONE:
    case NUM_CHECK:
        std::snprintf(buf, sizeof buf, value < 100 ? "%02d" : "%d", value);
        break;
    case NUM_LICENSE_YEAR:
        std::snprintf(buf, sizeof buf, "%04d", value);
        break;
    default:
        std::snprintf(buf, sizeof buf, "%d", value);
        break;
    }
    return buf;
}

std::string renderRole(int role, const std::string &field) {
    return role == ROLE_FULL ? "599 " + field : field;
}

// packjt77's ihashcall: base-38 over the first 11 characters, then the top
// m bits of a wrapping 64-bit multiply.
int ihashcall(const std::string &c0In, int m) {
    static const char c[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
    std::string c0 = pad(c0In, CALL_CHARS);
    std::uint64_t n8 = 0;
    for (int i = 0; i < 11; ++i) {
        std::int64_t j = findex(c, c0[i]) - 1;
        n8 = 38 * n8 + static_cast<std::uint64_t>(j);
    }
    std::uint64_t product = 47055833459ULL * n8;
    return static_cast<int>(product >> (64 - m));
}

} // namespace

/******************************************************************************/
// call28 codec
/******************************************************************************/

bool chkcall(const std::string &callIn, std::string *baseCall) {
    std::string w = pad(callIn, CALL_CHARS);
    std::string bc = w.substr(0, 6);
    bool cok = false;
    auto done = [&]() {
        if (baseCall) *baseCall = rtrim(bc);
        return cok;
    };
    int n1 = static_cast<int>(rtrim(w).size());
    if (n1 > 11) return done();
    if (w.find('.') != std::string::npos) return done();
    if (w.find('+') != std::string::npos) return done();
    if (w.find('-') != std::string::npos) return done();
    if (w.find('?') != std::string::npos) return done();
    if (n1 > 6 && w.find('/') == std::string::npos) return done();

    int i0 = findex(w.c_str(), '/');
    if (std::max(i0 - 1, n1 - i0) > 6) return done();   // base call must be < 7 characters
    if (i0 >= 2 && i0 <= n1 - 1) {                       // extract base call from compound call
        if (i0 - 1 <= n1 - i0) bc = pad(w.substr(i0, n1 - i0) + "   ", 6);
        if (i0 - 1 > n1 - i0) bc = pad(w.substr(0, i0 - 1) + "   ", 6);
    }

    int nbc = static_cast<int>(rtrim(bc).size());
    if (nbc > 6) return done();
    if (!isLetter(bc[0]) && !isLetter(bc[1])) return done();
    if (bc[0] == 'Q' && bc.substr(0, 5) != "QU1RK") return done();
    int i1 = 0;
    if (isDigit(bc[1])) i1 = 2;
    if (isDigit(bc[2])) i1 = 3;
    if (i1 == 0) return done();
    if (i1 == nbc) return done();
    int n = 0;
    for (int i = i1 + 1; i <= nbc; ++i) {
        char c = bc[i - 1];
        if (c < 'A' || c > 'Z') return done();
        ++n;
    }
    cok = n >= 1 && n <= 3;
    return done();
}

bool callok(const std::string &wIn) {
    auto islet = [](char c) { return (c >= 65 && c <= 90) || (c >= 97 && c <= 122); };
    std::string w = rtrim(wIn);
    int n = static_cast<int>(w.size());
    if (n < 3) return false;
    if (w[0] == 'Q') return false;
    int i;
    for (i = n; i >= 1; --i)
        if (isDigit(w[i - 1])) break;
    int i0 = i;   // 0 when no digit: the loop ran out
    if (i0 != 2 && i0 != 3) return false;
    std::string pfx = pad(w.substr(0, i0 - 1), 2);   // character*2
    int nlp = 0, ndp = 0;
    std::string p = rtrim(pfx);
    for (char c : p) {
        if (isDigit(c)) ++ndp;
        if (islet(c)) ++nlp;
    }
    if (nlp + ndp != static_cast<int>(p.size())) return false;   // prefix must be alphanumeric
    if (nlp == 0) return false;                                   // and hold a letter
    int ns = n - i0;
    if (ns < 1 || ns > 3) return false;
    int nls = 0;
    for (int k = i0 + 1; k <= n; ++k)
        if (islet(w[k - 1])) ++nls;
    if (nls < ns) return false;   // suffix must be all letters
    return true;
}

namespace {

// packjt77_grammar's c28 shape predicate: 3 to 6 alphanumerics, a call-area
// digit in position 2 or 3, at least one letter before it and not all
// digits there, and at most three letters after it.
bool c28StandardShape(const std::string &tokenIn) {
    std::string token = rtrim(tokenIn);
    int n = static_cast<int>(token.size());
    if (n < 3 || n > 6) return false;
    for (char c : token)
        if (!isLetter(c) && !isDigit(c)) return false;
    int iarea = 0;
    for (int i = n; i >= 2; --i)
        if (isDigit(token[i - 1])) {
            iarea = i;
            break;
        }
    if (iarea != 2 && iarea != 3) return false;
    int npdig = 0, nplet = 0;
    for (int i = 1; i <= iarea - 1; ++i) {
        char c = token[i - 1];
        if (isDigit(c))
            ++npdig;
        else if (isLetter(c))
            ++nplet;
        else
            return false;
    }
    if (nplet == 0 || npdig >= iarea - 1) return false;
    int nslet = 0;
    for (int i = iarea + 1; i <= n; ++i) {
        char c = token[i - 1];
        if (isLetter(c))
            ++nslet;
        else
            return false;
    }
    return nslet <= 3;
}

// CQ modifier: exactly three digits, or one to four letters.
bool validCqModifier(const std::string &tokenIn) {
    std::string token = rtrim(tokenIn);
    int n = static_cast<int>(token.size());
    if (n < 1 || n > 4) return false;
    int nletters = 0, ndigits = 0;
    for (char c : token) {
        if (isLetter(c))
            ++nletters;
        else if (isDigit(c))
            ++ndigits;
        else
            return false;
    }
    return (ndigits == 3 && nletters == 0) || (nletters >= 1 && nletters <= 4 && ndigits == 0);
}

// <...> token: 5 to 13 characters, bracketed, no nested brackets.
bool validHashCallToken(const std::string &tokenIn) {
    std::string token = rtrim(tokenIn);
    int n = static_cast<int>(token.size());
    if (n < 5 || n > 13) return false;
    if (token[0] != '<' || token[n - 1] != '>') return false;
    std::string inner = token.substr(1, n - 2);
    if (inner.find('<') != std::string::npos) return false;
    if (inner.find('>') != std::string::npos) return false;
    return true;
}

} // namespace

int pack28(const std::string &callIn) {
    std::string c13 = pad(callIn, CALL_CHARS);
    auto mask = [](long long v) { return static_cast<int>(v & ((1 << 28) - 1)); };

    auto hashed = [&](const std::string &s) {
        return mask(static_cast<long long>(NTOKENS) + ihashcall(s, 22));
    };

    // special tokens
    if (c13.compare(0, 3, "DE ") == 0) return mask(0);
    if (c13.compare(0, 4, "QRZ ") == 0) return mask(1);
    if (c13.compare(0, 3, "CQ ") == 0) return mask(2);
    if (c13.compare(0, 3, "CQ_") == 0) {
        int n = static_cast<int>(rtrim(c13).size());
        // The modifier grammar (1-4 letters or exactly 3 digits) is the
        // grammar module's; only the number-space arithmetic lives here.
        if (n >= 4 && validCqModifier(c13.substr(3, n - 3))) {
            std::string modifier = c13.substr(3, n - 3);
            bool allDigits = true;
            for (char c : modifier) allDigits = allDigits && isDigit(c);
            if (allDigits) {
                int nqsy = 100 * (c13[3] - '0') + 10 * (c13[4] - '0') + (c13[5] - '0');
                return mask(3 + nqsy);
            }
            std::string c4 = modifier;
            while (c4.size() < 4) c4 = " " + c4;   // adjustr into character*4
            int m = 0;
            for (int i = 0; i < 4; ++i) {
                int j = isLetter(c4[i]) ? c4[i] - 'A' + 1 : 0;
                m = 27 * m + j;
            }
            return mask(3 + 1000 + m);
        }
    }

    // <...> callsign: hashed when well formed, the token-space floor otherwise
    if (c13[0] == '<') {
        if (!validHashCallToken(c13)) return mask(NTOKENS);
        std::size_t i2 = c13.find('>');
        return hashed(c13.substr(1, i2 - 1));
    }

    // Malformed tokens are encoded as 22-bit hashes, never coerced to
    // standard calls, per the c28 shape predicate.
    if (!c28StandardShape(c13)) return hashed(c13);

    int n = static_cast<int>(rtrim(c13).size());
    int i;
    for (i = n; i >= 2; --i)
        if (isDigit(c13[i - 1])) break;
    int iarea = i;
    std::string callsign = iarea == 2 ? " " + c13.substr(0, 5) : c13.substr(0, 6);
    int i1 = findex(A1, callsign[0]) - 1;
    int i2 = findex(BASE36, callsign[1]) - 1;
    int i3 = findex("0123456789", callsign[2]) - 1;
    int i4 = findex(A4, callsign[3]) - 1;
    int i5 = findex(A4, callsign[4]) - 1;
    int i6 = findex(A4, callsign[5]) - 1;
    if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0 || i6 < 0) return hashed(c13);
    long long n28 = 36LL * 10 * 27 * 27 * 27 * i1 + 10LL * 27 * 27 * 27 * i2 +
                    27LL * 27 * 27 * i3 + 27LL * 27 * i4 + 27LL * i5 + i6;
    n28 += NTOKENS + MAX22;
    return mask(n28);
}

bool unpack28(int n28_0, std::string &call) {
    bool success = true;
    int n28 = n28_0;
    std::string c13;
    auto finish = [&]() {   // label 900: no internal blank allowed
        std::string p = pad(c13, CALL_CHARS);
        std::size_t i0 = p.find(' ');
        int lt = static_cast<int>(rtrim(p).size());
        if (i0 != std::string::npos && static_cast<int>(i0) + 1 < lt) {
            p = pad("QU1RK", CALL_CHARS);
            success = false;
        }
        call = rtrim(p);
        return success;
    };

    if (n28 < NTOKENS) {
        if (n28 == 0) c13 = "DE";
        if (n28 == 1) c13 = "QRZ";
        if (n28 == 2) c13 = "CQ";
        if (n28 <= 2) return finish();
        if (n28 <= 1002) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "CQ_%03d", n28 - 3);
            c13 = buf;
            return finish();
        }
        if (n28 <= 532443) {
            int n = n28 - 1003;
            int i1 = n / (27 * 27 * 27);
            n -= 27 * 27 * 27 * i1;
            int i2 = n / (27 * 27);
            n -= 27 * 27 * i2;
            int i3 = n / 27;
            int i4 = n - 27 * i3;
            std::string t;
            t += A4[i1];
            t += A4[i2];
            t += A4[i3];
            t += A4[i4];
            t = adjustl(pad(t, CALL_CHARS));
            c13 = "CQ_" + t.substr(0, 10);
            return finish();
        }
    }
    n28 -= NTOKENS;
    if (n28 < MAX22) {
        c13 = "<...>";   // 22-bit hash: no table here, as an empty table answers
        return finish();
    }

    // standard callsign
    int n = n28 - MAX22;
    int i1 = n / (36 * 10 * 27 * 27 * 27);
    n -= 36 * 10 * 27 * 27 * 27 * i1;
    int i2 = n / (10 * 27 * 27 * 27);
    n -= 10 * 27 * 27 * 27 * i2;
    int i3 = n / (27 * 27 * 27);
    n -= 27 * 27 * 27 * i3;
    int i4 = n / (27 * 27);
    n -= 27 * 27 * i4;
    int i5 = n / 27;
    int i6 = n - 27 * i5;
    std::string t;
    t += A1[i1];
    t += BASE36[i2];
    t += "0123456789"[i3];
    t += A4[i4];
    t += A4[i5];
    t += A4[i6];
    c13 = adjustl(pad(t, CALL_CHARS));
    if (!callok(rtrim(c13))) {
        c13 = "QU1RK";
        success = false;
    }
    return finish();
}

bool standardCall(const std::string &callIn) {
    std::string work = pad(callIn, CALL_CHARS);
    std::string base;
    bool ok = chkcall(work, &base);
    if (!ok || work.find('/') != std::string::npos || work[0] == 'Q') return false;
    for (char c : rtrim(work))
        if (!isLetter(c) && !isDigit(c)) return false;
    int n28 = pack28(work);
    std::string unpacked;
    bool success = unpack28(n28, unpacked);
    return success && rtrim(unpacked) == rtrim(work);
}

/******************************************************************************/
// Atoms
/******************************************************************************/

bool packJttyAtom(const SourceAtom &atom, SourceFrame &frame, bool eom) {
    std::fill(frame.bits, frame.bits + PAYLOAD_BITS, 0);
    std::uint64_t n32 = 0;
    switch (atom.kind) {
    case ATOM_CALL: {
        if (atom.subtype < 0 || atom.subtype > 5) return false;
        std::string callText = pad(atom.text, CALL_CHARS);
        if (!standardCall(callText)) return false;
        int n28 = pack28(callText);
        std::string roundtrip;
        bool success = unpack28(n28, roundtrip);
        if (!success || rtrim(roundtrip) != rtrim(callText)) return false;
        int i2, n2;
        if (atom.subtype <= 3) {
            i2 = 0;
            n2 = atom.subtype;
        } else {
            i2 = 1;
            n2 = atom.subtype - 4;
        }
        n32 = (static_cast<std::uint64_t>(n28) << 4) + 4 * n2 + i2;
        break;
    }
    case ATOM_EXCH_NUM: {
        if (!validRole(atom.role) || !validNumber(atom.subtype, atom.value)) return false;
        std::uint64_t body = (static_cast<std::uint64_t>(atom.role) << 26) +
                             (static_cast<std::uint64_t>(atom.subtype) << 22) +
                             (static_cast<std::uint64_t>(atom.value) << 5);
        n32 = finishStruct(body, 0);
        break;
    }
    case ATOM_EXCH_LOC: {
        if (!validRole(atom.role) || atom.subtype < 0 || atom.subtype > 4) return false;
        std::string t = rtrim(atom.text);
        int n = static_cast<int>(t.size());
        if (n != 2 && n != 3) return false;
        std::uint64_t tokenValue;
        if (!packBase36(t, tokenValue)) return false;
        std::uint64_t body = (static_cast<std::uint64_t>(atom.role) << 26) +
                             (static_cast<std::uint64_t>(atom.subtype) << 22) +
                             (static_cast<std::uint64_t>(n - 2) << 21) + (tokenValue << 5);
        n32 = finishStruct(body, 1);
        break;
    }
    case ATOM_EXCH_PAIR: {
        std::uint64_t pairData;
        switch (atom.subtype) {
        case PAIR_ZONE_LOC3: {
            if (atom.value < 1 || atom.value > 40) return false;
            std::string t = rtrim(atom.text);
            int n = static_cast<int>(t.size());
            if (n != 2 && n != 3) return false;
            std::uint64_t tokenValue;
            if (!packBase36(t, tokenValue)) return false;
            pairData = (static_cast<std::uint64_t>(atom.value) << 17) +
                       (static_cast<std::uint64_t>(n - 2) << 16) + tokenValue;
            break;
        }
        case PAIR_CLASS_SECTION: {
            if (atom.value < 1 || atom.value > 32) return false;
            if (atom.value2 < 1 || atom.value2 > ARRL_SECTIONS || rtrim(atom.text).size() != 1)
                return false;
            int classIndex = pad(atom.text, 1)[0] - 'A';
            if (classIndex < 0 || classIndex > 5) return false;
            pairData = (static_cast<std::uint64_t>(atom.value) << 17) +
                       (static_cast<std::uint64_t>(classIndex) << 14) +
                       (static_cast<std::uint64_t>(atom.value2) << 7);
            break;
        }
        default:
            return false;
        }
        std::uint64_t body = (static_cast<std::uint64_t>(atom.subtype) << 24) + (pairData << 1);
        n32 = finishStruct(body, 2);
        break;
    }
    case ATOM_EXCH_NUM_TIME: {
        if (!validRole(atom.role) || atom.value < 0 || atom.value > 16383) return false;
        if (atom.value2 < 0 || atom.value2 > 1439) return false;
        std::uint64_t body = (static_cast<std::uint64_t>(atom.role) << 26) +
                             (static_cast<std::uint64_t>(atom.value) << 12) +
                             (static_cast<std::uint64_t>(atom.value2) << 1);
        n32 = finishStruct(body, 3);
        break;
    }
    case ATOM_CONTROL: {
        if (atom.subtype < 0 || atom.subtype > 17) return false;
        std::uint64_t body = (static_cast<std::uint64_t>(MISC_CONTROL) << 23) +
                             (static_cast<std::uint64_t>(atom.subtype) << 16);
        n32 = finishStruct(body, 4);
        break;
    }
    case ATOM_GRID4: {
        if (!validRole(atom.role)) return false;
        int gridIndex;
        if (!grid4ToIndex(atom.text, gridIndex)) return false;
        std::uint64_t data = (static_cast<std::uint64_t>(atom.role) << 22) +
                             (static_cast<std::uint64_t>(gridIndex) << 7);
        std::uint64_t body = (static_cast<std::uint64_t>(MISC_GRID4) << 23) + data;
        n32 = finishStruct(body, 4);
        break;
    }
    case ATOM_TEXT5: {
        std::string text5 = pad(atom.text, 5);
        std::uint64_t top30 = 0;
        for (int i = 0; i < 5; ++i) {
            int idx = sourceIndex(text5[i]);
            if (idx < 0) return false;
            top30 = 64 * top30 + static_cast<std::uint64_t>(idx);
        }
        n32 = 4 * top30 + 3;
        break;
    }
    default:
        return false;
    }
    for (int b = 0; b < 32; ++b) frame.bits[b] = static_cast<std::int8_t>((n32 >> (31 - b)) & 1);
    frame.bits[32] = 0;
    frame.bits[33] = eom ? 1 : 0;
    return true;
}

bool unpackJttyAtom(const SourceFrame &frame, SourceAtom &atom, bool &eom) {
    atom = SourceAtom{};
    eom = false;
    for (int b = 0; b < PAYLOAD_BITS; ++b)
        if (frame.bits[b] != 0 && frame.bits[b] != 1) return false;
    if (frame.bits[32] != 0) return false;
    std::uint64_t n32 = 0;
    for (int b = 0; b < 32; ++b) n32 = (n32 << 1) | static_cast<std::uint64_t>(frame.bits[b]);
    if (n32 == 0) return false;
    std::uint64_t top30 = n32 >> 2;
    int i2 = static_cast<int>(n32 & 3);

    switch (i2) {
    case 0:
    case 1: {
        int n28 = static_cast<int>(n32 >> 4);
        int n2 = static_cast<int>((n32 >> 2) & 3);
        if (i2 == 1 && n2 > 1) return false;
        std::string callText;
        bool success = unpack28(n28, callText);
        if (!success || !standardCall(callText)) return false;
        atom.kind = ATOM_CALL;
        atom.subtype = i2 == 0 ? n2 : n2 + 4;
        atom.text = rtrim(callText);
        break;
    }
    case 2: {
        int family = static_cast<int>(top30 & 7);
        std::uint64_t body = top30 >> 3;
        switch (family) {
        case 0: {
            atom.kind = ATOM_EXCH_NUM;
            atom.role = static_cast<int>(ibits(body, 26, 1));
            atom.subtype = static_cast<int>(ibits(body, 22, 4));
            atom.value = static_cast<int>(ibits(body, 5, 17));
            int zero = static_cast<int>(ibits(body, 0, 5));
            if (zero != 0 || !validNumber(atom.subtype, atom.value)) return false;
            break;
        }
        case 1: {
            atom.kind = ATOM_EXCH_LOC;
            atom.role = static_cast<int>(ibits(body, 26, 1));
            atom.subtype = static_cast<int>(ibits(body, 22, 4));
            int n = static_cast<int>(ibits(body, 21, 1)) + 2;
            std::uint64_t tokenValue = ibits(body, 5, 16);
            int zero = static_cast<int>(ibits(body, 0, 5));
            if (zero != 0 || atom.subtype > 4 || tokenValue >= pow36(n)) return false;
            if (n == 3 && tokenValue < 36 * 36) return false;
            atom.text = unpackBase36(tokenValue, n);
            break;
        }
        case 2: {
            atom.kind = ATOM_EXCH_PAIR;
            atom.subtype = static_cast<int>(ibits(body, 24, 3));
            std::uint64_t pairData = ibits(body, 1, 23);
            int zero = static_cast<int>(ibits(body, 0, 1));
            if (zero != 0) return false;
            switch (atom.subtype) {
            case PAIR_ZONE_LOC3: {
                atom.value = static_cast<int>(ibits(pairData, 17, 6));
                int n = static_cast<int>(ibits(pairData, 16, 1)) + 2;
                std::uint64_t tokenValue = ibits(pairData, 0, 16);
                if (atom.value < 1 || atom.value > 40 || tokenValue >= pow36(n)) return false;
                if (n == 3 && tokenValue < 36 * 36) return false;
                atom.text = unpackBase36(tokenValue, n);
                break;
            }
            case PAIR_CLASS_SECTION: {
                atom.value = static_cast<int>(ibits(pairData, 17, 6));
                int classIndex = static_cast<int>(ibits(pairData, 14, 3));
                atom.value2 = static_cast<int>(ibits(pairData, 7, 7));
                int zero7 = static_cast<int>(ibits(pairData, 0, 7));
                if (atom.value < 1 || atom.value > 32 || classIndex > 5) return false;
                if (atom.value2 < 1 || atom.value2 > ARRL_SECTIONS || zero7 != 0) return false;
                atom.text = std::string(1, static_cast<char>('A' + classIndex));
                break;
            }
            default:
                return false;
            }
            break;
        }
        case 3: {
            atom.kind = ATOM_EXCH_NUM_TIME;
            atom.role = static_cast<int>(ibits(body, 26, 1));
            atom.value = static_cast<int>(ibits(body, 12, 14));
            atom.value2 = static_cast<int>(ibits(body, 1, 11));
            int zero = static_cast<int>(ibits(body, 0, 1));
            if (zero != 0 || atom.value2 > 1439) return false;
            break;
        }
        case 4: {
            int i = static_cast<int>(ibits(body, 23, 4));
            std::uint64_t data = ibits(body, 0, 23);
            switch (i) {
            case MISC_CONTROL: {
                atom.kind = ATOM_CONTROL;
                atom.subtype = static_cast<int>(ibits(data, 16, 7));
                int zero = static_cast<int>(ibits(data, 0, 16));
                if (zero != 0 || atom.subtype > 17) return false;
                break;
            }
            case MISC_GRID4: {
                atom.kind = ATOM_GRID4;
                atom.role = static_cast<int>(ibits(data, 22, 1));
                int gridIndex = static_cast<int>(ibits(data, 7, 15));
                int zero = static_cast<int>(ibits(data, 0, 7));
                if (zero != 0 || gridIndex >= 32400) return false;
                atom.text = indexToGrid4(gridIndex);
                break;
            }
            default:
                return false;
            }
            break;
        }
        default:
            return false;
        }
        break;
    }
    case 3: {
        atom.kind = ATOM_TEXT5;
        atom.text.clear();
        for (int i = 1; i <= 5; ++i)
            atom.text += sourceChar(static_cast<int>((top30 >> (6 * (5 - i))) & 63));
        break;
    }
    }
    eom = frame.bits[33] == 1;
    return true;
}

bool renderJttyAtom(const SourceAtom &atom, std::string &text) {
    text.clear();
    switch (atom.kind) {
    case ATOM_CALL: {
        std::string call = rtrim(atom.text);
        switch (atom.subtype) {
        case CALL_CQ: text = "CQ " + call + " CQ"; break;
        case CALL_CALL: text = call; break;
        case CALL_TU_CQ: text = "TU " + call + " CQ"; break;
        case CALL_CALL_TU: text = call + " TU"; break;
        case CALL_CALL_AGN: text = call + " AGN?"; break;
        case CALL_TU_NOW: text = "TU NOW " + call; break;
        default: return false;
        }
        break;
    }
    case ATOM_EXCH_NUM:
        if (!validRole(atom.role) || !validNumber(atom.subtype, atom.value)) return false;
        text = renderRole(atom.role, renderNumber(atom.subtype, atom.value));
        break;
    case ATOM_EXCH_LOC:
        if (atom.subtype < 0 || atom.subtype > 4) return false;
        text = renderRole(atom.role, rtrim(atom.text));
        break;
    case ATOM_EXCH_PAIR:
        switch (atom.subtype) {
        case PAIR_ZONE_LOC3: {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%02d", atom.value);
            text = std::string("599 ") + buf + " " + rtrim(atom.text);
            break;
        }
        case PAIR_CLASS_SECTION: {
            if (atom.value2 < 1 || atom.value2 > ARRL_SECTIONS) return false;
            char buf[16];
            std::snprintf(buf, sizeof buf, "%d%c", atom.value, pad(atom.text, 1)[0]);
            text = std::string(buf) + " " + ARRL_SECTION[atom.value2 - 1];
            break;
        }
        default:
            return false;
        }
        break;
    case ATOM_EXCH_NUM_TIME: {
        if (!validRole(atom.role) || atom.value < 0 || atom.value > 16383 || atom.value2 < 0 ||
            atom.value2 > 1439)
            return false;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02d%02d", atom.value2 / 60, atom.value2 % 60);
        text = renderRole(atom.role, renderNumber(NUM_SERIAL, atom.value) + " " + buf);
        break;
    }
    case ATOM_CONTROL:
        if (atom.subtype < 0 || atom.subtype > 17) return false;
        text = CONTROL_TEXT[atom.subtype];
        break;
    case ATOM_GRID4:
        text = renderRole(atom.role, pad(atom.text, 4));
        break;
    case ATOM_TEXT5:
        text = pad(atom.text, 5);
        break;
    default:
        return false;
    }
    if (text.size() > MESSAGE_CHARS) text.resize(MESSAGE_CHARS);
    return true;
}

/******************************************************************************/
// Messages
/******************************************************************************/

std::string normalizeJttyMessage(const std::string &raw) {
    std::string in = pad(raw, MESSAGE_CHARS);
    std::string out;
    bool lastSpace = true;
    for (char c : in) {
        if (c == '\0') c = ' ';
        // The decoder uses '~' as a display marker for space, not a source symbol.
        if (c == '~') c = ' ';
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (sourceIndex(c) < 0) c = '#';
        if (c == ' ') {
            if (lastSpace) continue;
            out += ' ';
            lastSpace = true;
        } else {
            out += c;
            lastSpace = false;
        }
    }
    return pad(out, MESSAGE_CHARS);
}

AssembledMessage unpackJtty(const SourceFrame *frames, int nframes) {
    AssembledMessage m;
    std::string message(MESSAGE_CHARS, ' ');
    int k = 1;
    bool lastFrameSep = false, lastFrameFlag = false;
    bool allValid = nframes >= 1 && nframes <= MAX_FRAMES;
    int count = std::max(0, std::min(nframes, MAX_FRAMES));
    for (int f = 0; f < count; ++f) {
        lastFrameSep = false;
        lastFrameFlag = false;
        SourceAtom atom;
        bool eom = false;
        if (!unpackJttyAtom(frames[f], atom, eom)) {
            allValid = false;
            continue;
        }
        std::string rendered;
        if (!renderJttyAtom(atom, rendered)) {
            allValid = false;
            continue;
        }
        lastFrameFlag = eom;
        if (atom.kind == ATOM_TEXT5) {
            std::string t = pad(atom.text, 5);
            for (int j = 0; j < 5; ++j) {
                char c = t[j];
                if (c == ' ') c = '~';
                if (k <= MESSAGE_CHARS) message[k - 1] = c;
                ++k;
            }
        } else {
            for (char c : rtrim(rendered)) {
                if (k <= MESSAGE_CHARS) message[k - 1] = c;
                ++k;
            }
            ++k;   // structured frames leave one blank column before any following frame
            lastFrameSep = true;
        }
    }
    m.text = message;
    m.trailingSeparator = lastFrameSep;
    m.isLastFrame = lastFrameFlag && allValid;
    m.sourceValid = allValid;
    return m;
}

/******************************************************************************/
// Automatic text packing (pack_jtty, Unknown exchange profile)
/******************************************************************************/

namespace {

constexpr int PACK_INF = 999;

int lenTrim(const std::string &s) { return static_cast<int>(rtrim(s).size()); }

// decimal_value: one to six digits, value at most 131071.
bool decimalValue(const std::string &text, int &value) {
    value = 0;
    if (text.size() < 1 || text.size() > 6) return false;
    for (char c : text)
        if (!isDigit(c)) return false;
    for (char c : text) value = 10 * value + (c - '0');
    return value <= 131071;
}

// pack77_arrl_section_index on a character(80) word: the first three
// columns, blank padded, against the registry.
int arrlSectionIndex(const std::string &word) {
    std::string s3 = pad(word, 3);
    for (int i = 0; i < ARRL_SECTIONS; ++i)
        if (pad(ARRL_SECTION[i], 3) == s3) return i + 1;
    return -1;
}

struct Packer {
    std::string msg;          // normalised, MESSAGE_CHARS wide
    int n = 0;                // len_trim(msg)
    int dp[MESSAGE_CHARS + 2];          // dp[i]: minimum frames for msg(i:n), 1-based
    int successor[MESSAGE_CHARS + 2];
    SourceAtom choice[MESSAGE_CHARS + 2];
    int ipos = 0;

    void consider(const SourceAtom &atom, int next) {
        const int cost = 1 + dp[next];
        if (cost > MAX_FRAMES || cost > dp[ipos]) return;
        const int rank = atom.kind == ATOM_TEXT5 ? 1 : 0;
        const int bestRank = choice[ipos].kind == ATOM_TEXT5 ? 1 : 0;
        const int key = 100 * atom.kind + 2 * atom.subtype + atom.role;
        const int bestKey = 100 * choice[ipos].kind + 2 * choice[ipos].subtype + choice[ipos].role;
        if (cost == dp[ipos]) {
            if (rank > bestRank) return;
            if (rank == bestRank) {
                if (next < successor[ipos]) return;
                if (next == successor[ipos] && key >= bestKey) return;
            }
        }
        dp[ipos] = cost;
        successor[ipos] = next;
        choice[ipos] = atom;
    }

    // A structured candidate must pack, unpack and render exactly as its
    // span of the text, and consume one following separator column.
    void offer(const SourceAtom &atom) {
        SourceFrame frame;
        if (!packJttyAtom(atom, frame, false)) return;
        SourceAtom decoded;
        bool eom;
        if (!unpackJttyAtom(frame, decoded, eom)) return;
        std::string rendered;
        if (!renderJttyAtom(decoded, rendered)) return;
        const int length = lenTrim(rendered);
        if (length == 0) return;
        const int last = ipos + length - 1;
        if (last > n) return;
        if (msg.compare(ipos - 1, length, rendered, 0, length) != 0) return;
        int next = n + 1;
        if (last < n) {
            if (msg[last] != ' ') return;   // msg(last+1:last+1)
            next = last + 2;
        }
        consider(atom, next);
    }

    void tryCompact() {
        std::string words[3];
        int nwords = 0;
        int first = ipos;
        for (int j = 1; j <= 3; ++j) {
            if (first > n) break;
            std::size_t sp = msg.find(' ', first - 1);
            int last = (sp == std::string::npos || static_cast<int>(sp) >= n) ? n : static_cast<int>(sp);
            nwords = j;
            words[j - 1] = msg.substr(first - 1, last - first + 1);
            first = last + 2;
        }

        for (int j = 0; j < nwords; ++j) {
            if (lenTrim(words[j]) > 13) continue;
            for (int action = CALL_CQ; action <= CALL_TU_NOW; ++action) {
                SourceAtom a;
                a.kind = ATOM_CALL;
                a.subtype = action;
                a.text = rtrim(words[j]);
                offer(a);
            }
        }
        for (int j = 0; j < CONTROL_PHRASES; ++j) {
            SourceAtom a;
            a.kind = ATOM_CONTROL;
            a.subtype = j;
            offer(a);
        }

        for (int role = ROLE_FIELD_ONLY; role <= ROLE_FULL; ++role) {
            int field = 1;
            if (role == ROLE_FULL) {
                if (rtrim(words[0]) != "599" || nwords < 2) continue;
                field = 2;
            }
            const std::string w = rtrim(words[field - 1]);
            const int length = static_cast<int>(w.size());
            int value;
            const bool numeric = decimalValue(w, value);
            if (numeric) {
                SourceAtom a;
                a.kind = ATOM_EXCH_NUM;
                a.role = role;
                a.subtype = NUM_GENERIC;
                a.value = value;
                offer(a);
            }
            if (length == 4) {
                SourceAtom a;
                a.kind = ATOM_GRID4;
                a.role = role;
                a.text = w.substr(0, 4);
                offer(a);
            }
            if (role != ROLE_FULL || length < 2 || length > 3) continue;
            bool hasLetter = false;
            for (char c : w) hasLetter = hasLetter || isLetter(c);
            if (!hasLetter) continue;
            SourceAtom a;
            a.kind = ATOM_EXCH_LOC;
            a.role = role;
            a.subtype = LOC_QTH;
            a.text = w;
            offer(a);
        }

        const std::string w1 = rtrim(words[0]);
        const int length = static_cast<int>(w1.size());
        if (nwords < 2 || length < 2 || length > 3) return;
        int value;
        if (!decimalValue(w1.substr(0, length - 1), value)) return;
        SourceAtom a;
        a.kind = ATOM_EXCH_PAIR;
        a.subtype = PAIR_CLASS_SECTION;
        a.value = value;
        a.value2 = arrlSectionIndex(words[1]);
        a.text = std::string(1, w1[length - 1]);
        offer(a);
    }
};

} // namespace

int packJtty(const std::string &message, std::vector<SourceFrame> &frames, bool isFinal,
             std::vector<int> *frameStarts, std::string *normalized) {
    frames.clear();
    if (frameStarts) frameStarts->clear();
    Packer p;
    p.msg = normalizeJttyMessage(message);
    if (normalized) *normalized = p.msg;
    p.n = lenTrim(p.msg);
    if (p.n <= 0) return 0;

    for (int i = 0; i < MESSAGE_CHARS + 2; ++i) {
        p.dp[i] = PACK_INF;
        p.successor[i] = 0;
        p.choice[i] = SourceAtom{};
    }
    p.dp[p.n + 1] = 0;
    for (p.ipos = p.n; p.ipos >= 1; --p.ipos) {
        const int inext = std::min(p.n + 1, p.ipos + 5);
        SourceAtom t;
        t.kind = ATOM_TEXT5;
        t.text = p.msg.substr(p.ipos - 1, inext - p.ipos);
        p.consider(t, inext);
        if (p.ipos > 1 && p.msg[p.ipos - 2] != ' ') continue;   // compact atoms start at token boundaries
        p.tryCompact();
    }
    if (p.dp[1] > MAX_FRAMES) return -1;

    std::vector<SourceAtom> atoms;
    int ipos = 1;
    while (ipos <= p.n) {
        atoms.push_back(p.choice[ipos]);
        if (frameStarts) frameStarts->push_back(ipos);
        ipos = p.successor[ipos];
    }
    const int natoms = static_cast<int>(atoms.size());
    if (natoms < 1 || natoms > MAX_FRAMES) return -1;
    frames.resize(natoms);
    for (int i = 0; i < natoms; ++i) {
        if (!packJttyAtom(atoms[i], frames[i], i == natoms - 1 && isFinal)) {
            frames.clear();
            return -1;
        }
    }
    return natoms;
}

int genJttyFrames(const SourceFrame *frames, int nframes, int *itone) {
    // is13 from jtty_fec_mod.f90; 13 sync + 46 channel symbols per frame.
    constexpr int syncSymbols = 13, frameSymbols = 59;
    static const int syncTones[syncSymbols] = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};
    int nsym = 0;
    for (int i = 0; i < nframes; ++i) {
        std::int8_t tones[INFO_BITS];
        tbccEncode(frames[i].bits, tones);
        const int ib = i * frameSymbols;
        for (int k = 0; k < syncSymbols; ++k) itone[ib + k] = syncTones[k];
        for (int k = 0; k < INFO_BITS; ++k) itone[ib + syncSymbols + k] = tones[k];
        nsym += frameSymbols;
    }
    return nsym;
}

} // namespace JTTY

#endif // JS8_ENABLE_JTTY
