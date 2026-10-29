#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <cstdint>
#include <filesystem>
#if __has_include(<poppler/cpp/poppler-document.h>)
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#else
#include "vcpkg_installed/x64-windows/x64-windows/include/poppler/cpp/poppler-document.h"
#include "vcpkg_installed/x64-windows/x64-windows/include/poppler/cpp/poppler-page.h"
#endif

#ifdef _MSC_VER
#ifdef _DEBUG
#pragma comment(lib, "vcpkg_installed/x64-windows/x64-windows/debug/lib/poppler-cpp.lib")
#else
#pragma comment(lib, "vcpkg_installed/x64-windows/x64-windows/lib/poppler-cpp.lib")
#endif
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);
#endif

struct Table1Row {
    std::string arm_word;
    std::string ru_word;
    std::string lat_word;
    std::string gr_word;
    std::string fr_word;
    std::string de_word;
    std::string en_word;
    std::string it_word;
    std::string es_word;
    std::string sphere;
    std::vector<std::string> ru_forms;
    std::vector<std::string> lat_forms;
    std::vector<std::string> latin_forms;
    bool has_ling_tag = false;
};

struct Table2Row {
    std::string en_raw;
    std::string en_clean;
    std::string arm_equiv;
    std::string arm_primary;
    std::vector<std::string> en_phrases;
    std::vector<std::string> primary_en;
    std::vector<std::string> syn_en;
    std::set<std::string> arm_tokens;
};

struct Table3Row {
    std::string ru_word;
    std::string en_word;
    std::string fr_word;
    std::string de_word;
    std::string es_word;
    std::vector<std::string> ru_forms;
    std::vector<std::string> en_phrases;
    std::vector<std::string> en_forms;
    std::vector<std::string> other_forms;
    bool is_single_ru = true;
};

struct MergedRow {
    std::string main_word;
    std::string prev_main_word;
    std::string ru;
    std::string en;
    std::string fr;
    std::string de;
    std::string es;
    std::string lat;
    std::string gr;
    std::string sphere;
    double p_t1_t3 = 0.0;
    double p_t1_t2 = 0.0;
    double p_en = 0.0, p_fr = 0.0, p_de = 0.0, p_es = 0.0;
    double p_mean = 0.0;

    // Внутренние результаты сравнения всех возможных случаев (не выводятся в базовую таблицу)
    double arm_sim[7] = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0 };
    double arm_sim_max = 0.0;
    double arm_root_sim_max = 0.0;
    double arm_surf_sim_max = 0.0;
    double five_lang_sim_mean = -1.0; // Среднее попарное сходство Ru-En-Fr-De-Es
    double five_lang_sim_max = -1.0;  // Максимальное попарное сходство внутри Ru-En-Fr-De-Es
    bool has_greco_latin_etymon = false;
    bool is_greco_latin_loan_in_lang[6] = { false, false, false, false, false, false }; // Arm, Ru, En, Fr, De, Es
    bool is_independent_armenian = false;
    bool five_langs_coincide = false;
    bool is_general_multisphere = false;
    bool is_strict_ling_plus_other = false;
    bool has_hnts_exact = false;
    bool has_hnts_with_ocr = false;
};

std::string strip_bom_and_cr(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s = s.substr(3);
    }
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<std::string> parse_csv_line(const std::string& raw_line, char delim) {
    std::string line = strip_bom_and_cr(raw_line);
    std::vector<std::string> cols;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            }
            else {
                in_quotes = !in_quotes;
            }
        }
        else if (c == delim && !in_quotes) {
            cols.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur += c;
        }
    }
    cols.push_back(trim(cur));
    return cols;
}

std::string csv_escape(const std::string& s, char delim = ';') {
    bool need_quotes = (s.find(delim) != std::string::npos ||
        s.find('"') != std::string::npos ||
        s.find('\n') != std::string::npos);
    if (!need_quotes) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::ifstream open_table_file(const std::wstring& filename) {
    std::vector<fs::path> candidates = {
        fs::path(filename),
        fs::path(L"Verluchutyun") / filename,
        fs::path(L"C:\\Users\\sokra\\source\\repos\\Verluchutyun\\Verluchutyun") / filename
    };
    for (const auto& p : candidates) {
        std::ifstream f(p, std::ios::binary);
        if (f.is_open()) return f;
    }
    return std::ifstream();
}

bool is_ru_vowel(unsigned char b1, unsigned char b2) {
    if (b1 == 0xD0) {
        return b2 == 0x90 || b2 == 0x95 || b2 == 0x98 || b2 == 0x9E || b2 == 0xA3 ||
            b2 == 0xAB || b2 == 0xAD || b2 == 0xAE || b2 == 0xAF || b2 == 0x81 ||
            b2 == 0xB0 || b2 == 0xB5 || b2 == 0xB8 || b2 == 0xBE;
    }
    if (b1 == 0xD1) {
        return b2 == 0x83 || b2 == 0x8B || b2 == 0x8D || b2 == 0x8E || b2 == 0x8F || b2 == 0x91;
    }
    return false;
}

std::string clean_russian_headword(const std::string& raw) {
    std::string s = trim(strip_bom_and_cr(raw));
    if (s.size() > 5 && (static_cast<unsigned char>(s[0]) == 0xD0 || static_cast<unsigned char>(s[0]) == 0xD1) && s[2] == ' ') {
        s = trim(s.substr(3));
    }
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        unsigned char b1 = static_cast<unsigned char>(s[i]);
        unsigned char b2 = static_cast<unsigned char>(s[i + 1]);
        if (b1 == 0xD0 && b2 == 0x99) {
            bool after_vowel = (i >= 2) && is_ru_vowel(static_cast<unsigned char>(s[i - 2]),
                static_cast<unsigned char>(s[i - 1]));
            if (!after_vowel) s[i + 1] = static_cast<char>(0x98);
        }
        else if (b1 == 0xD0 && b2 == 0xB9) {
            bool after_vowel = (i >= 2) && is_ru_vowel(static_cast<unsigned char>(s[i - 2]),
                static_cast<unsigned char>(s[i - 1]));
            if (!after_vowel) s[i + 1] = static_cast<char>(0xB8);
        }
        if (b1 == 0xD0 || b1 == 0xD1) ++i;
    }
    if (s == "ливергенция") s = "дивергенция";
    else if (s == "лифтонг") s = "дифтонг";
    else if (s == "тельда") s = "тильда";
    else if (s == "паронимиа") s = "паронимия";
    else if (s == "длактилография") s = "дактилография";
    return s;
}

std::string clean_etymology_field(const std::string& raw) {
    std::string out;
    int bracket = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '[') { bracket++; continue; }
        if (c == ']') { if (bracket > 0) bracket--; continue; }
        if (bracket > 0) continue;
        if (c == '~') continue;
        out += c;
    }
    replace_all(out, "casus)", "(casus)");
    return trim(out);
}

std::string transliterate_ru(const std::string& ru) {
    std::string out;
    for (size_t i = 0; i < ru.size(); ++i) {
        unsigned char b1 = static_cast<unsigned char>(ru[i]);
        if ((b1 == 0xD0 || b1 == 0xD1) && i + 1 < ru.size()) {
            unsigned char b2 = static_cast<unsigned char>(ru[i + 1]);
            if (b1 == 0xD0 && b2 >= 0x90 && b2 <= 0x9F) b2 += 0x20;
            else if (b1 == 0xD0 && b2 >= 0xA0 && b2 <= 0xAF) { b1 = 0xD1; b2 -= 0x20; }
            else if (b1 == 0xD0 && b2 == 0x81) { b1 = 0xD0; b2 = 0xB5; }
            else if (b1 == 0xD1 && b2 == 0x91) { b1 = 0xD0; b2 = 0xB5; }

            if (b1 == 0xD0) {
                switch (b2) {
                case 0xB0: out += "a"; break;  case 0xB1: out += "b"; break;
                case 0xB2: out += "v"; break;  case 0xB3: out += "g"; break;
                case 0xB4: out += "d"; break;  case 0xB5: out += "e"; break;
                case 0xB6: out += "zh"; break; case 0xB7: out += "z"; break;
                case 0xB8: out += "i"; break;  case 0xB9: out += "i"; break;
                case 0xBA: out += "k"; break;  case 0xBB: out += "l"; break;
                case 0xBC: out += "m"; break;  case 0xBD: out += "n"; break;
                case 0xBE: out += "o"; break;  case 0xBF: out += "p"; break;
                }
            }
            else if (b1 == 0xD1) {
                switch (b2) {
                case 0x80: out += "r"; break;  case 0x81: out += "s"; break;
                case 0x82: out += "t"; break;  case 0x83: out += "u"; break;
                case 0x84: out += "f"; break;  case 0x85: out += "kh"; break;
                case 0x86: out += "ts"; break; case 0x87: out += "tc"; break;
                case 0x88: out += "sh"; break; case 0x89: out += "sh"; break;
                case 0x8B: out += "y"; break;  case 0x8D: out += "e"; break;
                case 0x8E: out += "yu"; break; case 0x8F: out += "ya"; break;
                }
            }
            ++i;
        }
        else if (b1 >= 'A' && b1 <= 'Z') {
            out += static_cast<char>(b1 + 32);
        }
        else if (b1 >= 'a' && b1 <= 'z') {
            out += static_cast<char>(b1);
        }
    }
    return out;
}

std::string transliterate_arm(const std::string& arm) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < arm.size(); ++i) {
        unsigned char b1 = static_cast<unsigned char>(arm[i]);
        if ((b1 >= 0xD4 && b1 <= 0xD6) && i + 1 < arm.size()) {
            unsigned char b2 = static_cast<unsigned char>(arm[i + 1]);
            uint32_t cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
            if (cp >= 0x0531 && cp <= 0x0556) cp += 0x30;
            if (cp >= 0x0561 && cp <= 0x0587) cps.push_back(cp);
            ++i;
        }
    }
    std::string out;
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = cps[i];
        if (cp == 0x0578 && i + 1 < cps.size() && cps[i + 1] == 0x0582) {
            out += "u";
            ++i;
            continue;
        }
        switch (cp) {
        case 0x0561: out += "a"; break;  case 0x0562: out += "b"; break;
        case 0x0563: out += "g"; break;  case 0x0564: out += "d"; break;
        case 0x0565: out += "e"; break;  case 0x0566: out += "z"; break;
        case 0x0567: out += "e"; break;  case 0x0568: out += "e"; break;
        case 0x0569: out += "t"; break;  case 0x056A: out += "zh"; break;
        case 0x056B: out += "i"; break;  case 0x056C: out += "l"; break;
        case 0x056D: out += "kh"; break; case 0x056E: out += "ts"; break;
        case 0x056F: out += "k"; break;  case 0x0570: out += "h"; break;
        case 0x0571: out += "dz"; break; case 0x0572: out += "gh"; break;
        case 0x0573: out += "tc"; break; case 0x0574: out += "m"; break;
        case 0x0575: out += "y"; break;  case 0x0576: out += "n"; break;
        case 0x0577: out += "sh"; break; case 0x0578: out += "o"; break;
        case 0x0579: out += "tc"; break; case 0x057A: out += "p"; break;
        case 0x057B: out += "j"; break;  case 0x057C: out += "r"; break;
        case 0x057D: out += "s"; break;  case 0x057E: out += "v"; break;
        case 0x057F: out += "t"; break;  case 0x0580: out += "r"; break;
        case 0x0581: out += "ts"; break; case 0x0582: out += "u"; break;
        case 0x0583: out += "p"; break;  case 0x0584: out += "k"; break;
        case 0x0585: out += "o"; break;  case 0x0586: out += "f"; break;
        case 0x0587: out += "ev"; break;
        }
    }
    return out;
}

std::string arm_to_lower(const std::string& arm) {
    std::string out;
    for (size_t i = 0; i < arm.size(); ++i) {
        unsigned char b1 = static_cast<unsigned char>(arm[i]);
        if ((b1 >= 0xD4 && b1 <= 0xD6) && i + 1 < arm.size()) {
            unsigned char b2 = static_cast<unsigned char>(arm[i + 1]);
            uint32_t cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
            if (cp >= 0x0531 && cp <= 0x0556) cp += 0x30;
            if (cp >= 0x0561 && cp <= 0x0587) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            ++i;
        }
    }
    return out;
}

std::string clean_latin_word(const std::string& s) {
    std::string first = s.substr(0, s.find(','));
    std::string out;
    int paren = 0;
    for (size_t i = 0; i < first.size(); ++i) {
        char c = first[i];
        if (c == '(' || c == '[') { paren++; continue; }
        if (c == ')' || c == ']') { if (paren > 0) paren--; continue; }
        if (paren > 0) continue;

        unsigned char b1 = static_cast<unsigned char>(c);
        if (b1 == 0xC3 && i + 1 < first.size()) {
            unsigned char b2 = static_cast<unsigned char>(first[i + 1]);
            if ((b2 >= 0x80 && b2 <= 0x85) || (b2 >= 0xA0 && b2 <= 0xA5)) out += 'a';
            else if ((b2 >= 0x88 && b2 <= 0x8B) || (b2 >= 0xA8 && b2 <= 0xAB)) out += 'e';
            else if ((b2 >= 0x8C && b2 <= 0x8F) || (b2 >= 0xAC && b2 <= 0xAF)) out += 'i';
            else if ((b2 >= 0x92 && b2 <= 0x96) || (b2 >= 0xB2 && b2 <= 0xB6)) out += 'o';
            else if ((b2 >= 0x99 && b2 <= 0x9C) || (b2 >= 0xB9 && b2 <= 0xBC)) out += 'u';
            else if (b2 == 0x91 || b2 == 0xB1) out += 'n';
            else if (b2 == 0x87 || b2 == 0xA7) out += 'c';
            ++i;
            continue;
        }
        if (c >= 'A' && c <= 'Z') out += static_cast<char>(c + 32);
        else if (c >= 'a' && c <= 'z') out += c;
    }
    return out;
}

bool remove_suffix(std::string& w, const std::vector<std::string>& suffixes, size_t min_root = 3) {
    for (const auto& suf : suffixes) {
        if (w.size() >= suf.size() + min_root && w.compare(w.size() - suf.size(), suf.size(), suf) == 0) {
            w.erase(w.size() - suf.size());
            return true;
        }
    }
    return false;
}

std::string pre_normalize_orthography(std::string w, bool strip_initial_h = false) {
    if (w.empty()) return w;
    replace_all(w, "ph", "f");
    replace_all(w, "th", "t");
    replace_all(w, "rh", "r");
    replace_all(w, "sch", "sh");
    replace_all(w, "ch", "kh");
    replace_all(w, "ae", "e");
    replace_all(w, "oe", "e");
    replace_all(w, "qu", "k");
    replace_all(w, "x", "ks");
    replace_all(w, "shch", "sh");
    if (strip_initial_h && w.size() > 3 && (w[0] == 'h' || w[0] == 'g')) {
        if (w[0] == 'h' || w.rfind("gebr", 0) == 0 || w.rfind("gip", 0) == 0 || w.rfind("gidr", 0) == 0) {
            w.erase(0, 1);
        }
    }
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == 'c' && (i == 0 || w[i - 1] != 't')) {
            char next = (i + 1 < w.size()) ? w[i + 1] : '\0';
            w[i] = (next == 'e' || next == 'i' || next == 'y') ? 's' : 'k';
        }
    }
    return w;
}

std::string post_normalize_phonetics(std::string w) {
    replace_all(w, "tsa", "ca");
    replace_all(w, "ts", "s");
    replace_all(w, "dz", "z");
    replace_all(w, "gh", "g");
    replace_all(w, "kh", "k");
    replace_all(w, "yu", "u");
    replace_all(w, "ya", "a");
    replace_all(w, "yo", "o");
    replace_all(w, "yi", "i");
    replace_all(w, "ju", "u");
    replace_all(w, "ja", "a");
    replace_all(w, "je", "e");
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == 'y' || w[i] == 'j') w[i] = 'i';
        else if (w[i] == 'w') w[i] = 'v';
        else if (w[i] == 'z') w[i] = 's';
    }
    std::string dedup;
    for (char c : w) {
        if (!dedup.empty() && dedup.back() == c) continue;
        dedup += c;
    }
    return dedup;
}

std::string phonetic_normalize(const std::string& w, bool strip_initial_h = false) {
    return post_normalize_phonetics(pre_normalize_orthography(w, strip_initial_h));
}

std::string extract_stem_from_latin(std::string w, bool strip_h = false) {
    w = pre_normalize_orthography(w, strip_h);
    if (w.rfind("esp", 0) == 0 || w.rfind("est", 0) == 0) {
        if (w.size() >= 6) w.erase(0, 1);
    }
    if (w == "termination" || w == "informal") return post_normalize_phonetics(w);

    static const std::vector<std::string> suffixes = {
        "abanutyunner", "abanutyun", "abanutyan", "abanakan", "abakan",
        "agitutyun", "agitutyan", "agrutyun", "agrutyan",
        "akanutyun", "akanutyan", "oghutyun", "oghutyan",
        "utyunner", "utyun", "utyan", "utamb",
        "aynacum", "atsum", "acum", "ayin", "akan", "avor",
        "uyt", "uyts", "aran",
        "ikheskii", "ikheskaya", "ikheskoe", "ikheskie", "tikheskii",
        "icheskii", "icheskaya", "icheskoe", "icheskie", "ticheskii",
        "itceskii", "itceskaya", "itceskoe", "itceskie", "titceskii",
        "ovannyi", "iruyushii", "iruyushchii", "irovanie", "ovanie",
        "izatsiya", "ifikatsiya", "atsiya", "yatsiya", "itsiya", "entsiya", "antsiya", "tsiya",
        "izatsia", "ifikatsia", "atsia", "yatsia", "itsia", "entsia", "antsia", "tsia",
        "ization", "isation", "ifikation", "ification", "ation", "ition", "tion", "sion", "cion", "ion",
        "ologiya", "ologia", "ology", "ologie", "logiya", "logia",
        "ografiya", "ografia", "ography", "ographie", "grafiya", "grafia", "graph", "graf",
        "ense", "anse", "ensy", "ansy", "atura", "ature", "miento", "mente",
        "alnyi", "alnii", "ivnyi", "ovnyi", "yarnyi", "skii", "nyi", "nii",
        "ismus", "ismos", "isme", "ismo", "izm", "ism", "ist", "ika", "ik",
        "ikal", "ike", "iskh", "iko", "ical", "ique", "isch", "lich", "ico", "ica", "ic",
        "ung", "keit", "heit", "schaft", "ieren",
        "al", "ive", "iv", "if", "ous", "ing", "ed", "ity", "ty", "enie", "anie",
        "ikos", "ikon", "sis", "ma", "tos", "tes", "os", "on",
        "ner", "er", "um", "us", "is", "ia", "ii", "yi", "oi", "aya", "oe", "ie", "iya", "ya",
        "e", "a", "o", "i", "y", "s"
    };
    remove_suffix(w, suffixes, 3);
    return post_normalize_phonetics(w);
}

double levenshtein_similarity(const std::string& s1, const std::string& s2) {
    size_t n = s1.size(), m = s2.size();
    if (n == 0 || m == 0) return 0.0;
    if (s1 == s2) return 1.0;
    static thread_local std::vector<size_t> dp;
    dp.resize(m + 1);
    for (size_t j = 0; j <= m; ++j) dp[j] = j;

    for (size_t i = 1; i <= n; ++i) {
        size_t prev = dp[0];
        dp[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t temp = dp[j];
            size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            dp[j] = std::min(std::min(dp[j] + 1, dp[j - 1] + 1), prev + cost);
            prev = temp;
        }
    }
    return 1.0 - static_cast<double>(dp[m]) / static_cast<double>(std::max(n, m));
}

struct NormalizedForms {
    std::string pa, pah, sa, sah;
};

const NormalizedForms& get_normalized_forms(const std::string& w) {
    static std::unordered_map<std::string, NormalizedForms> cache;
    auto it = cache.find(w);
    if (it != cache.end()) return it->second;
    NormalizedForms nf;
    nf.pa = phonetic_normalize(w, false);
    nf.pah = phonetic_normalize(w, true);
    nf.sa = extract_stem_from_latin(w, false);
    nf.sah = extract_stem_from_latin(w, true);
    return cache.emplace(w, std::move(nf)).first->second;
}

double compare_surface_only(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    const NormalizedForms& na = get_normalized_forms(a);
    const NormalizedForms& nb = get_normalized_forms(b);
    return std::max(levenshtein_similarity(na.pa, nb.pa),
        levenshtein_similarity(na.pah, nb.pah) * 0.98);
}

double compare_root_only(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    const NormalizedForms& na = get_normalized_forms(a);
    const NormalizedForms& nb = get_normalized_forms(b);
    return std::max(levenshtein_similarity(na.sa, nb.sa),
        levenshtein_similarity(na.sah, nb.sah) * 0.98);
}

double compare_latin_forms(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    const NormalizedForms& na = get_normalized_forms(a);
    const NormalizedForms& nb = get_normalized_forms(b);
    double p_surf = std::max(levenshtein_similarity(na.pa, nb.pa),
        levenshtein_similarity(na.pah, nb.pah) * 0.98);
    double p_stem = std::max(levenshtein_similarity(na.sa, nb.sa),
        levenshtein_similarity(na.sah, nb.sah) * 0.98);

    if ((na.sa == nb.sa && (na.sa.size() >= 5 || (na.sa.size() >= 4 && p_surf >= 0.55))) ||
        (na.sah == nb.sah && (na.sah.size() >= 5 || (na.sah.size() >= 4 && p_surf >= 0.55)))) {
        return 0.85 + 0.15 * p_surf;
    }
    return 0.45 * p_surf + 0.55 * p_stem;
}

double compare_latin_forms_fast(const std::string& a, const std::string& b, double min_needed) {
    if (a.empty() || b.empty()) return 0.0;
    const NormalizedForms& na = get_normalized_forms(a);
    const NormalizedForms& nb = get_normalized_forms(b);
    if (!(na.sa == nb.sa || na.sah == nb.sah)) {
        auto len_ratio = [](const std::string& x, const std::string& y) -> double {
            if (x.empty() || y.empty()) return 0.0;
            return static_cast<double>(std::min(x.size(), y.size())) / static_cast<double>(std::max(x.size(), y.size()));
            };
        double ub_surf = std::max(len_ratio(na.pa, nb.pa), len_ratio(na.pah, nb.pah) * 0.98);
        double ub_stem = std::max(len_ratio(na.sa, nb.sa), len_ratio(na.sah, nb.sah) * 0.98);
        if (0.45 * ub_surf + 0.55 * ub_stem < min_needed) return 0.0;
    }
    return compare_latin_forms(a, b);
}

double compute_similarity_probability(const std::string& ru_word, const std::string& foreign_word,
    const std::string& fallback_arm = "", const std::string& fallback_en = "",
    const std::string& fallback_lat = "") {
    std::vector<std::string> base_lats;
    std::stringstream ss_ru(ru_word);
    std::string ru_part;
    while (std::getline(ss_ru, ru_part, ',')) {
        std::string tr = transliterate_ru(trim(ru_part));
        if (!tr.empty()) base_lats.push_back(tr);
    }
    if (!fallback_arm.empty()) {
        std::string tr_arm = transliterate_arm(fallback_arm.substr(0, fallback_arm.find(',')));
        if (!tr_arm.empty()) base_lats.push_back(tr_arm);
    }
    if (!fallback_en.empty()) {
        std::stringstream ss_en(fallback_en);
        std::string en_part;
        while (std::getline(ss_en, en_part, ',')) {
            std::string cl = clean_latin_word(en_part);
            if (!cl.empty()) base_lats.push_back(cl);
        }
    }
    if (!fallback_lat.empty()) {
        std::string cl_lat = clean_latin_word(fallback_lat);
        if (!cl_lat.empty()) base_lats.push_back(cl_lat);
    }
    if (base_lats.empty()) return 0.0;

    double best = 0.0;
    std::stringstream ss_fw(foreign_word);
    std::string fw_part;
    while (std::getline(ss_fw, fw_part, ',')) {
        std::string fw_lat = clean_latin_word(fw_part);
        if (fw_lat.empty()) continue;
        for (const auto& b_lat : base_lats) {
            double sim = compare_latin_forms(b_lat, fw_lat);
            if (sim > best) best = sim;
        }
    }
    return best;
}

bool is_generic_classifier(const std::string& w) {
    static const std::set<std::string> generic_words = {
        "case", "consonant", "consonants", "tense", "mood", "sound", "sounds",
        "word", "words", "aspect", "clause", "verb", "noun", "number", "voice",
        "accent", "form", "forms", "marker", "markers", "element", "principle",
        "process", "system", "style", "analysis", "attraction", "vowel", "vowels",
        "syllable", "sentence", "speech", "opposition", "numeral", "pronoun",
        "participle", "conjunction", "expressions", "expression", "evolution", "line"
    };
    return generic_words.count(w) > 0;
}

std::vector<std::string> extract_ascii_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cur += static_cast<char>(c | 0x20);
        }
        else if (c == '-' && !cur.empty()) {
            while (i + 1 < s.size() && s[i + 1] == ' ') ++i;
        }
        else {
            if (cur.size() >= 2) words.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 2) words.push_back(cur);
    return words;
}

std::string join_words(const std::vector<std::string>& ws) {
    std::string out;
    for (size_t i = 0; i < ws.size(); ++i) {
        if (i > 0) out += ' ';
        out += ws[i];
    }
    return out;
}

bool is_arm_upper_at(const std::string& s, size_t i) {
    if (i + 1 >= s.size()) return false;
    unsigned char b1 = static_cast<unsigned char>(s[i]);
    unsigned char b2 = static_cast<unsigned char>(s[i + 1]);
    if (b1 < 0xD4 || b1 > 0xD5) return false;
    uint32_t cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
    return (cp >= 0x0531 && cp <= 0x0556);
}

bool is_arm_lower_at(const std::string& s, size_t i) {
    if (i + 1 >= s.size()) return false;
    unsigned char b1 = static_cast<unsigned char>(s[i]);
    unsigned char b2 = static_cast<unsigned char>(s[i + 1]);
    if (b1 < 0xD5 || b1 > 0xD6) return false;
    uint32_t cp = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
    return (cp >= 0x0561 && cp <= 0x0587);
}

bool is_arm_letter_at(const std::string& s, size_t i) {
    return is_arm_upper_at(s, i) || is_arm_lower_at(s, i);
}

bool is_lowercase_main_word(const std::string& s) {
    int upper = 0, lower = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (is_arm_upper_at(s, i)) { upper++; ++i; }
        else if (is_arm_lower_at(s, i)) { lower++; ++i; }
    }
    return lower > 0 && upper == 0;
}

std::string strip_armenian_gloss_clause(std::string s) {
    static const std::vector<std::string> gloss_markers = {
        " ոճական հնարք", " իմաստային դեր", " որի դեպքում", " որը ցույց",
        " որոնցում ", " իրար հակիմաստ", " հնդեվրոպական լեզուներ"
    };
    for (const auto& m : gloss_markers) {
        size_t pos = s.find(m);
        if (pos != std::string::npos && pos >= 6) {
            s = trim(s.substr(0, pos));
            while (!s.empty() && (s.back() == ',' || s.back() == ';')) s.pop_back();
        }
    }
    return trim(s);
}

std::string extract_clean_armenian_equiv(const std::string& raw) {
    std::vector<std::string> arm_parts;
    std::stringstream ss(raw);
    std::string seg;
    while (std::getline(ss, seg, ',')) {
        std::string cur_seg;
        for (size_t i = 0; i < seg.size(); ++i) {
            unsigned char b1 = static_cast<unsigned char>(seg[i]);
            if ((b1 >= 0xD4 && b1 <= 0xD6) && i + 1 < seg.size()) {
                cur_seg += seg[i];
                cur_seg += seg[i + 1];
                ++i;
            }
            else if ((seg[i] == ' ' || seg[i] == '-') && !cur_seg.empty() && cur_seg.back() != ' ') {
                cur_seg += seg[i];
            }
        }
        std::stringstream ws_ss(trim(cur_seg));
        std::string tok;
        std::vector<std::string> clean_tokens;
        while (ws_ss >> tok) {
            if (tok == "Ճձևահնչույթային") tok = "ձևահնչույթային";
            else if (tok == "Մեծ" || tok == "Էմիկ" || tok == "Ժամանակ" || tok == "Հռուծ") tok = arm_to_lower(tok);

            int u_cnt = 0, l_cnt = 0;
            for (size_t k = 0; k + 1 < tok.size(); ++k) {
                if (is_arm_upper_at(tok, k)) { u_cnt++; ++k; }
                else if (is_arm_lower_at(tok, k)) { l_cnt++; ++k; }
            }
            if (u_cnt > 0 || l_cnt == 0) continue;
            if (l_cnt == 1 && tok != "և" && tok != "ի") continue;
            if (tok == "աա" || tok == "րր" || tok == "քր") continue;
            clean_tokens.push_back(tok);
        }
        while (!clean_tokens.empty() && (clean_tokens.back() == "և" || clean_tokens.back() == "ի")) {
            clean_tokens.pop_back();
        }
        std::string rebuilt = join_words(clean_tokens);
        if (!rebuilt.empty()) {
            arm_parts.push_back(rebuilt);
        }
    }
    std::string out;
    for (size_t i = 0; i < std::min<size_t>(3, arm_parts.size()); ++i) {
        if (i > 0) out += ", ";
        out += arm_parts[i];
    }
    std::string res = out.empty() ? arm_to_lower(raw) : out;
    return strip_armenian_gloss_clause(res);
}

std::string load_pdf_dictionary_text() {
    std::ifstream pdf_file = open_table_file(L"\u0532\u0561\u057C\u0565\u0580\u056B \u0561\u0572\u0575\u0578\u0582\u057D\u0561\u056F 1-\u056B \u0562\u0561\u057C\u0561\u0580\u0561\u0576.pdf");
    if (!pdf_file.is_open()) return "";

    pdf_file.seekg(0, std::ios::end);
    std::streamsize sz = pdf_file.tellg();
    pdf_file.seekg(0, std::ios::beg);
    if (sz <= 0) return "";

    std::vector<char> buf(static_cast<size_t>(sz));
    pdf_file.read(buf.data(), sz);

    poppler::document* doc = poppler::document::load_from_raw_data(buf.data(), static_cast<int>(buf.size()));
    if (!doc) return "";

    std::vector<std::string> lines;
    int num_pages = doc->pages();
    for (int p_idx = 0; p_idx < num_pages; ++p_idx) {
        poppler::page* p = doc->create_page(p_idx);
        if (!p) continue;
        poppler::byte_array utf8_bytes = p->text(p->page_rect(), poppler::page::raw_order_layout).to_utf8();
        delete p;

        std::string page_str(utf8_bytes.begin(), utf8_bytes.end());
        std::stringstream ss(page_str);
        std::string l;
        std::vector<std::string> page_lines;
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty() && l.back() == '\f') l.pop_back();
            page_lines.push_back(l);
        }
        if (!page_lines.empty()) {
            const std::string& last_l = page_lines.back();
            bool only_digits = !last_l.empty();
            for (char c : last_l) {
                if ((c < '0' || c > '9') && c != ' ') { only_digits = false; break; }
            }
            if (only_digits) page_lines.pop_back();
        }
        for (const auto& pl : page_lines) lines.push_back(pl);
    }
    delete doc;

    std::string full_text;
    full_text.reserve(1500000);
    for (const auto& l : lines) {
        if (!l.empty() && l.back() == '-' && (l.size() < 2 || l[l.size() - 2] != ' ')) {
            full_text += l.substr(0, l.size() - 1);
        }
        else {
            full_text += l + "\n";
        }
    }
    return full_text;
}

bool is_pdf_headword_line(const std::string& s, size_t line_start) {
    size_t i = line_start;
    int upper_count = 0;
    while (i < s.size()) {
        if (is_arm_upper_at(s, i)) {
            upper_count++;
            i += 2;
        }
        else if (s[i] == '(' || s[i] == ')' || s[i] == '.') {
            i++;
        }
        else {
            break;
        }
    }
    if (upper_count < 2) return false;
    if (i < s.size() && is_arm_lower_at(s, i)) return false;
    while (i < s.size() && s[i] == ' ') i++;
    if (i < s.size() && (s[i] == '[' || s[i] == '|' || s[i] == ',' || s[i] == '-' || s[i] == 'I' || is_arm_upper_at(s, i))) {
        return true;
    }
    return false;
}

size_t find_standalone_keyword_in_entry(const std::string& entry, const std::string& kw) {
    size_t pos = 0;
    while ((pos = entry.find(kw, pos)) != std::string::npos) {
        bool ok_before = true;
        if (pos >= 2 && is_arm_letter_at(entry, pos - 2)) {
            if (!(kw == "եզվբ" && entry.compare(pos - 2, 2, "ղ") == 0)) {
                ok_before = false;
            }
        }
        size_t after = pos + kw.size();
        bool ok_after = (after >= entry.size() || !is_arm_letter_at(entry, after));
        if (ok_before && ok_after) return pos;
        pos += kw.size();
    }
    return std::string::npos;
}

std::string extract_word_after_keyword(const std::string& entry, size_t kw_pos, size_t kw_len) {
    size_t i = kw_pos + kw_len;
    size_t close_paren = entry.find(')', i);
    if (close_paren != std::string::npos && close_paren - i <= 30) {
        i = close_paren + 1;
    }
    else {
        bool skipped = true;
        while (skipped && i < entry.size()) {
            skipped = false;
            while (i < entry.size() && (entry[i] == '.' || entry[i] == ',' || entry[i] == ':' || entry[i] == ' ' || entry[i] == ')')) i++;
            for (const std::string& tag : { "հնց", "ոնց", "hfg", "hGg", "քեր", "գրկն", "Գրկն", "գըկն", "տեխ", "տպգը", "լեզվբ", "եզվբ" }) {
                if (entry.compare(i, tag.size(), tag) == 0 && (i + tag.size() >= entry.size() || !is_arm_letter_at(entry, i + tag.size()))) {
                    i += tag.size();
                    skipped = true;
                    break;
                }
            }
        }
    }

    while (i < entry.size()) {
        while (i < entry.size() && !is_arm_letter_at(entry, i)) {
            i++;
        }
        if (i >= entry.size()) break;

        std::string word;
        while (i < entry.size()) {
            if (is_arm_letter_at(entry, i)) {
                word += entry[i];
                word += entry[i + 1];
                i += 2;
            }
            else if (entry[i] == '-' && !word.empty() && i + 1 < entry.size() && is_arm_letter_at(entry, i + 1)) {
                word += '-';
                i++;
            }
            else {
                break;
            }
        }

        if (word.size() == 2 && i < entry.size() && entry[i] == '.') {
            i++;
            continue;
        }
        if (word == "լեզվբ" || word == "եզվբ" || word == "հնց" || word == "ոնց" || word == "քեր" ||
            word == "գրկն" || word == "Գրկն" || word == "գըկն" || word == "տեխ" || word == "տպգը" ||
            word == "ած" || word == "մկբ" || word == "շղկ") {
            continue;
        }
        if (!word.empty()) {
            if (word.size() >= 6 && is_arm_upper_at(word, 0) && is_arm_upper_at(word, 2) && is_arm_lower_at(word, 4)) {
                word = word.substr(2);
            }
            return word;
        }
    }
    return "";
}

std::string lookup_main_word_in_pdf_dictionary(const std::string& pdf_text, const std::string& upper_headword) {
    if (pdf_text.empty() || upper_headword.empty()) return upper_headword;

    size_t pos = 0, match_pos = std::string::npos;
    while ((pos = pdf_text.find(upper_headword, pos)) != std::string::npos) {
        bool at_line_start = (pos == 0 || pdf_text[pos - 1] == '\n');
        size_t after = pos + upper_headword.size();
        bool valid_after = (after >= pdf_text.size() || !is_arm_letter_at(pdf_text, after));
        if (at_line_start && valid_after) {
            match_pos = pos;
            break;
        }
        pos += upper_headword.size();
    }
    if (match_pos == std::string::npos) {
        pos = 0;
        while ((pos = pdf_text.find(upper_headword, pos)) != std::string::npos) {
            bool valid_before = (pos < 2 || !is_arm_letter_at(pdf_text, pos - 2));
            size_t after = pos + upper_headword.size();
            bool valid_after = (after >= pdf_text.size() || !is_arm_letter_at(pdf_text, after));
            if (valid_before && valid_after) {
                match_pos = pos;
                break;
            }
            pos += upper_headword.size();
        }
    }
    if (match_pos == std::string::npos) return upper_headword;

    size_t cur = match_pos + upper_headword.size();
    size_t end_pos = std::min(pdf_text.size(), match_pos + 2000);
    while (cur < end_pos) {
        size_t nl = pdf_text.find('\n', cur);
        if (nl == std::string::npos || nl + 1 >= end_pos) break;
        size_t next_line = nl + 1;
        if (is_pdf_headword_line(pdf_text, next_line)) {
            end_pos = next_line;
            break;
        }
        cur = next_line;
    }

    std::string entry = pdf_text.substr(match_pos, end_pos - match_pos);
    for (char& c : entry) {
        if (c == '\n') c = ' ';
    }

    size_t kw_pos = find_standalone_keyword_in_entry(entry, "լեզվբ");
    size_t kw_len = 10;
    if (kw_pos == std::string::npos) {
        kw_pos = find_standalone_keyword_in_entry(entry, "եզվբ");
        kw_len = 8;
    }

    if (kw_pos == std::string::npos) {
        size_t p_ker = find_standalone_keyword_in_entry(entry, "քեր");
        size_t p_hnts = find_standalone_keyword_in_entry(entry, "հնց");
        if (p_hnts == std::string::npos) p_hnts = find_standalone_keyword_in_entry(entry, "ոնց");
        if (p_ker != std::string::npos && (p_hnts == std::string::npos || p_ker <= p_hnts)) {
            kw_pos = p_ker;
            kw_len = 6;
        }
        else if (p_hnts != std::string::npos) {
            kw_pos = p_hnts;
            kw_len = 6;
        }
    }

    if (kw_pos != std::string::npos) {
        std::string extracted = extract_word_after_keyword(entry, kw_pos, kw_len);
        if (!extracted.empty()) return extracted;
    }
    return upper_headword;
}

uint32_t next_utf8_codepoint(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    ++i;
    for (size_t k = 0; k < extra && i < s.size(); ++k, ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    return cp;
}

bool fold_codepoint_to_latin(uint32_t cp, std::string& out) {
    if (cp >= 'A' && cp <= 'Z') { out += static_cast<char>(cp + 32); return true; }
    if (cp >= 'a' && cp <= 'z') { out += static_cast<char>(cp); return true; }

    if (cp >= 0xC0 && cp <= 0xFF) {
        if (cp == 0xD7 || cp == 0xF7) return false;
        static const char* l1[64] = {
            "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
            "d","n","o","o","o","o","o","","o","u","u","u","u","y","th","ss",
            "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
            "d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y" };
        out += l1[cp - 0xC0];
        return true;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp <= 0x105) out += "a"; else if (cp <= 0x10D) out += "c"; else if (cp <= 0x111) out += "d";
        else if (cp <= 0x11B) out += "e"; else if (cp <= 0x123) out += "g"; else if (cp <= 0x127) out += "h";
        else if (cp <= 0x131) out += "i"; else if (cp <= 0x133) out += "ij"; else if (cp <= 0x135) out += "j";
        else if (cp <= 0x138) out += "k"; else if (cp <= 0x142) out += "l"; else if (cp <= 0x14B) out += "n";
        else if (cp <= 0x151) out += "o"; else if (cp <= 0x153) out += "oe"; else if (cp <= 0x159) out += "r";
        else if (cp <= 0x161) out += "s"; else if (cp <= 0x167) out += "t"; else if (cp <= 0x173) out += "u";
        else if (cp <= 0x175) out += "w"; else if (cp <= 0x178) out += "y"; else out += "z";
        return true;
    }
    if (cp >= 0x180 && cp <= 0x24F) return true;
    if (cp >= 0x300 && cp <= 0x36F) return true;

    auto greek_base = [](uint32_t g) -> uint32_t {
        switch (g) {
        case 0x386: case 0x3AC: return 0x3B1; case 0x388: case 0x3AD: return 0x3B5;
        case 0x389: case 0x3AE: return 0x3B7; case 0x38A: case 0x3AF: case 0x3CA: case 0x390: return 0x3B9;
        case 0x38C: case 0x3CC: return 0x3BF; case 0x38E: case 0x3CD: case 0x3CB: case 0x3B0: return 0x3C5;
        case 0x38F: case 0x3CE: return 0x3C9;
        }
        if (g >= 0x391 && g <= 0x3A9) return g + 0x20;
        if (g >= 0x1F00 && g <= 0x1FFF) {
            uint32_t o = g - 0x1F00;
            if (o < 0x10) return 0x3B1; if (o < 0x20) return 0x3B5; if (o < 0x30) return 0x3B7;
            if (o < 0x40) return 0x3B9; if (o < 0x50) return 0x3BF; if (o < 0x60) return 0x3C5;
            if (o < 0x70) return 0x3C9;
            if (o < 0x72) return 0x3B1; if (o < 0x74) return 0x3B5; if (o < 0x76) return 0x3B7;
            if (o < 0x78) return 0x3B9; if (o < 0x7A) return 0x3BF; if (o < 0x7C) return 0x3C5;
            if (o < 0x7E) return 0x3C9;
            if (o < 0x90) return 0x3B1; if (o < 0xA0) return 0x3B7; if (o < 0xB0) return 0x3C9;
            if (o < 0xBD) return 0x3B1; if (o >= 0xC2 && o < 0xCD) return 0x3B7;
            if (o >= 0xD0 && o < 0xDC) return 0x3B9; if (o == 0xE4 || o == 0xE5) return 0x3C1;
            if (o >= 0xE0 && o < 0xED) return 0x3C5; if (o >= 0xF2 && o < 0xFD) return 0x3C9;
            return 0;
        }
        return g;
        };
    if ((cp >= 0x386 && cp <= 0x3CE) || (cp >= 0x1F00 && cp <= 0x1FFF)) {
        uint32_t g = greek_base(cp);
        if (g == 0) return true;
        static const char* gr[25] = { "a","b","g","d","e","z","e","th","i","k","l","m","n","ks","o","p","r","s","s","t","y","ph","ch","ps","o" };
        if (g >= 0x3B1 && g <= 0x3C9) out += gr[g - 0x3B1];
        return true;
    }
    if (cp >= 0x400 && cp <= 0x4FF) {
        std::string ch;
        ch += static_cast<char>(0xC0 | (cp >> 6));
        ch += static_cast<char>(0x80 | (cp & 0x3F));
        out += transliterate_ru(ch);
        return true;
    }
    if (cp >= 0x531 && cp <= 0x587) {
        std::string ch;
        ch += static_cast<char>(0xC0 | (cp >> 6));
        ch += static_cast<char>(0x80 | (cp & 0x3F));
        out += transliterate_arm(ch);
        return true;
    }
    if (cp == 0x55B || cp == 0x55C || cp == 0x55E) return true;
    return false;
}

bool is_comparison_stopword(const std::string& w) {
    static const std::set<std::string> stop = {
        "the", "and", "with", "from", "for", "des", "del", "los", "las", "der", "die", "das", "und", "von",
        "une", "pour", "avec", "sich", "eine", "einer", "les", "par", "sur", "attr", "aus", "dem", "den",
        "kam", "vor", "hamar", "mej", "het", "ayl", "ev", "nor", "ayd", "mi", "en", "vori", "vorpeszi"
    };
    return stop.count(w) > 0;
}

std::vector<std::string> normalized_armenian_tokens(const std::string& raw_text) {
    std::string text = strip_armenian_gloss_clause(raw_text);
    std::vector<std::string> out;
    std::string run;
    auto flush = [&]() {
        if (!run.empty()) {
            std::string tr = transliterate_arm(run);
            if (tr.size() >= 3 && !is_comparison_stopword(tr)) out.push_back(tr);
            run.clear();
        }
        };
    for (size_t i = 0; i < text.size();) {
        size_t start = i;
        uint32_t cp = next_utf8_codepoint(text, i);
        if (cp >= 0x531 && cp <= 0x587) {
            run.append(text, start, i - start);
        }
        else if (cp == 0x55B || cp == 0x55C || cp == 0x55E) {
            continue;
        }
        else {
            flush();
        }
    }
    flush();
    return out;
}

std::vector<std::string> normalized_foreign_tokens(const std::string& raw_text) {
    std::string text = raw_text;
    replace_all(text, "- ", ""); // склейка дефисных переносов вида morfo- logia, entona- ciôn
    replace_all(text, "-", "");  // склейка дефисных приставок вида pre-position -> preposition
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 3 && !is_comparison_stopword(cur)) out.push_back(cur);
        cur.clear();
        };
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_utf8_codepoint(text, i);
        if (!fold_codepoint_to_latin(cp, cur)) flush();
    }
    flush();
    return out;
}

double best_token_similarity(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return -1.0;
    double best = 0.0;
    for (const auto& x : a) {
        for (const auto& y : b) {
            double s = compare_latin_forms(x, y);
            if (s > best) best = s;
        }
    }
    return best;
}

// Сравнение всех возможных случаев для строки (выполняется внутри программы)
void evaluate_all_cases_for_row(MergedRow& r) {
    const std::string* fields[7] = { &r.ru, &r.en, &r.fr, &r.de, &r.es, &r.lat, &r.gr };
    std::vector<std::string> arm_tokens = normalized_armenian_tokens(r.main_word);
    std::vector<std::vector<std::string>> lang_tokens(7);
    for (int k = 0; k < 7; ++k) lang_tokens[k] = normalized_foreign_tokens(*fields[k]);

    r.arm_sim_max = 0.0;
    r.arm_root_sim_max = 0.0;
    r.arm_surf_sim_max = 0.0;
    for (int k = 0; k < 7; ++k) {
        r.arm_sim[k] = best_token_similarity(arm_tokens, lang_tokens[k]);
        if (r.arm_sim[k] > r.arm_sim_max) r.arm_sim_max = r.arm_sim[k];
        for (const auto& x : arm_tokens) {
            for (const auto& y : lang_tokens[k]) {
                double rs = compare_root_only(x, y);
                double ss = compare_surface_only(x, y);
                if (rs > r.arm_root_sim_max) r.arm_root_sim_max = rs;
                if (ss > r.arm_surf_sim_max) r.arm_surf_sim_max = ss;
            }
        }
    }

    // Требование 2: самостоятельный армянский эквивалент (не заимствование)
    r.is_independent_armenian = (!arm_tokens.empty() && r.arm_sim_max < 0.50);

    // Требование 3: все попарные комбинации среди 5 современных языков (Ru, En, Fr, De, Es)
    double sum5 = 0.0, max5 = 0.0;
    int cnt5 = 0;
    for (int a = 0; a < 5; ++a) {
        for (int b = a + 1; b < 5; ++b) {
            double s = best_token_similarity(lang_tokens[a], lang_tokens[b]);
            if (s >= 0.0) {
                sum5 += s;
                if (s > max5) max5 = s;
                cnt5++;
            }
        }
    }
    r.five_lang_sim_mean = cnt5 > 0 ? (sum5 / cnt5) : -1.0;
    r.five_lang_sim_max = cnt5 > 0 ? max5 : -1.0;
    r.five_langs_coincide = (r.five_lang_sim_mean >= 0.50 || r.five_lang_sim_max >= 0.70);

    // Требование 1: заимствования греческого или латинского происхождения в 6 языках (Arm, Ru, En, Fr, De, Es)
    r.has_greco_latin_etymon = (!r.lat.empty() || !r.gr.empty());
    if (r.has_greco_latin_etymon) {
        r.is_greco_latin_loan_in_lang[0] = !r.main_word.empty(); // в Табл. 1 заглавное арм. слово — заимствование
        for (int k = 0; k < 5; ++k) {
            r.is_greco_latin_loan_in_lang[k + 1] = !fields[k]->empty();
        }
    }

    // Требование 4: междисциплинарность (помимо լեզվբ. используется хотя бы еще в одной сфере)
    static const std::vector<std::string> other_domains = {
        "կենսբ", "մաթ", "գիտ", "գրկն", "Գրկն", "գըկն", "փիլ", "տրամբ",
        "իրավբ", "հրավբ", "բժշկ", "կզմխս", "քիմ", "երժշտ", "թատր", "պատմ",
        "երկրբ", "երկբշխ", "տնտսգ", "ֆին", "քղք", "ռազմ", "տեխ", "տպգը",
        "լսնկրչ", "նկարչ", "արվ", "սպորտ", "մսնգ", "մնծգը"
    };
    static const std::vector<std::string> ling_domains = { "լեզվբ", "եզվբ", "քեր" };

    bool has_other = false;
    for (const auto& d : other_domains) {
        if (r.sphere.find(d) != std::string::npos) { has_other = true; break; }
    }
    bool has_ling = false;
    for (const auto& d : ling_domains) {
        if (r.sphere.find(d) != std::string::npos) { has_ling = true; break; }
    }
    r.is_general_multisphere = has_other;
    r.is_strict_ling_plus_other = (has_ling && has_other);

    // Требование 5: помета հնց.
    r.has_hnts_exact = (r.sphere.find("հնց") != std::string::npos);
    r.has_hnts_with_ocr = r.has_hnts_exact ||
        (r.sphere.find("ոնց") != std::string::npos) ||
        (r.sphere.find("hfg") != std::string::npos) ||
        (r.sphere.find("hGg") != std::string::npos) ||
        (r.sphere.find("hfig") != std::string::npos);
}

std::string pct_cell(double v) {
    if (v < 0.0) return "";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (v * 100.0) << "%";
    std::string s = oss.str();
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::ifstream f1 = open_table_file(L"\u0532\u0561\u057C\u0565\u0580\u056B \u0561\u0572\u0575\u0578\u0582\u057D\u0561\u056F 1.csv");
    std::ifstream f2 = open_table_file(L"\u0532\u0561\u057C\u0565\u0580\u056B \u0561\u0572\u0575\u0578\u0582\u057D\u0561\u056F 2.csv");
    std::ifstream f3 = open_table_file(L"\u0540\u056B\u0574\u0561\u056F\u0561\u0576 \u0562\u0561\u057C\u0565\u0580 \u0561\u0572\u0575\u0578\u057D\u0561\u056F.csv");
    if (!f1.is_open() || !f2.is_open() || !f3.is_open()) {
        std::cerr << "Ошибка: не удалось открыть исходные таблицы 1, 2 или 3.\n";
        return 1;
    }

    std::string line;
    std::getline(f1, line);
    std::vector<Table1Row> t1_rows;
    while (std::getline(f1, line)) {
        auto cols = parse_csv_line(line, ';');
        if (cols.size() < 15) continue;
        Table1Row r;
        r.arm_word = cols[0];
        r.ru_word = clean_russian_headword(cols[1]);
        r.lat_word = clean_etymology_field(cols[2]);
        r.gr_word = clean_etymology_field(cols[3]);
        r.fr_word = clean_etymology_field(cols[4]);
        r.de_word = clean_etymology_field(cols[5]);
        r.en_word = clean_etymology_field(cols[6]);
        r.it_word = clean_etymology_field(cols[7]);
        r.es_word = clean_etymology_field(cols[8]);
        r.sphere = cols[14];

        if (r.sphere.find("լեզվբ") != std::string::npos || r.sphere.find("եզվբ") != std::string::npos ||
            r.sphere.find("քեր") != std::string::npos || r.sphere.find("գրկն") != std::string::npos ||
            r.sphere.find("գիտ") != std::string::npos || r.sphere.find("կզմխս") != std::string::npos ||
            r.sphere.find("տրամբ") != std::string::npos || r.sphere.find("փիլ") != std::string::npos) {
            r.has_ling_tag = true;
        }
        if (!r.ru_word.empty()) {
            std::stringstream ss(r.ru_word);
            std::string part;
            while (std::getline(ss, part, ',')) {
                part = trim(part);
                if (part.find(' ') == std::string::npos) {
                    std::string tr = transliterate_ru(part);
                    if (tr.size() >= 3) { r.ru_forms.push_back(tr); r.latin_forms.push_back(tr); }
                }
            }
        }
        for (const std::string& etym_col : { r.lat_word, r.gr_word, r.fr_word, r.de_word, r.en_word }) {
            if (etym_col.empty()) continue;
            for (const auto& w : extract_ascii_words(etym_col)) {
                if (w.size() >= 4 && w != "casus" && w != "from") {
                    r.lat_forms.push_back(w);
                    r.latin_forms.push_back(w);
                }
            }
        }
        std::string arm_first = r.arm_word.substr(0, r.arm_word.find(','));
        if (arm_first.find(' ') == std::string::npos) {
            std::string tr_arm = transliterate_arm(arm_first);
            if (tr_arm.size() >= 3) {
                r.ru_forms.push_back(tr_arm);
                r.latin_forms.push_back(tr_arm);
                if (tr_arm.find('s') != std::string::npos) {
                    std::string tr_m = tr_arm;
                    replace_all(tr_m, "s", "m");
                    r.ru_forms.push_back(tr_m);
                    r.latin_forms.push_back(tr_m);
                }
            }
        }
        if (r.arm_word == "ԼԵԶՎԱԲԱՆՈՒԹՅՈՒՆ") {
            r.ru_forms.push_back("lingvistika");
            r.latin_forms.push_back("lingvistika");
            r.ru_word = "лингвистика";
        }
        t1_rows.push_back(r);
    }

    std::getline(f2, line);
    std::vector<Table2Row> t2_rows;
    std::map<std::string, std::string> t2_lookup_equiv = {
        {"case", "հոլով"}, {"voice", "բայասեռ"}, {"mood", "բայի եղանակ"},
        {"subjunctive mood", "ստորադասական եղանակ"}, {"sound", "հնչյուն"},
        {"speech", "խոսք"}, {"style", "ոճ"}, {"strategy", "խոսքային ռազմավարություն"},
        {"ber", "թիվ"}, {"con", "բաղաձայն հնչյուն"}
    };
    while (std::getline(f2, line)) {
        auto cols = parse_csv_line(line, ';');
        if (cols.size() < 2 || cols[0] == "LL") continue;
        Table2Row r;
        r.en_raw = cols[0];
        r.arm_equiv = cols[1];

        std::string en_fixed = r.en_raw;
        const std::vector<std::pair<std::string, std::string>> ocr_fixes = {
            {"Perivation", "Derivation"}, {"Pialect", "Dialect"}, {"Pefective", "Defective"},
            {"Compa rative", "Comparative"}, {"Diach rony", "Diachrony"},
            {"De terminative", "Determinative"}, {"Phono og", "Phonology"},
            {"Lllocution", "Illocution"}, {"Jambic", "Iambic"}, {"Parogoge", "Paragoge"},
            {"Accomodation", "Accommodation"}, {"Quotatio mark", "Quotation mark"},
            {"Questi n marks", "Question marks"}
        };
        for (const auto& [bad, good] : ocr_fixes) {
            if (en_fixed.rfind(bad, 0) == 0) en_fixed = good + en_fixed.substr(bad.size());
        }

        size_t cut_pos = en_fixed.find_first_of("([/,;");
        std::string head_part = trim(cut_pos == std::string::npos ? en_fixed : en_fixed.substr(0, cut_pos));
        bool has_ipa_noise = (head_part.find_first_of("]:0123456789'\"!") != std::string::npos) ||
            (head_part.find("bra kiled") != std::string::npos) ||
            (head_part.find("kntemas") != std::string::npos) ||
            (head_part.find("nju:trks") != std::string::npos) ||
            (head_part.find("meta pl") != std::string::npos) ||
            (head_part.find("pare gaud") != std::string::npos) ||
            (head_part.find("okst mo") != std::string::npos) ||
            (head_part.find("peaeradaim") != std::string::npos) ||
            (head_part.find("Prolepsis pra") != std::string::npos) ||
            (head_part.find("Original ulj") != std::string::npos) ||
            (head_part.find("Paired conjunction ania") != std::string::npos) ||
            (head_part.find("Attraction Samana") != std::string::npos);

        auto words = extract_ascii_words(head_part);
        if (!words.empty()) {
            if (has_ipa_noise) {
                r.en_clean = words[0];
                r.en_phrases.push_back(words[0]);
                r.primary_en.push_back(words[0]);
            }
            else {
                std::string phrase = join_words(words);
                r.en_phrases.push_back(phrase);
                if (words.size() == 1) {
                    r.en_clean = words[0];
                    r.primary_en.push_back(words[0]);
                }
                else if (words.size() == 2 && is_generic_classifier(words[1])) {
                    r.en_clean = phrase;
                    r.primary_en.push_back(words[0]);
                }
                else {
                    r.en_clean = head_part;
                }
            }
        }
        else {
            r.en_clean = en_fixed;
        }

        std::stringstream ss(r.arm_equiv);
        std::string seg;
        while (std::getline(ss, seg, ',')) {
            seg = trim(seg);
            if (!seg.empty() && seg[0] >= 'A' && seg[0] <= 'Z' && seg.rfind("See ", 0) != 0) {
                auto sw = extract_ascii_words(seg);
                if (!sw.empty()) {
                    r.en_phrases.push_back(join_words(sw));
                    if (sw.size() == 1 && sw[0].size() >= 4) r.syn_en.push_back(sw[0]);
                    else if (sw.size() == 2 && is_generic_classifier(sw[1]) && sw[0].size() >= 4) r.syn_en.push_back(sw[0]);
                }
            }
        }
        if (r.arm_equiv.find("palatalization") != std::string::npos) {
            r.en_phrases.push_back("palatalization");
            r.primary_en.push_back("palatalization");
        }

        std::string cur_arm;
        for (size_t i = 0; i < r.arm_equiv.size(); ++i) {
            unsigned char b1 = static_cast<unsigned char>(r.arm_equiv[i]);
            if ((b1 >= 0xD4 && b1 <= 0xD6) && i + 1 < r.arm_equiv.size()) {
                cur_arm += r.arm_equiv[i];
                cur_arm += r.arm_equiv[i + 1];
                ++i;
            }
            else {
                if (!cur_arm.empty()) { r.arm_tokens.insert(arm_to_lower(cur_arm)); cur_arm.clear(); }
            }
        }
        if (!cur_arm.empty()) r.arm_tokens.insert(arm_to_lower(cur_arm));

        if (!r.en_phrases.empty() && r.arm_equiv.rfind("See ", 0) != 0) t2_lookup_equiv.emplace(r.en_phrases[0], r.arm_equiv);
        if (!r.primary_en.empty() && r.arm_equiv.rfind("See ", 0) != 0) t2_lookup_equiv.emplace(r.primary_en[0], r.arm_equiv);
        t2_rows.push_back(r);
    }

    for (auto& r : t2_rows) {
        if (r.arm_equiv.rfind("See ", 0) == 0) {
            auto ref_words = extract_ascii_words(r.arm_equiv.substr(4));
            std::string ref_phrase = join_words(ref_words);
            if (!ref_phrase.empty() && t2_lookup_equiv.count(ref_phrase)) r.arm_equiv = t2_lookup_equiv[ref_phrase] + " (" + r.arm_equiv + ")";
            else if (!ref_words.empty() && t2_lookup_equiv.count(ref_words[0])) r.arm_equiv = t2_lookup_equiv[ref_words[0]] + " (" + r.arm_equiv + ")";
        }
        r.arm_primary = extract_clean_armenian_equiv(r.arm_equiv);
    }

    std::getline(f3, line);
    std::vector<Table3Row> t3_rows;
    while (std::getline(f3, line)) {
        auto cols = parse_csv_line(line, ',');
        if (cols.size() < 5) continue;
        Table3Row r;
        r.ru_word = clean_russian_headword(cols[0]);
        r.en_word = cols[1];
        replace_all(r.en_word, " (attr", "");
        replace_all(r.en_word, " (aitг", "");
        r.fr_word = cols[2];
        r.de_word = cols[3];
        r.es_word = cols[4];
        if (r.ru_word.empty()) continue;

        if (r.ru_word.find(' ') == std::string::npos) {
            r.is_single_ru = true;
            std::string tr = transliterate_ru(r.ru_word);
            if (tr.size() >= 3) r.ru_forms.push_back(tr);
        }
        else {
            r.is_single_ru = false;
            std::stringstream ss(r.ru_word);
            std::string w;
            while (ss >> w) {
                std::string tr = transliterate_ru(w);
                if (tr.size() >= 4) r.ru_forms.push_back(tr);
            }
        }

        std::stringstream ss_en(r.en_word);
        std::string en_part;
        while (std::getline(ss_en, en_part, ',')) {
            std::string no_par;
            int p_depth = 0;
            for (char c : en_part) {
                if (c == '(' || c == '[') { p_depth++; continue; }
                if (c == ')' || c == ']') { if (p_depth > 0) p_depth--; continue; }
                if (p_depth == 0) no_par += c;
            }
            auto ws_raw = extract_ascii_words(no_par);
            std::vector<std::string> ws;
            for (const auto& w : ws_raw) {
                if (w != "attr" && w != "pl" && w != "sg" && w != "ucti" && w != "ucri" && w != "tien") ws.push_back(w);
            }
            if (!ws.empty()) {
                r.en_phrases.push_back(join_words(ws));
                if (ws.size() == 1 && ws[0].size() >= 3) r.en_forms.push_back(ws[0]);
                else if (ws.size() == 2 && is_generic_classifier(ws[1])) r.en_forms.push_back(ws[0]);
            }

            std::string merged_par;
            for (char c : en_part) {
                if (c != '(' && c != ')' && c != '[' && c != ']') merged_par += c;
            }
            auto ws_merged = extract_ascii_words(merged_par);
            if (ws_merged.size() == 1 && ws_merged[0].size() >= 4) {
                r.en_phrases.push_back(ws_merged[0]);
                r.en_forms.push_back(ws_merged[0]);
            }

            size_t lp = en_part.find('(');
            size_t rp = en_part.find(')', lp == std::string::npos ? 0 : lp);
            if (lp != std::string::npos && rp != std::string::npos && lp > 0 && en_part[lp - 1] == ' ') {
                std::string before = trim(en_part.substr(0, lp));
                std::string inside = trim(en_part.substr(lp + 1, rp - lp - 1));
                std::string after = trim(en_part.substr(rp + 1));
                auto ws_in = extract_ascii_words(inside);
                auto ws_af = extract_ascii_words(after);
                auto ws_bf = extract_ascii_words(before);
                if (!ws_in.empty() && ws_in[0] != "attr") {
                    if (ws_bf.size() == 1 && !ws_af.empty()) {
                        std::vector<std::string> alt_ws = ws_in;
                        alt_ws.insert(alt_ws.end(), ws_af.begin(), ws_af.end());
                        r.en_phrases.push_back(join_words(alt_ws));
                    }
                    if (ws_in.size() == 1 && ws_in[0].size() >= 4 && (ws_bf.size() + ws_af.size() <= 2)) {
                        r.en_forms.push_back(ws_in[0]);
                    }
                }
            }
        }
        t3_rows.push_back(r);
    }

    std::map<std::string, std::vector<size_t>> t1_ru_exact, t1_stem_idx;
    for (size_t i1 = 0; i1 < t1_rows.size(); ++i1) {
        for (const auto& rf : t1_rows[i1].ru_forms) {
            t1_ru_exact[rf].push_back(i1);
            std::string st = extract_stem_from_latin(rf);
            if (st.size() >= 5) t1_stem_idx[st].push_back(i1);
        }
        for (const auto& lf : t1_rows[i1].lat_forms) {
            std::string st = extract_stem_from_latin(lf);
            if (st.size() >= 5) t1_stem_idx[st].push_back(i1);
        }
    }

    std::map<std::string, std::vector<size_t>> t2_phrase_idx, t2_primary_idx, t2_syn_idx, t2_stem_idx;
    for (size_t i2 = 0; i2 < t2_rows.size(); ++i2) {
        if (!t2_rows[i2].en_phrases.empty()) t2_phrase_idx[t2_rows[i2].en_phrases[0]].push_back(i2);
        for (size_t k = 1; k < t2_rows[i2].en_phrases.size(); ++k) t2_syn_idx[t2_rows[i2].en_phrases[k]].push_back(i2);
        for (const auto& w : t2_rows[i2].primary_en) {
            t2_primary_idx[w].push_back(i2);
            if (w.size() >= 5 && w != "informal" && w != "termination") {
                std::string st = extract_stem_from_latin(w);
                if (st.size() >= 5) t2_stem_idx[st].push_back(i2);
            }
        }
        for (const auto& w : t2_rows[i2].syn_en) {
            t2_syn_idx[w].push_back(i2);
            if (w.size() >= 5 && w != "informal" && w != "termination") {
                std::string st = extract_stem_from_latin(w);
                if (st.size() >= 5) t2_stem_idx[st].push_back(i2);
            }
        }
    }

    std::vector<MergedRow> merged_rows;
    std::set<size_t> used_t1;

    for (size_t i3 = 0; i3 < t3_rows.size(); ++i3) {
        const auto& r3 = t3_rows[i3];
        const Table1Row* best_t1 = nullptr;
        size_t best_i1 = 0;
        double best_s13 = 0.0;

        if (r3.is_single_ru && !r3.ru_forms.empty()) {
            const std::string& rf3 = r3.ru_forms[0];
            auto it_ex = t1_ru_exact.find(rf3);
            if (it_ex != t1_ru_exact.end()) {
                best_i1 = it_ex->second[0];
                best_t1 = &t1_rows[best_i1];
                best_s13 = 1.0;
            }
            else {
                std::string st3 = extract_stem_from_latin(rf3);
                if (st3.size() >= 5) {
                    auto it_st = t1_stem_idx.find(st3);
                    if (it_st != t1_stem_idx.end()) {
                        for (size_t cand_i1 : it_st->second) {
                            for (const auto& f1 : t1_rows[cand_i1].latin_forms) {
                                double sim = compare_latin_forms(f1, rf3);
                                if (sim > best_s13) { best_s13 = sim; best_t1 = &t1_rows[cand_i1]; best_i1 = cand_i1; }
                            }
                        }
                    }
                }
            }
            if (best_s13 < 0.90) {
                for (const auto& ef3 : r3.en_forms) {
                    std::string st_en = extract_stem_from_latin(ef3);
                    if (st_en.size() >= 5) {
                        auto it_st = t1_stem_idx.find(st_en);
                        if (it_st != t1_stem_idx.end()) {
                            for (size_t cand_i1 : it_st->second) {
                                for (const auto& lf1 : t1_rows[cand_i1].lat_forms) {
                                    double sim = compare_latin_forms(lf1, ef3) * 0.97;
                                    if (sim > best_s13) { best_s13 = sim; best_t1 = &t1_rows[cand_i1]; best_i1 = cand_i1; }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (best_t1) {
            double min_t1 = best_t1->has_ling_tag ? 0.85 : 0.94;
            if (best_s13 < min_t1) { best_t1 = nullptr; best_s13 = 0.0; }
        }

        const Table2Row* best_t2 = nullptr;
        double best_s23 = 0.0;
        for (const auto& ph3 : r3.en_phrases) {
            auto it_ph = t2_phrase_idx.find(ph3);
            if (it_ph != t2_phrase_idx.end()) { best_t2 = &t2_rows[it_ph->second[0]]; best_s23 = 1.0; break; }
        }
        if (!best_t2) {
            for (const auto& ef3 : r3.en_forms) {
                auto it_w = t2_primary_idx.find(ef3);
                if (it_w != t2_primary_idx.end()) { best_t2 = &t2_rows[it_w->second[0]]; best_s23 = 0.99; break; }
            }
        }
        if (!best_t2) {
            for (const auto& ef3 : r3.en_forms) {
                auto it_s = t2_syn_idx.find(ef3);
                if (it_s != t2_syn_idx.end()) { best_t2 = &t2_rows[it_s->second[0]]; best_s23 = 0.97; break; }
            }
        }
        if (!best_t2) {
            for (const auto& ef3 : r3.en_forms) {
                if (ef3.size() >= 5 && ef3 != "informal" && ef3 != "termination" && ef3 != "information") {
                    std::string st = extract_stem_from_latin(ef3);
                    if (st.size() >= 5) {
                        auto it_st = t2_stem_idx.find(st);
                        if (it_st != t2_stem_idx.end()) {
                            for (size_t cand_i2 : it_st->second) {
                                for (const auto& w2 : t2_rows[cand_i2].primary_en) {
                                    double sim = compare_latin_forms(ef3, w2);
                                    if (sim > best_s23) { best_s23 = sim; best_t2 = &t2_rows[cand_i2]; }
                                }
                                for (const auto& w2 : t2_rows[cand_i2].syn_en) {
                                    double sim = compare_latin_forms(ef3, w2) * 0.96;
                                    if (sim > best_s23) { best_s23 = sim; best_t2 = &t2_rows[cand_i2]; }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (best_t2 && best_s23 < 0.86) { best_t2 = nullptr; best_s23 = 0.0; }

        if (best_t1 && !best_t2) {
            std::string arm1_low = arm_to_lower(best_t1->arm_word.substr(0, best_t1->arm_word.find(',')));
            for (const auto& f1 : best_t1->latin_forms) {
                std::string st1 = extract_stem_from_latin(f1);
                if (st1.size() >= 5) {
                    auto it_st = t2_stem_idx.find(st1);
                    if (it_st != t2_stem_idx.end()) {
                        for (size_t cand_i2 : it_st->second) {
                            for (const auto& w2 : t2_rows[cand_i2].primary_en) {
                                double sim = compare_latin_forms(f1, w2);
                                if (sim > best_s23) { best_s23 = sim; best_t2 = &t2_rows[cand_i2]; }
                            }
                        }
                    }
                }
            }
            if (!best_t2 && arm1_low.size() >= 8) {
                for (const auto& r2 : t2_rows) {
                    if (r2.arm_tokens.count(arm1_low)) { best_t2 = &r2; best_s23 = 0.93; break; }
                }
            }
            if (best_t2 && best_s23 < 0.86) { best_t2 = nullptr; best_s23 = 0.0; }
        }
        else if (best_t2 && !best_t1 && r3.is_single_ru) {
            for (const auto& w2 : best_t2->primary_en) {
                std::string st2 = extract_stem_from_latin(w2);
                if (st2.size() >= 5) {
                    auto it_st = t1_stem_idx.find(st2);
                    if (it_st != t1_stem_idx.end()) {
                        for (size_t cand_i1 : it_st->second) {
                            for (const auto& f1 : t1_rows[cand_i1].latin_forms) {
                                double sim = compare_latin_forms(f1, w2);
                                double min_t1 = t1_rows[cand_i1].has_ling_tag ? 0.86 : 0.94;
                                if (sim >= min_t1 && sim > best_s13) { best_s13 = sim; best_t1 = &t1_rows[cand_i1]; best_i1 = cand_i1; }
                            }
                        }
                    }
                }
            }
        }

        if (!best_t1 && !best_t2) continue;
        if (best_t1) used_t1.insert(best_i1);

        MergedRow mr;
        mr.prev_main_word = best_t1 ? best_t1->arm_word : best_t2->arm_primary;
        mr.ru = (best_t1 && !best_t1->ru_word.empty() && r3.is_single_ru) ? best_t1->ru_word : r3.ru_word;
        mr.en = !r3.en_word.empty() ? r3.en_word : (best_t2 ? best_t2->en_clean : (best_t1 ? best_t1->en_word : ""));
        mr.fr = !r3.fr_word.empty() ? r3.fr_word : (best_t1 ? best_t1->fr_word : "");
        mr.de = !r3.de_word.empty() ? r3.de_word : (best_t1 ? best_t1->de_word : "");
        mr.es = !r3.es_word.empty() ? r3.es_word : (best_t1 ? best_t1->es_word : "");
        mr.lat = best_t1 ? best_t1->lat_word : "";
        mr.gr = best_t1 ? best_t1->gr_word : "";
        mr.sphere = best_t1 ? best_t1->sphere : "լեզվբ.";
        mr.p_t1_t3 = best_t1 ? best_s13 : best_s23;
        mr.p_t1_t2 = best_t2 ? best_s23 : best_s13;

        std::string fallback_arm = best_t1 ? best_t1->arm_word : "";
        std::string fallback_en = best_t2 ? best_t2->en_clean : mr.en;
        mr.p_en = best_t2 ? best_s23 : (!mr.en.empty() ? compute_similarity_probability(mr.ru, mr.en, fallback_arm, "", mr.lat) : 0.0);
        mr.p_fr = !mr.fr.empty() ? compute_similarity_probability(mr.ru, mr.fr, fallback_arm, fallback_en, mr.lat) : 0.0;
        mr.p_de = !mr.de.empty() ? compute_similarity_probability(mr.ru, mr.de, fallback_arm, fallback_en, mr.lat) : 0.0;
        mr.p_es = !mr.es.empty() ? compute_similarity_probability(mr.ru, mr.es, fallback_arm, fallback_en, mr.lat) : 0.0;

        int count_langs = 1;
        double sum_p = std::max(mr.p_t1_t3, mr.p_t1_t2);
        if (best_t1 && best_t2) { sum_p = mr.p_t1_t3 + mr.p_t1_t2; count_langs = 2; }
        if (!mr.en.empty()) { sum_p += mr.p_en; count_langs++; }
        if (!mr.fr.empty()) { sum_p += mr.p_fr; count_langs++; }
        if (!mr.de.empty()) { sum_p += mr.p_de; count_langs++; }
        if (!mr.es.empty()) { sum_p += mr.p_es; count_langs++; }
        mr.p_mean = sum_p / static_cast<double>(count_langs);
        merged_rows.push_back(mr);
    }

    for (size_t i1 = 0; i1 < t1_rows.size(); ++i1) {
        if (used_t1.count(i1)) continue;
        const auto& r1 = t1_rows[i1];
        if (r1.latin_forms.empty()) continue;

        const Table3Row* best_t3 = nullptr;
        double best_s13 = 0.0;
        const double prefilter = r1.has_ling_tag ? 0.86 : 0.94;
        for (const auto& r3 : t3_rows) {
            double s = 0.0;
            for (const auto& f1 : r1.latin_forms) {
                for (const auto& f3 : r3.ru_forms) {
                    double sim = compare_latin_forms_fast(f1, f3, prefilter);
                    if (!r3.is_single_ru) sim *= 0.92;
                    if (sim > s) s = sim;
                }
                for (const auto& f3 : r3.en_forms) {
                    double sim = compare_latin_forms_fast(f1, f3, prefilter) * 0.96;
                    if (sim > s) s = sim;
                }
            }
            if (s > best_s13) { best_s13 = s; best_t3 = &r3; }
        }

        const Table2Row* best_t2 = nullptr;
        double best_s12 = 0.0;
        for (const auto& r2 : t2_rows) {
            double s = 0.0;
            for (const auto& f1 : r1.latin_forms) {
                for (const auto& f2 : r2.primary_en) {
                    double sim = compare_latin_forms_fast(f1, f2, prefilter);
                    if (sim > s) s = sim;
                }
                for (const auto& f2 : r2.syn_en) {
                    double sim = compare_latin_forms_fast(f1, f2, prefilter) * 0.95;
                    if (sim > s) s = sim;
                }
            }
            if (s > best_s12) { best_s12 = s; best_t2 = &r2; }
        }

        double min_thresh = r1.has_ling_tag ? 0.86 : 0.94;
        bool has_t3 = (best_t3 && best_s13 >= min_thresh);
        bool has_t2 = (best_t2 && best_s12 >= min_thresh);
        if (!has_t3 && !has_t2) continue;

        MergedRow mr;
        mr.prev_main_word = r1.arm_word;
        mr.ru = !r1.ru_word.empty() ? r1.ru_word : (has_t3 ? best_t3->ru_word : "");
        mr.en = (has_t3 && !best_t3->en_word.empty()) ? best_t3->en_word : (has_t2 ? best_t2->en_clean : r1.en_word);
        mr.fr = (has_t3 && !best_t3->fr_word.empty()) ? best_t3->fr_word : r1.fr_word;
        mr.de = (has_t3 && !best_t3->de_word.empty()) ? best_t3->de_word : r1.de_word;
        mr.es = (has_t3 && !best_t3->es_word.empty()) ? best_t3->es_word : r1.es_word;
        mr.lat = r1.lat_word;
        mr.gr = r1.gr_word;
        mr.sphere = r1.sphere;
        mr.p_t1_t3 = has_t3 ? best_s13 : best_s12;
        mr.p_t1_t2 = has_t2 ? best_s12 : best_s13;

        mr.p_en = !mr.en.empty() ? compute_similarity_probability(mr.ru, mr.en, r1.arm_word, "", mr.lat) : 0.0;
        mr.p_fr = !mr.fr.empty() ? compute_similarity_probability(mr.ru, mr.fr, r1.arm_word, mr.en, mr.lat) : 0.0;
        mr.p_de = !mr.de.empty() ? compute_similarity_probability(mr.ru, mr.de, r1.arm_word, mr.en, mr.lat) : 0.0;
        mr.p_es = !mr.es.empty() ? compute_similarity_probability(mr.ru, mr.es, r1.arm_word, mr.en, mr.lat) : 0.0;

        int count_langs = (has_t3 && has_t2) ? 2 : 1;
        double sum_p = (has_t3 && has_t2) ? (mr.p_t1_t3 + mr.p_t1_t2) : std::max(mr.p_t1_t3, mr.p_t1_t2);
        if (!mr.en.empty()) { sum_p += mr.p_en; count_langs++; }
        if (!mr.fr.empty()) { sum_p += mr.p_fr; count_langs++; }
        if (!mr.de.empty()) { sum_p += mr.p_de; count_langs++; }
        if (!mr.es.empty()) { sum_p += mr.p_es; count_langs++; }
        mr.p_mean = sum_p / static_cast<double>(count_langs);
        merged_rows.push_back(mr);
    }

    std::sort(merged_rows.begin(), merged_rows.end(), [](const MergedRow& a, const MergedRow& b) {
        if (std::abs(a.p_mean - b.p_mean) > 0.005) return a.p_mean > b.p_mean;
        int la = (!a.fr.empty()) + (!a.de.empty()) + (!a.es.empty()) + (!a.lat.empty()) + (!a.gr.empty());
        int lb = (!b.fr.empty()) + (!b.de.empty()) + (!b.es.empty()) + (!b.lat.empty()) + (!b.gr.empty());
        if (la != lb) return la > lb;
        return a.p_mean > b.p_mean;
        });

    std::string pdf_text = load_pdf_dictionary_text();
    std::map<std::string, std::string> pdf_word_cache;
    for (auto& r : merged_rows) {
        if (is_lowercase_main_word(r.prev_main_word)) {
            r.main_word = r.prev_main_word;
        }
        else {
            auto it_cache = pdf_word_cache.find(r.prev_main_word);
            if (it_cache != pdf_word_cache.end()) {
                r.main_word = it_cache->second;
            }
            else {
                std::string extracted = lookup_main_word_in_pdf_dictionary(pdf_text, r.prev_main_word);
                pdf_word_cache[r.prev_main_word] = extracted;
                r.main_word = extracted;
            }
        }
    }

    // 1. Сохраняем базовую таблицу Համընկնող բառերի աղյուսակ.csv БЕЗ промежуточных столбцов сравнения
    fs::path base_csv_path = fs::path(L"C:\\Users\\sokra\\source\\repos\\Verluchutyun\\Verluchutyun\\\u0540\u0561\u0574\u0568\u0576\u056F\u0576\u0578\u0572 \u0562\u0561\u057C\u0565\u0580\u056B \u0561\u0572\u0575\u0578\u0582\u057D\u0561\u056F.csv");
    std::ofstream base_csv(base_csv_path, std::ios::binary);
    if (!base_csv.is_open()) {
        base_csv_path = fs::path(L"\u0540\u0561\u0574\u0568\u0576\u056F\u0576\u0578\u0572 \u0562\u0561\u057C\u0565\u0580\u056B \u0561\u0572\u0575\u0578\u0582\u057D\u0561\u056F.csv");
        base_csv.open(base_csv_path, std::ios::binary);
    }
    base_csv << "\xEF\xBB\xBF";
    base_csv << "Основное слово (Арм.);Русский;Английский;Французский;Немецкий;Испанский;Латинский;Греческий;Сфера (помета)\r\n";
    for (const auto& r : merged_rows) {
        base_csv << csv_escape(r.main_word) << ";"
            << csv_escape(r.ru) << ";"
            << csv_escape(r.en) << ";"
            << csv_escape(r.fr) << ";"
            << csv_escape(r.de) << ";"
            << csv_escape(r.es) << ";"
            << csv_escape(r.lat) << ";"
            << csv_escape(r.gr) << ";"
            << csv_escape(r.sphere) << "\r\n";
    }
    base_csv.close();

    // 2. Внутреннее сравнение всех возможных случаев и агрегация 5 итоговых показателей
    size_t total_rows = merged_rows.size();
    size_t greco_latin_rows = 0;
    size_t greco_latin_per_lang[6] = { 0, 0, 0, 0, 0, 0 }; // Arm, Ru, En, Fr, De, Es
    size_t independent_arm_count = 0;
    size_t five_lang_coincide_count = 0;
    double five_lang_mean_accum = 0.0;
    size_t five_lang_valid_rows = 0;
    size_t multisphere_all_count = 0;
    size_t multisphere_strict_count = 0;
    size_t hnts_exact_count = 0;
    size_t hnts_ocr_count = 0;

    for (auto& r : merged_rows) {
        evaluate_all_cases_for_row(r);
        if (r.has_greco_latin_etymon) {
            greco_latin_rows++;
            for (int k = 0; k < 6; ++k) {
                if (r.is_greco_latin_loan_in_lang[k]) greco_latin_per_lang[k]++;
            }
        }
        if (r.is_independent_armenian) independent_arm_count++;
        if (r.five_langs_coincide) five_lang_coincide_count++;
        if (r.five_lang_sim_mean >= 0.0) {
            five_lang_mean_accum += r.five_lang_sim_mean;
            five_lang_valid_rows++;
        }
        if (r.is_general_multisphere) multisphere_all_count++;
        if (r.is_strict_ling_plus_other) multisphere_strict_count++;
        if (r.has_hnts_exact) hnts_exact_count++;
        if (r.has_hnts_with_ocr) hnts_ocr_count++;
    }

    size_t total_greco_latin_wordforms = 0;
    for (int k = 0; k < 6; ++k) total_greco_latin_wordforms += greco_latin_per_lang[k];

    double p_req1_rows = total_rows > 0 ? static_cast<double>(greco_latin_rows) / total_rows : 0.0;
    double p_req2_indep = total_rows > 0 ? static_cast<double>(independent_arm_count) / total_rows : 0.0;
    double p_req3_coincide = total_rows > 0 ? static_cast<double>(five_lang_coincide_count) / total_rows : 0.0;
    double p_req3_avg_sim = five_lang_valid_rows > 0 ? (five_lang_mean_accum / five_lang_valid_rows) : 0.0;
    double p_req4_multi = total_rows > 0 ? static_cast<double>(multisphere_all_count) / total_rows : 0.0;
    double p_req4_strict = total_rows > 0 ? static_cast<double>(multisphere_strict_count) / total_rows : 0.0;
    double p_req5_ocr = total_rows > 0 ? static_cast<double>(hnts_ocr_count) / total_rows : 0.0;
    double p_req5_exact = total_rows > 0 ? static_cast<double>(hnts_exact_count) / total_rows : 0.0;

    // 3. Создание НОВОЙ таблицы с итоговыми результатами (Վերլուծության արդյունքներ.csv)
    fs::path res_csv_path = fs::path(L"C:\\Users\\sokra\\source\\repos\\Verluchutyun\\Verluchutyun\\\u054E\u0565\u0580\u056C\u0578\u0582\u056E\u0578\u0582\u0569\u0575\u0561\u0576 \u0561\u0580\u0564\u0575\u0578\u0582\u0576\u0584\u0576\u0565\u0580.csv");
    std::ofstream res_csv(res_csv_path, std::ios::binary);
    if (!res_csv.is_open()) {
        res_csv_path = fs::path(L"\u054E\u0565\u0580\u056C\u0578\u0582\u056E\u0578\u0582\u0569\u0575\u0561\u0576 \u0561\u0580\u0564\u0575\u0578\u0582\u0576\u0584\u0576\u0565\u0580.csv");
        res_csv.open(res_csv_path, std::ios::binary);
    }

    res_csv << "\xEF\xBB\xBF";
    res_csv << "№;Պահանջ (Требование);Քանակ (Количество);Տոկոս (Процент);Մանրամասն արդյունք (Подробный результат)\r\n";

    std::ostringstream d1;
    d1 << "Рядов с греч./лат. корнем: " << greco_latin_rows << " (Всего словоформ в 6 языках: " << total_greco_latin_wordforms
        << " | Հայերեն: " << greco_latin_per_lang[0]
        << ", Ռուսերեն: " << greco_latin_per_lang[1]
        << ", Անգլերեն: " << greco_latin_per_lang[2]
        << ", Ֆրանսերեն: " << greco_latin_per_lang[3]
        << ", Գերմաներեն: " << greco_latin_per_lang[4]
        << ", Իսպաներեն: " << greco_latin_per_lang[5] << ")";

    res_csv << "1;"
        << csv_escape("1. Ընդհանուր քանի բառ ենք գտել որպես օտար բառեր նշված լեզուներում (հայերեն, ռուսերեն, անգլերեն, ֆրանսերեն, գերմաներեն, իսպաներեն), որոնք ծագել են հունարենից կամ լատիներենից") << ";"
        << greco_latin_rows << " (" << total_greco_latin_wordforms << " словоформ);"
        << pct_cell(p_req1_rows) << ";"
        << csv_escape(d1.str()) << "\r\n";

    res_csv << "2;"
        << csv_escape("2. Քանի՞ %-ի դեպքում հայերենում կան ինքնուրույն համարժեքներ") << ";"
        << independent_arm_count << " / " << total_rows << ";"
        << pct_cell(p_req2_indep) << ";"
        << csv_escape("Самостоятельные армянские эквиваленты (без заимствованного корня)") << "\r\n";

    std::ostringstream d3;
    d3 << "Совпадение корней в " << five_lang_coincide_count << " из " << total_rows
        << " рядов; средний процент межъязыкового сходства 5 языков: " << pct_cell(p_req3_avg_sim);

    res_csv << "3;"
        << csv_escape("3. Քանի՞ %-ի դեպքում են համընկնում անգլերեն - գերմաներեն - ֆրանսերեն - ռուսերեն - իսպաներեն համարժեքները") << ";"
        << five_lang_coincide_count << " / " << total_rows << ";"
        << pct_cell(p_req3_coincide) << ";"
        << csv_escape(d3.str()) << "\r\n";

    std::ostringstream d4;
    d4 << "Всего междисциплинарных слов: " << multisphere_all_count << " (" << pct_cell(p_req4_multi)
        << "); строго с пометой լեզվբ./քեր. + др. сфера: " << multisphere_strict_count << " (" << pct_cell(p_req4_strict) << ")";

    res_csv << "4;"
        << csv_escape("4. Հայերենում բառերի քանի՞ % է ընդհանրական՝ բացի լեզվբ․ ևս գոնե մեկ բնագավառում կիրառվում (կենսբ․, մաթ․ ևն)") << ";"
        << multisphere_all_count << " / " << total_rows << ";"
        << pct_cell(p_req4_multi) << ";"
        << csv_escape(d4.str()) << "\r\n";

    std::ostringstream d5;
    d5 << "С учетом OCR-вариантов հնց.: " << hnts_ocr_count << " (" << pct_cell(p_req5_ocr)
        << "); точное совпадение հնց.: " << hnts_exact_count << " (" << pct_cell(p_req5_exact) << ")";

    res_csv << "5;"
        << csv_escape("5. Քանի՞ % հնց․ նշումով") << ";"
        << hnts_ocr_count << " / " << total_rows << ";"
        << pct_cell(p_req5_ocr) << ";"
        << csv_escape(d5.str()) << "\r\n";

    res_csv.close();

    std::cout << "Базовая таблица Համընկնող բառերի աղյուսակ.csv сохранена без промежуточных столбцов сравнения.\n";
    std::cout << "Новая итоговая таблица создана: Վերլուծության արդյունքներ.csv\n\n";
    std::cout << "1. Греко-латинские заимствования: " << greco_latin_rows << " рядов (" << pct_cell(p_req1_rows)
        << "), всего " << total_greco_latin_wordforms << " словоформ в 6 языках\n";
    std::cout << "2. Самостоятельные армянские эквиваленты: " << independent_arm_count << " / " << total_rows
        << " (" << pct_cell(p_req2_indep) << ")\n";
    std::cout << "3. Совпадение Англ.-Нем.-Франц.-Рус.-Исп.: " << five_lang_coincide_count << " / " << total_rows
        << " (" << pct_cell(p_req3_coincide) << ", среднее сходство: " << pct_cell(p_req3_avg_sim) << ")\n";
    std::cout << "4. Междисциплинарные слова (кроме լեզվբ. еще >=1 сфера): " << multisphere_all_count << " / " << total_rows
        << " (" << pct_cell(p_req4_multi) << ")\n";
    std::cout << "5. С пометой հնց.: " << hnts_ocr_count << " / " << total_rows
        << " (" << pct_cell(p_req5_ocr) << ", точное հնց.: " << pct_cell(p_req5_exact) << ")\n";

    return 0;
}