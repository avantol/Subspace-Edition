#pragma once
#ifdef JS8_ENABLE_JTTY
#include "jtty_tbcc.h"

#include <cstdint>
#include <string>
#include <vector>

// JTTY source grammar: the 34-bit source word and its atoms, ported from
// WSJT-X lib/jtty (tag v3.3.0-beta1): jtty_source_codec.f90 (atom pack /
// unpack / render), jtty_mod.f90 (message assembly and normalisation) and
// the call28 codec from lib/77bit/packjt77.f90 (pack28 / unpack28, chkcall,
// callok) restricted to what JTTY can carry: standard calls only, no hash
// table -- a hashed call arrives as "<...>" and is rejected by the grammar,
// exactly as the reference rejects it.
//
// A frame is the FEC's 34-bit payload: bit 1 first (MSB of the 32-bit grammar
// word), bit 33 reserved-zero, bit 34 the end-of-message flag.

namespace JTTY {

constexpr int MAX_FRAMES = 16;
constexpr int MESSAGE_CHARS = 80;

// Character values 0..63 index this alphabet (TEXT5 and normalisation).
extern const char SOURCE_ALPHABET[65];

enum AtomKind : int {
    ATOM_CALL = 0, ATOM_EXCH_NUM = 1, ATOM_EXCH_LOC = 2, ATOM_EXCH_PAIR = 3,
    ATOM_EXCH_NUM_TIME = 4, ATOM_CONTROL = 5, ATOM_GRID4 = 6, ATOM_TEXT5 = 7
};
enum CallAction : int {
    CALL_CQ = 0, CALL_CALL = 1, CALL_TU_CQ = 2, CALL_CALL_TU = 3,
    CALL_CALL_AGN = 4, CALL_TU_NOW = 5
};
enum Role : int { ROLE_FIELD_ONLY = 0, ROLE_FULL = 1 };
enum NumberKind : int {
    NUM_SERIAL = 0, NUM_CQ_ZONE = 1, NUM_ITU_ZONE = 2, NUM_AGE = 3, NUM_POWER = 4,
    NUM_CHECK = 5, NUM_LICENSE_YEAR = 6, NUM_GENERIC = 7
};
enum LocationKind : int {
    LOC_STATE_PROVINCE = 0, LOC_SECTION = 1, LOC_COUNTRY_PREFIX = 2, LOC_QTH = 3,
    LOC_ADMIN_CODE = 4
};
enum PairSchema : int { PAIR_ZONE_LOC3 = 0, PAIR_CLASS_SECTION = 1 };
enum MiscSubtype : int { MISC_CONTROL = 0, MISC_GRID4 = 1 };

constexpr int CONTROL_PHRASES = 18;
extern const char *const CONTROL_TEXT[CONTROL_PHRASES];   // AGN? .. OK?

constexpr int ARRL_SECTIONS = 86;
extern const char *const ARRL_SECTION[ARRL_SECTIONS];     // 1-based in the grammar

struct SourceAtom {
    int kind = -1;
    int subtype = 0;
    int role = ROLE_FIELD_ONLY;
    int value = 0;
    int value2 = 0;
    // Up to 13 characters, trailing blanks trimmed -- except TEXT5, which
    // keeps exactly its five characters, spaces included.
    std::string text;
};

struct SourceFrame {
    std::int8_t bits[PAYLOAD_BITS];   // bits[0] is bit 1
};

// Atom <-> 34-bit frame. pack validates every field exactly as the
// reference and returns false for anything it will not put on the air.
bool packJttyAtom(const SourceAtom &atom, SourceFrame &frame, bool eom);
bool unpackJttyAtom(const SourceFrame &frame, SourceAtom &atom, bool &eom);

// Canonical rendering of an atom (no implicit trailing separator).
bool renderJttyAtom(const SourceAtom &atom, std::string &text);

// call28 codec, as packjt77 does it for the ranges JTTY can carry.
int pack28(const std::string &call);
bool unpack28(int n28, std::string &call);
bool chkcall(const std::string &call, std::string *baseCall = nullptr);
bool callok(const std::string &call);
// True only for tokens safe to carry in a 28-bit structured callsign field.
bool standardCall(const std::string &call);

// Fold operator text into the source alphabet: uppercase, '~' and NUL to
// space, unsupported characters to '#', spaces collapsed. 80-character
// field semantics: input truncated to 80, output at most 80.
std::string normalizeJttyMessage(const std::string &raw);

struct AssembledMessage {
    std::string text;              // exactly MESSAGE_CHARS, blank padded, '~' marks a TEXT5 space
    bool trailingSeparator = false;   // last frame processed appended an implicit separator
    bool isLastFrame = false;         // last frame processed carried EOM, and every frame was valid
    bool sourceValid = false;         // every frame passed grammar validation
};

// Assemble a received message from its frames (unpack_jtty): invalid frames
// are skipped and flagged; structured atoms leave one blank column after
// them; TEXT5 spaces display as '~'.
AssembledMessage unpackJtty(const SourceFrame *frames, int nframes);

// Automatic text packing (pack_jtty) under the Unknown exchange profile:
// normalise, then a dynamic program over character offsets selects the
// fewest frames from the context-free candidates -- the six call forms,
// registered control phrases, generic numbers, "599 " + base-36 location,
// GRID4 and CLASS_SECTION -- with TEXT5 as the fallback, tie-breaking as the
// specification orders. The RTTY Roundup and Field Day profiles are not
// carried. Returns the frame count, 0 for an empty message, -1 when the
// message does not fit sixteen frames. isFinal=false suppresses the EOM
// flag on the last frame so another message can be queued behind it.
// frameStarts receives the 1-based start column of each frame's text.
int packJtty(const std::string &message, std::vector<SourceFrame> &frames, bool isFinal = true,
             std::vector<int> *frameStarts = nullptr, std::string *normalized = nullptr);

// Frames -> channel tones for transmission (genjtty_frames): per frame, the
// 13 sync tones followed by the 46 FEC-encoded tones. itone needs
// 59*nframes entries; returns the tone count.
int genJttyFrames(const SourceFrame *frames, int nframes, int *itone);

} // namespace JTTY
#endif // JS8_ENABLE_JTTY
