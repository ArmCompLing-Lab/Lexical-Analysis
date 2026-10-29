// Требования: C++17 (/std:c++17), библиотека Poppler (через vcpkg)
#pragma warning(push)
#pragma warning(disable : 4251)
#include <poppler/cpp/poppler-global.h>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#pragma warning(pop)

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// 1. КОНВЕРТАЦИЯ UTF-8 <-> UTF-16 И НАСТРОЙКА КОНСОЛИ VISUAL STUDIO (БЕЗ '?')
// ============================================================================

std::wstring utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
#ifdef _WIN32
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (size_needed <= 0) return std::wstring();
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wstrTo[0], size_needed);
    return wstrTo;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string utf16ToUtf8(const std::wstring& utf16) {
    if (utf16.empty()) return std::string();
#ifdef _WIN32
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return std::string();
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
#else
    return std::string(utf16.begin(), utf16.end());
#endif
}

void setupVisualStudioConsole() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
#endif
}

void printW(const std::wstring& msg) {
    std::wcout << msg << L"\n";
}

// ============================================================================
// 2. ФУНКЦИИ ПРОВЕРКИ СИМВОЛОВ UNICODE (АРМЯНСКИЙ, КИРИЛЛИЦА, ЛАТИНИЦА)
// ============================================================================

std::wstring trimW(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring collapseSpacesW(const std::wstring& s) {
    std::wstring out;
    bool inSpace = false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            if (!inSpace) {
                out += L' ';
                inSpace = true;
            }
        }
        else {
            out += c;
            inSpace = false;
        }
    }
    return trimW(out);
}

// Заглавные армянские буквы: Ա (0x0531) .. Ֆ (0x0556)
bool isArmenianUpper(wchar_t c) {
    return (c >= 0x0531 && c <= 0x0556);
}

// Строчные армянские буквы: ա (0x0561) .. և (0x0587)
bool isArmenianLower(wchar_t c) {
    return (c >= 0x0561 && c <= 0x0587);
}

bool isArmenianChar(wchar_t c) {
    return isArmenianUpper(c) || isArmenianLower(c);
}

// Кириллица: 0x0400 .. 0x04FF
bool isCyrillicChar(wchar_t c) {
    return (c >= 0x0400 && c <= 0x04FF);
}

// Латиница + диакритика (включая макроны ā, ē, ī, ō, ū из словаря: 0x00C0 .. 0x024F)
bool isLatinChar(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0x00C0 && c <= 0x024F);
}

bool isDigitsOnly(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// Проверка: начинается ли строка с нового заголовочного слова (с учетом ошибок OCR: |, Հ и т.д.)
bool isNewEntryLine(const std::wstring& rawLine) {
    std::wstring line = trimW(rawLine);
    if (line.length() < 3) return false;

    size_t idx = 0;
    if (line[idx] == 0x00AB || line[idx] == L'"') idx++;
    if (idx >= line.length() || !isArmenianUpper(line[idx])) return false;

    int upperCount = 0;
    while (idx < line.length()) {
        wchar_t c = line[idx];
        // Если встретили одиночную букву Հ (0x0540), которую OCR поставил вместо '<' после заголовочного слова
        if (c == 0x0540 && upperCount >= 2 && idx > 0 && line[idx - 1] == L' ' &&
            idx + 1 < line.length() && line[idx + 1] == L' ') {
            break;
        }
        if (isArmenianUpper(c)) {
            upperCount++;
        }
        else if (c == L'(' || c == L')' || c == L'-' || c == 0x058A || c == L',' || c == L'.' || c == 0x00BB || c == L'"' || c == L' ') {
            if (c == L' ' && idx + 1 < line.length()) {
                wchar_t nextC = line[idx + 1];
                if (nextC == L'[' || nextC == L'|' || nextC == L'<' || nextC == L'-' ||
                    nextC == 0x2013 || nextC == 0x2014 || isArmenianLower(nextC) || isCyrillicChar(nextC)) {
                    break;
                }
            }
        }
        else {
            break;
        }
        idx++;
    }

    if (upperCount < 2) return false;

    bool hasDelimiter = (line.find(L'[') != std::wstring::npos) ||
        (line.find(L'|') != std::wstring::npos) ||
        (line.find(L'<') != std::wstring::npos) ||
        (line.find(L" \u0540 ") != std::wstring::npos) ||
        (line.find(L',') != std::wstring::npos) ||
        (line.find(L'-') != std::wstring::npos) ||
        (line.find(0x2013) != std::wstring::npos);
    return hasDelimiter;
}

// Транслитерация армянского слова в латиницу (для реконструкции опущенных корней вроде < հուն.])
std::wstring transliterateArmenianToLatin(const std::wstring& arm) {
    static const std::unordered_map<wchar_t, std::wstring> mapArm = {
        {0x0531, L"a"},  {0x0532, L"b"},  {0x0533, L"g"},  {0x0534, L"d"},
        {0x0535, L"e"},  {0x0536, L"z"},  {0x0537, L"e"},  {0x0538, L"e"},
        {0x0539, L"t"},  {0x053A, L"zh"}, {0x053B, L"i"},  {0x053C, L"l"},
        {0x053D, L"kh"}, {0x053E, L"ts"}, {0x053F, L"k"},  {0x0540, L"h"},
        {0x0541, L"dz"}, {0x0542, L"gh"}, {0x0543, L"ch"}, {0x0544, L"m"},
        {0x0545, L"y"},  {0x0546, L"n"},  {0x0547, L"sh"}, {0x0548, L"o"},
        {0x0549, L"ch"}, {0x054A, L"p"},  {0x054B, L"j"},  {0x054C, L"r"},
        {0x054D, L"s"},  {0x054E, L"v"},  {0x054F, L"t"},  {0x0550, L"r"},
        {0x0551, L"ts"}, {0x0552, L"u"},  {0x0553, L"p"},  {0x0554, L"k"},
        {0x0555, L"o"},  {0x0556, L"f"}
    };
    std::wstring res;
    for (size_t i = 0; i < arm.size(); ++i) {
        wchar_t c = arm[i];
        if (isArmenianLower(c) && c <= 0x0586) c -= 0x0030;
        if (c == 0x0548 && i + 1 < arm.size() && (arm[i + 1] == 0x0552 || arm[i + 1] == 0x0582)) {
            res += L"u";
            i++;
            continue;
        }
        auto it = mapArm.find(c);
        if (it != mapArm.end()) res += it->second;
    }
    return res;
}

// ============================================================================
// 3. СТРУКТУРА СЛОВАРНОЙ СТАТЬИ И СПИСОК ЯЗЫКОВ (СТРОГО СЛЕВА НАПРАВО)
// ============================================================================

enum class Lang {
    RUSSIAN,        // Русский (слово после [< или ռուս.)
    LATIN,          // Латинский (լատ., միջին լատ.)
    GREEK,          // Греческий (հուն.)
    FRENCH,         // Французский (ֆր., ֆը.)
    GERMAN,         // Немецкий (գերմ.)
    ENGLISH,        // Английский (անգլ.)
    ITALIAN,        // Итальянский (իտլ., իտալ.)
    SPANISH,        // Испанский (իսպ.)
    ARABIC,         // Арабский (արաբ.)
    PERSIAN,        // Персидский (պարսկ.)
    TURKISH,        // Турецкий (թուրք., թրք.)
    OTHER,          // Прочие языки (ասոր., հոլանդ., լեհ., ճապ., չին., պորտ., հին հրեերեն и др.)
    ARMENIAN_ROOT   // Внутр. арм. корень (напр. ագրո..., ակտինիում)
};

struct DictionaryEntry {
    std::wstring headword;                 // Чистое основное слово (напр., ԱԲԱԶԻԱ)
    std::map<Lang, std::wstring> langs;    // Языковые соответствия
    std::wstring partOfSpeech;             // Часть речи (գ., ած. и т.д.)
    std::vector<std::wstring> domains;     // Сферы: (բժշկ.), (տեխ.), (քիմ.) и т.д.
    std::wstring definition;               // Полное определение (хранится в памяти для поиска)
    std::wstring rawEtymology;             // Исходная строка этимологии внутри [...]
};

// ============================================================================
// 4. OCR-УСТОЙЧИВЫЙ ПАРСЕР СЛОВАРЯ И АЛГОРИТМ СВЯЗЫВАНИЯ КОРНЕЙ
// ============================================================================

class DictionaryParser {
private:
    std::vector<DictionaryEntry> entries;

    std::vector<std::wstring> splitW(const std::wstring& s, wchar_t delim) const {
        std::vector<std::wstring> elems;
        std::wstringstream ss(s);
        std::wstring item;
        while (std::getline(ss, item, delim)) {
            elems.push_back(trimW(item));
        }
        return elems;
    }

    void replaceAllW(std::wstring& str, const std::wstring& from, const std::wstring& to) const {
        if (from.empty()) return;
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::wstring::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }

    // Нормализует ошибки OCR внутри блока этимологии:
    // заменяет одиночную заглавную 'Հ' (0x0540) на '<', а 'ֆը.' на 'ֆր.' и т.д.
    std::wstring normalizeOcrEtymology(const std::wstring& rawEtymo) const {
        std::wstring s = rawEtymo;

        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == 0x0540) {
                bool nextIsLowerArm = (i + 1 < s.size()) && isArmenianLower(s[i + 1]);
                if (!nextIsLowerArm) {
                    s[i] = L'<';
                }
            }
        }

        replaceAllW(s, L"\u0586\u0568.", L"\u0586\u0580.");               // ֆը. -> ֆր.
        replaceAllW(s, L"\u0586\u0580,", L"\u0586\u0580.");               // ֆր, -> ֆր.
        replaceAllW(s, L"\u056C\u0561\u057F,", L"\u056C\u0561\u057F.");   // լատ, -> լատ.
        replaceAllW(s, L"\u0570\u0578\u0582\u0576,", L"\u0570\u0578\u0582\u0576."); // հուն, -> հուն.
        replaceAllW(s, L"\u0563\u0565\u0580\u0574,", L"\u0563\u0565\u0580\u0574."); // գերմ, -> գերմ.
        replaceAllW(s, L"\u0561\u0576\u0563\u056C,", L"\u0561\u0576\u0563\u056C."); // անգլ, -> անգլ.
        replaceAllW(s, L"\u056B\u057F\u0561\u056C.", L"\u056B\u057F\u056C.");       // իտալ. -> իտլ.
        replaceAllW(s, L"\u056B\u057F\u056C,", L"\u056B\u057F\u056C.");             // իտլ, -> իտլ.
        replaceAllW(s, L"\u056B\u057D\u057A,", L"\u056B\u057D\u057A.");             // իսպ, -> իսպ.
        replaceAllW(s, L"\u057C\u0578\u0582\u057D,", L"\u057C\u0578\u0582\u057D."); // ռուս, -> ռուս.
        replaceAllW(s, L"\u0561\u0580\u0561\u0562,", L"\u0561\u0580\u0561\u0562."); // արաբ, -> արաբ.
        replaceAllW(s, L"\u057A\u0561\u0580\u057D\u056F,", L"\u057A\u0561\u0580\u057D\u056F."); // պարսկ, -> պարսկ.
        replaceAllW(s, L"\u0569\u0580\u0584.", L"\u0569\u0578\u0582\u0580\u0584."); // թրք. -> թուրք.
        return s;
    }

    // Разделяет строку статьи на: 1) чистое основное слово, 2) блок этимологии, 3) определение
    void splitBlockIntoParts(const std::wstring& block, std::wstring& outHeadword, std::wstring& outEtymo, std::wstring& outAfter) const {
        outHeadword.clear();
        outEtymo.clear();
        outAfter.clear();

        size_t n = block.size();
        size_t i = 0;

        // 1. Извлекаем строго заглавное основное слово (останавливаемся перед [, |, <, Հ, кириллицей, латиницей и строчными буквами)
        int upperSeen = 0;
        while (i < n) {
            wchar_t c = block[i];
            if (c == L'[' || c == L'|' || c == L'<') break;

            if (c == 0x0540 && upperSeen >= 2 &&
                (i > 0 && (block[i - 1] == L' ' || block[i - 1] == L'[' || block[i - 1] == L'|')) &&
                (i + 1 < n && block[i + 1] == L' ')) {
                break;
            }

            if (isCyrillicChar(c) || isLatinChar(c) || isArmenianLower(c)) break;

            if ((c == L',' || c == L'-' || c == 0x2013 || c == 0x2014) && upperSeen >= 2) {
                size_t look = i + 1;
                while (look < n && block[look] == L' ') look++;
                if (look < n && !isArmenianUpper(block[look])) {
                    break;
                }
            }

            if (isArmenianUpper(c)) upperSeen++;
            outHeadword += c;
            i++;
        }

        outHeadword = trimW(outHeadword);
        while (!outHeadword.empty() && (outHeadword.back() == L',' || outHeadword.back() == L'-' ||
            outHeadword.back() == 0x2013 || outHeadword.back() == 0x2014 || outHeadword.back() == L'|' || outHeadword.back() == L'[')) {
            outHeadword.pop_back();
            outHeadword = trimW(outHeadword);
        }

        while (i < n && block[i] == L' ') i++;
        if (i >= n) return;

        // 2. Проверяем, начинается ли здесь блок этимологии
        bool hasEtymoOpener = false;
        if (block[i] == L'[' || block[i] == L'|' || block[i] == L'<') {
            hasEtymoOpener = true;
            if (block[i] == L'[' || block[i] == L'|') i++;
        }
        else if (block[i] == 0x0540 && i + 1 < n && block[i + 1] == L' ') {
            hasEtymoOpener = true;
        }
        else if (isCyrillicChar(block[i])) {
            hasEtymoOpener = true;
        }

        if (!hasEtymoOpener) {
            outAfter = trimW(block.substr(i));
            return;
        }

        // 3. Ищем конец блока этимологии (с учетом ошибок OCR: ], |, ), գ., ի Գ.)
        size_t etymoStart = i;
        size_t etymoEnd = std::wstring::npos;
        size_t afterStart = std::wstring::npos;
        size_t searchLimit = std::min<size_t>(n, etymoStart + 400);
        int parenDepth = 0;

        for (size_t k = etymoStart; k < searchLimit; ++k) {
            wchar_t c = block[k];
            if (c == L'(') {
                parenDepth++;
            }
            else if (c == L']' || c == L'|') {
                etymoEnd = k;
                afterStart = k + 1;
                break;
            }
            else if (c == L')') {
                if (parenDepth > 0) {
                    parenDepth--;
                }
                else {
                    etymoEnd = k;
                    afterStart = k + 1;
                    break;
                }
                size_t look = k + 1;
                if (look < n && block[look] == L',') look++;
                while (look < n && block[look] == L' ') look++;
                if (look + 1 < n && ((block[look] == 0x0563 && block[look + 1] == L'.') ||
                    (block[look] == 0x0533 && block[look + 1] == L'.') ||
                    (look + 2 < n && block[look] == 0x0561 && block[look + 1] == 0x056E && block[look + 2] == L'.'))) {
                    etymoEnd = k;
                    afterStart = k + 1;
                    break;
                }
            }
            else {
                if (c == L',' && k + 2 < n) {
                    size_t look = k + 1;
                    while (look < n && block[look] == L' ') look++;
                    if (look + 1 < n && ((block[look] == 0x0563 && block[look + 1] == L'.') ||
                        (block[look] == 0x0533 && block[look + 1] == L'.') ||
                        (look + 2 < n && block[look] == 0x0561 && block[look + 1] == 0x056E && block[look + 2] == L'.'))) {
                        etymoEnd = k;
                        if (etymoEnd > etymoStart && block[etymoEnd - 1] == 0x056B) {
                            etymoEnd--;
                        }
                        afterStart = k;
                        break;
                    }
                }
                if (c == 0x056B && k + 3 < n && block[k + 1] == L' ' &&
                    (block[k + 2] == 0x0533 || block[k + 2] == 0x0563) && block[k + 3] == L'.') {
                    etymoEnd = k;
                    afterStart = k + 1;
                    break;
                }
            }
        }

        if (etymoEnd != std::wstring::npos) {
            outEtymo = trimW(block.substr(etymoStart, etymoEnd - etymoStart));
            outAfter = trimW(block.substr(afterStart));
        }
        else {
            outAfter = trimW(block.substr(etymoStart));
        }
    }

    // Извлекает только чистое латинское написание корня (без армянского перевода)
    std::wstring extractLatinTokens(const std::wstring& textAfterTag) const {
        std::wstring result;
        bool seenLatin = false;

        for (size_t i = 0; i < textAfterTag.size(); ++i) {
            wchar_t c = textAfterTag[i];
            if (isLatinChar(c)) {
                result += c;
                seenLatin = true;
            }
            else if (seenLatin && (c == L'-' || c == L'(' || c == L')' || c == L' ' || c == L',' || c == L'.')) {
                result += c;
            }
            else if (isArmenianChar(c)) {
                if (seenLatin) {
                    break;
                }
                else {
                    size_t openParen = textAfterTag.find(L'(', i);
                    size_t closeParen = textAfterTag.find(L')', openParen != std::wstring::npos ? openParen : i);
                    if (openParen != std::wstring::npos && closeParen != std::wstring::npos) {
                        std::wstring inParen = trimW(textAfterTag.substr(openParen + 1, closeParen - openParen - 1));
                        if (!inParen.empty() && isLatinChar(inParen[0])) {
                            return inParen;
                        }
                    }
                    break;
                }
            }
        }

        result = collapseSpacesW(result);
        while (!result.empty() && (result.back() == L',' || result.back() == L' ' || result.back() == L'(' || result.back() == L'.')) {
            result.pop_back();
        }
        return trimW(result);
    }

    std::wstring extractCyrillicTokens(const std::wstring& segment) const {
        std::wstring result;
        for (wchar_t c : segment) {
            if (isCyrillicChar(c) || c == L'-' || c == L',' || c == L' ' || c == L'"' || c == 0x00AB || c == 0x00BB || c == L'.') {
                result += c;
            }
            else if (isArmenianChar(c) || isLatinChar(c)) {
                if (!result.empty()) break;
            }
        }
        result = collapseSpacesW(result);
        while (!result.empty() && (result.back() == L',' || result.back() == L'.' || result.back() == L' ')) {
            result.pop_back();
        }
        return trimW(result);
    }

    std::wstring extractArmenianRootTokens(const std::wstring& segment) const {
        std::wstring result;
        for (wchar_t c : segment) {
            if (isArmenianChar(c) || c == L'.' || c == L' ' || c == L'-') {
                result += c;
            }
        }
        result = collapseSpacesW(result);
        if (result == L"\u0570\u0574\u0574\u057F." ||
            result == L"\u0570\u0574\u057C\u057F\u0563.") {
            return L"";
        }
        return result;
    }

    void addLangValue(DictionaryEntry& entry, Lang lang, const std::wstring& val) {
        if (val.empty()) return;
        if (entry.langs[lang].empty()) {
            entry.langs[lang] = val;
        }
        else if (entry.langs[lang] == L"@IMPLIED@" && val != L"@IMPLIED@") {
            entry.langs[lang] = val;
        }
        else if (val != L"@IMPLIED@" && entry.langs[lang].find(val) == std::wstring::npos) {
            entry.langs[lang] += L" + " + val;
        }
    }

    struct LangMarkerInfo {
        std::wstring tag;
        Lang lang;
        std::wstring prefixForOther;
    };

    void parseEtymologyBlock(DictionaryEntry& entry, const std::wstring& rawEtymoContent) {
        std::wstring etymoContent = normalizeOcrEtymology(rawEtymoContent);
        entry.rawEtymology = etymoContent;

        static const std::vector<LangMarkerInfo> langMarkers = {
            { L"\u0574\u056B\u057B\u056B\u0576 \u056C\u0561\u057F.", Lang::LATIN, L"" },                 // միջին լատ.
            { L"\u0570\u056B\u0576 \u0570\u0580\u0565\u0565\u0580\u0565\u0576", Lang::OTHER, L"\u0570\u056B\u0576 \u0570\u0580. " }, // հին հրեերեն
            { L"\u0570\u0578\u056C\u0561\u0576\u0564.", Lang::OTHER, L"\u0570\u0578\u056C\u0561\u0576\u0564. " }, // հոլանդ.
            { L"\u057A\u0561\u0580\u057D\u056F.", Lang::PERSIAN, L"" },                                  // պարսկ.
            { L"\u0569\u0578\u0582\u0580\u0584.", Lang::TURKISH, L"" },                                  // թուրք.
            { L"\u057D\u0561\u0576\u057D\u056F\u0580.", Lang::OTHER, L"\u057D\u0561\u0576\u057D\u056F\u0580. " }, // սանսկր.
            { L"\u0561\u0580\u0561\u0562.", Lang::ARABIC, L"" },                                         // արաբ.
            { L"\u0561\u057D\u0578\u0580.", Lang::OTHER, L"\u0561\u057D\u0578\u0580. " },                // ասոր.
            { L"\u057A\u0578\u0580\u057F.", Lang::OTHER, L"\u057A\u0578\u0580\u057F. " },                // պորտ.
            { L"\u056C\u0565\u0570.", Lang::OTHER, L"\u056C\u0565\u0570. " },                            // լեհ.
            { L"\u0573\u0561\u057A.", Lang::OTHER, L"\u0573\u0561\u057A. " },                            // ճապ.
            { L"\u0579\u056B\u0576.", Lang::OTHER, L"\u0579\u056B\u0576. " },                            // չին.
            { L"\u0565\u0562\u0580.", Lang::OTHER, L"\u0565\u0562\u0580. " },                            // եբր.
            { L"\u0561\u0576\u0563\u056C.", Lang::ENGLISH, L"" },                                        // անգլ.
            { L"\u0563\u0565\u0580\u0574.", Lang::GERMAN, L"" },                                         // գերմ.
            { L"\u056C\u0561\u057F.", Lang::LATIN, L"" },                                                // լատ.
            { L"\u0570\u0578\u0582\u0576.", Lang::GREEK, L"" },                                          // հուն.
            { L"\u056B\u057F\u056C.", Lang::ITALIAN, L"" },                                              // իտլ.
            { L"\u056B\u057D\u057A.", Lang::SPANISH, L"" },                                              // իսպ.
            { L"\u057C\u0578\u0582\u057D.", Lang::RUSSIAN, L"" },                                        // ռուս.
            { L"\u0586\u0580.", Lang::FRENCH, L"" }                                                      // ֆր.
        };

        std::vector<std::wstring> chainSegments = splitW(etymoContent, L'<');
        Lang lastDetectedLang = Lang::RUSSIAN;
        std::wstring lastOtherPrefix;
        bool firstNonEmpty = true;

        for (const std::wstring& chainSeg : chainSegments) {
            if (chainSeg.empty()) continue;

            std::vector<std::wstring> plusParts = splitW(chainSeg, L'+');

            for (const std::wstring& part : plusParts) {
                if (part.empty()) continue;

                bool matchedTag = false;
                for (const auto& marker : langMarkers) {
                    size_t pos = part.find(marker.tag);
                    if (pos != std::wstring::npos) {
                        matchedTag = true;
                        lastDetectedLang = marker.lang;
                        lastOtherPrefix = marker.prefixForOther;
                        std::wstring afterTag = trimW(part.substr(pos + marker.tag.length()));

                        if (marker.lang == Lang::RUSSIAN) {
                            addLangValue(entry, Lang::RUSSIAN, extractCyrillicTokens(afterTag));
                        }
                        else {
                            std::wstring latTokens = extractLatinTokens(afterTag);
                            if (!latTokens.empty()) {
                                addLangValue(entry, marker.lang, marker.prefixForOther + latTokens);
                            }
                            else {
                                addLangValue(entry, marker.lang, L"@IMPLIED@");
                            }
                        }
                        break;
                    }
                }

                if (!matchedTag) {
                    if (firstNonEmpty) {
                        std::wstring cyr = extractCyrillicTokens(part);
                        if (!cyr.empty()) {
                            addLangValue(entry, Lang::RUSSIAN, cyr);
                        }
                    }
                    else {
                        std::wstring lat = extractLatinTokens(part);
                        if (!lat.empty() && lastDetectedLang != Lang::RUSSIAN) {
                            addLangValue(entry, lastDetectedLang, lastOtherPrefix + lat);
                        }
                        else {
                            std::wstring armRoot = extractArmenianRootTokens(part);
                            if (!armRoot.empty()) {
                                addLangValue(entry, Lang::ARMENIAN_ROOT, armRoot);
                            }
                        }
                    }
                }
                firstNonEmpty = false;
            }
        }
    }

    std::wstring getUpperArmenianStem(const std::wstring& text, size_t maxLen = 4) const {
        std::wstring stem;
        for (wchar_t c : text) {
            if (isArmenianUpper(c)) {
                stem += c;
            }
            else if (isArmenianLower(c) && c <= 0x0586) {
                stem += (wchar_t)(c - 0x0030);
            }
            if (stem.length() >= maxLen) break;
        }
        return stem;
    }

    void resolveCrossReferencesAndImpliedRoots() {
        std::unordered_map<std::wstring, std::vector<size_t>> stemIndex;
        for (size_t i = 0; i < entries.size(); ++i) {
            std::wstring stem = getUpperArmenianStem(entries[i].headword, 4);
            if (stem.length() >= 3) {
                stemIndex[stem].push_back(i);
            }
        }

        for (size_t i = 0; i < entries.size(); ++i) {
            auto& cur = entries[i];

            auto itArm = cur.langs.find(Lang::ARMENIAN_ROOT);
            if (itArm != cur.langs.end() && !itArm->second.empty()) {
                std::wstring rootStem = getUpperArmenianStem(itArm->second, 4);
                if (rootStem.length() >= 3 && stemIndex.count(rootStem)) {
                    for (size_t candIdx : stemIndex[rootStem]) {
                        if (candIdx == i) continue;
                        const auto& candidate = entries[candIdx];
                        for (const auto& kv : candidate.langs) {
                            if (kv.first != Lang::RUSSIAN && kv.first != Lang::ARMENIAN_ROOT &&
                                !kv.second.empty() && kv.second != L"@IMPLIED@") {
                                if (cur.langs[kv.first].empty()) {
                                    cur.langs[kv.first] = kv.second + L" [\u043E\u0442 " + candidate.headword + L"]";
                                }
                            }
                        }
                        break;
                    }
                }
            }

            for (auto& kv : cur.langs) {
                if (kv.second == L"@IMPLIED@") {
                    Lang targetLang = kv.first;
                    std::wstring resolved;
                    std::wstring curStem = getUpperArmenianStem(cur.headword, 5);

                    for (int offset = -8; offset <= 8; ++offset) {
                        int idx = (int)i + offset;
                        if (idx < 0 || idx >= (int)entries.size() || idx == (int)i) continue;
                        std::wstring otherStem = getUpperArmenianStem(entries[idx].headword, 5);
                        if (!curStem.empty() && otherStem.find(curStem.substr(0, std::min<size_t>(4, curStem.size()))) == 0) {
                            auto it = entries[idx].langs.find(targetLang);
                            if (it != entries[idx].langs.end() && !it->second.empty() && it->second != L"@IMPLIED@") {
                                resolved = it->second + L" [\u043A\u043E\u0440\u0435\u043D\u044C \u043E\u0442 " + entries[idx].headword + L"]";
                                break;
                            }
                        }
                    }

                    if (resolved.empty()) {
                        resolved = L"~" + transliterateArmenianToLatin(cur.headword) + L" [\u0438\u0437 " + cur.headword + L"]";
                    }
                    kv.second = resolved;
                }
            }
        }
    }

    void extractPosAndDomains(DictionaryEntry& entry, const std::wstring& afterHeader) {
        static const std::vector<std::wstring> posMarkers = {
            L"\u0563. \u0561\u056E.", // գ. ած.
            L"\u0563.",               // գ.
            L"\u0533.",               // Գ. (ошибка OCR вместо գ.)
            L"\u0561\u056E.",         // ած.
            L"\u0574\u056F."          // մկ.
        };

        std::wstring trimmed = trimW(afterHeader);
        for (const auto& pos : posMarkers) {
            if (trimmed.find(pos) == 0 || trimmed.find(L"1. " + pos) == 0) {
                entry.partOfSpeech = (pos == L"\u0533.") ? L"\u0563." : pos;
                break;
            }
        }

        size_t pos = 0;
        while ((pos = trimmed.find(L'(', pos)) != std::wstring::npos) {
            size_t closePos = trimmed.find(L')', pos + 1);
            if (closePos == std::wstring::npos) break;

            std::wstring inside = trimW(trimmed.substr(pos + 1, closePos - pos - 1));
            if (inside.length() >= 2 && inside.length() <= 25 && inside.back() == L'.') {
                bool hasArmenian = false;
                bool hasDigits = false;
                for (wchar_t c : inside) {
                    if (isArmenianLower(c)) hasArmenian = true;
                    if (c >= L'0' && c <= L'9') hasDigits = true;
                }
                if (hasArmenian && !hasDigits &&
                    inside.find(L"\u0585\u0580.") != 0 &&
                    inside.find(L"\u057F\u0565") != 0) {
                    if (std::find(entry.domains.begin(), entry.domains.end(), inside) == entry.domains.end()) {
                        entry.domains.push_back(inside);
                    }
                }
            }
            pos = closePos + 1;
        }
    }

public:
    bool loadFromPdf(const std::string& pdfPath) {
        std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(pdfPath));
        if (!doc) {
            printW(L"\u041E\u0428\u0418\u0411\u041A\u0410: \u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u043E\u0442\u043A\u0440\u044B\u0442\u044C PDF-\u0444\u0430\u0439\u043B: " + utf8ToUtf16(pdfPath));
            return false;
        }

        int numPages = doc->pages();
        printW(L"\u0424\u0430\u0439\u043B \u0443\u0441\u043F\u0435\u0448\u043D\u043E \u043E\u0442\u043A\u0440\u044B\u0442. \u0412\u0441\u0435\u0433\u043E \u0441\u0442\u0440\u0430\u043D\u0438\u0446: " + std::to_wstring(numPages));

        std::vector<std::wstring> allLines;
        allLines.reserve(static_cast<size_t>(numPages) * 45);

        for (int i = 0; i < numPages; ++i) {
            if ((i + 1) % 25 == 0 || i + 1 == numPages) {
                printW(L"\u0427\u0442\u0435\u043D\u0438\u0435 PDF: \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0430 " + std::to_wstring(i + 1) + L" \u0438\u0437 " + std::to_wstring(numPages) + L"...");
            }

            std::unique_ptr<poppler::page> p(doc->create_page(i));
            if (!p) continue;

            poppler::byte_array utf8Bytes = p->text().to_utf8();
            if (utf8Bytes.empty()) continue;

            std::string pageText(utf8Bytes.data(), utf8Bytes.size());
            std::wstring wPageText = utf8ToUtf16(pageText);

            std::wstringstream ss(wPageText);
            std::wstring line;
            while (std::getline(ss, line)) {
                allLines.push_back(line);
            }
        }

        printW(L"\u0418\u0437\u0432\u043B\u0435\u0447\u0435\u043D\u043E \u0441\u0442\u0440\u043E\u043A \u0442\u0435\u043A\u0441\u0442\u0430: " + std::to_wstring(allLines.size()) + L". \u041D\u0430\u0447\u0438\u043D\u0430\u0435\u0442\u0441\u044F \u0430\u043D\u0430\u043B\u0438\u0437 \u0441\u043B\u043E\u0432\u0430\u0440\u043D\u044B\u0445 \u0441\u0442\u0430\u0442\u0435\u0439...");
        parseLines(allLines);
        return true;
    }

    void parseLines(const std::vector<std::wstring>& rawLines) {
        // Автоматически пропускаем 12-страничное предисловие книги до первого слова словаря ԱԲԱԶԻԱ (\u0531\u0532\u0531\u0536\u053B\u0531)
        const std::wstring firstDictWord = L"\u0531\u0532\u0531\u0536\u053B\u0531"; // ԱԲԱԶԻԱ
        bool hasFirstDictWord = false;
        for (const auto& l : rawLines) {
            if (l.find(firstDictWord) != std::wstring::npos) {
                hasFirstDictWord = true;
                break;
            }
        }

        bool startedDictionary = !hasFirstDictWord;
        std::vector<std::wstring> mergedEntries;
        std::wstring currentBlock;

        for (const std::wstring& rawLine : rawLines) {
            std::wstring line = trimW(rawLine);
            if (line.empty()) continue;

            if (!startedDictionary) {
                if (line.find(firstDictWord) == 0) {
                    startedDictionary = true;
                }
                else {
                    continue;
                }
            }

            if (isDigitsOnly(line)) continue;
            if (line.length() <= 2 && !isNewEntryLine(line)) continue;

            if (isNewEntryLine(line)) {
                if (!currentBlock.empty()) {
                    mergedEntries.push_back(currentBlock);
                }
                currentBlock = line;
            }
            else if (!currentBlock.empty()) {
                if (!currentBlock.empty() && (currentBlock.back() == L'-' || currentBlock.back() == 0x058A)) {
                    currentBlock.pop_back();
                    currentBlock += line;
                }
                else {
                    currentBlock += L" " + line;
                }
            }
        }
        if (!currentBlock.empty()) {
            mergedEntries.push_back(currentBlock);
        }

        for (const std::wstring& block : mergedEntries) {
            DictionaryEntry entry;
            std::wstring etymo;
            std::wstring afterHeader;

            splitBlockIntoParts(block, entry.headword, etymo, afterHeader);
            if (entry.headword.length() < 2) continue;

            if (!etymo.empty()) {
                parseEtymologyBlock(entry, etymo);
            }

            if (!afterHeader.empty() && (afterHeader[0] == L',' || afterHeader[0] == L'-' || afterHeader[0] == 0x2013 || afterHeader[0] == 0x2014)) {
                afterHeader = trimW(afterHeader.substr(1));
            }

            extractPosAndDomains(entry, afterHeader);
            entry.definition = collapseSpacesW(afterHeader);

            entries.push_back(std::move(entry));
        }

        resolveCrossReferencesAndImpliedRoots();
    }

    const std::vector<DictionaryEntry>& getEntries() const { return entries; }

    // ========================================================================
    // 5. ЭКСПОРТ АККУРАТНОЙ И ЧИСТОЙ ТАБЛИЦЫ В CSV И HTML (БЕЗ ХАОСА И БЕЗ '?')
    // ========================================================================

    void exportToCsvAndHtml(const std::string& csvFilename, const std::string& htmlFilename) const {
        // Строгий порядок столбцов слева направо (после Основного слова в 1-м столбце)
        const std::vector<std::pair<Lang, std::wstring>> langCols = {
            {Lang::RUSSIAN,       L"\u0420\u0443\u0441\u0441\u043A\u0438\u0439"},                                   // Русский
            {Lang::LATIN,         L"\u041B\u0430\u0442\u0438\u043D\u0441\u043A\u0438\u0439 (\u056C\u0561\u057F.)"}, // Латинский (լատ.)
            {Lang::GREEK,         L"\u0413\u0440\u0435\u0447\u0435\u0441\u043A\u0438\u0439 (\u0570\u0578\u0582\u0576.)"}, // Греческий (հուն.)
            {Lang::FRENCH,        L"\u0424\u0440\u0430\u043D\u0446\u0443\u0437\u0441\u043A\u0438\u0439 (\u0586\u0580.)"}, // Французский (ֆր.)
            {Lang::GERMAN,        L"\u041D\u0435\u043C\u0435\u0446\u043A\u0438\u0439 (\u0563\u0565\u0580\u0574.)"},       // Немецкий (գերմ.)
            {Lang::ENGLISH,       L"\u0410\u043D\u0433\u043B\u0438\u0439\u0441\u043A\u0438\u0439 (\u0561\u0576\u0563\u056C.)"}, // Английский (անգլ.)
            {Lang::ITALIAN,       L"\u0418\u0442\u0430\u043B\u044C\u044F\u043D\u0441\u043A\u0438\u0439 (\u056B\u057F\u056C.)"}, // Итальянский (իտլ.)
            {Lang::SPANISH,       L"\u0418\u0441\u043F\u0430\u043D\u0441\u043A\u0438\u0439 (\u056B\u057D\u057A.)"},       // Испанский (իսպ.)
            {Lang::ARABIC,        L"\u0410\u0440\u0430\u0431\u0441\u043A\u0438\u0439 (\u0561\u0580\u0561\u0562.)"},       // Арабский (արաբ.)
            {Lang::PERSIAN,       L"\u041F\u0435\u0440\u0441\u0438\u0434\u0441\u043A\u0438\u0439 (\u057A\u0561\u0580\u057D\u056F.)"}, // Персидский (պարսկ.)
            {Lang::TURKISH,       L"\u0422\u0443\u0440\u0435\u0446\u043A\u0438\u0439 (\u0569\u0578\u0582\u0580\u0584.)"}, // Турецкий (թուրք.)
            {Lang::OTHER,         L"\u041F\u0440\u043E\u0447\u0438\u0435 \u044F\u0437\u044B\u043A\u0438"},               // Прочие языки
            {Lang::ARMENIAN_ROOT, L"\u0412\u043D\u0443\u0442\u0440. \u0430\u0440\u043C. \u043A\u043E\u0440\u0435\u043D\u044C"} // Внутр. арм. корень
        };

        const std::wstring colHeadword = L"\u041E\u0441\u043D\u043E\u0432\u043D\u043E\u0435 \u0441\u043B\u043E\u0432\u043E (\u0410\u0440\u043C.)"; // Основное слово (Арм.)
        const std::wstring colDomain = L"\u0421\u0444\u0435\u0440\u0430 (\u043F\u043E\u043C\u0435\u0442\u0430)"; // Сфера (помета)

        // 1. Запись CSV с маркером UTF-8 BOM для Excel
        std::ofstream csv(csvFilename, std::ios::binary);
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        csv.write(reinterpret_cast<const char*>(bom), sizeof(bom));

        auto escapeCsv = [](const std::wstring& ws) -> std::string {
            std::string s = utf16ToUtf8(ws);
            size_t pos = 0;
            while ((pos = s.find('"', pos)) != std::string::npos) {
                s.replace(pos, 1, "\"\"");
                pos += 2;
            }
            return "\"" + s + "\"";
            };

        csv << escapeCsv(colHeadword);
        for (const auto& col : langCols) {
            csv << ";" << escapeCsv(col.second);
        }
        csv << ";" << escapeCsv(colDomain) << "\r\n";

        for (const auto& e : entries) {
            csv << escapeCsv(e.headword);
            for (const auto& col : langCols) {
                auto it = e.langs.find(col.first);
                csv << ";" << escapeCsv(it != e.langs.end() ? it->second : L"");
            }
            std::wstring domStr;
            for (size_t i = 0; i < e.domains.size(); ++i) {
                domStr += (i > 0 ? L", " : L"") + e.domains[i];
            }
            csv << ";" << escapeCsv(domStr) << "\r\n";
        }
        csv.close();

        // 2. Запись визуально чистой HTML-таблицы с отдельными областями для каждого языка и живым поиском
        auto escapeHtml = [](const std::wstring& ws) -> std::string {
            std::wstring out;
            for (wchar_t c : ws) {
                if (c == L'&') out += L"&amp;";
                else if (c == L'<') out += L"&lt;";
                else if (c == L'>') out += L"&gt;";
                else if (c == L'"') out += L"&quot;";
                else out += c;
            }
            return utf16ToUtf8(out);
            };

        std::ofstream html(htmlFilename, std::ios::binary);
        html.write(reinterpret_cast<const char*>(bom), sizeof(bom));

        const std::wstring htmlTitle = L"\u0421\u043B\u043E\u0432\u0430\u0440\u044C \u0438\u043D\u043E\u0441\u0442\u0440\u0430\u043D\u043D\u044B\u0445 \u0441\u043B\u043E\u0432 \u2014 \u0422\u0430\u0431\u043B\u0438\u0446\u0430 \u044F\u0437\u044B\u043A\u043E\u0432\u044B\u0445 \u0441\u043E\u043E\u0442\u0432\u0435\u0442\u0441\u0442\u0432\u0438\u0439";
        const std::wstring htmlSubtitle = L"\u0412\u0441\u0435\u0433\u043E \u0441\u043B\u043E\u0432\u0430\u0440\u043D\u044B\u0445 \u0441\u0442\u0430\u0442\u0435\u0439: ";
        const std::wstring htmlSearchPlaceholder = L"\u0411\u044B\u0441\u0442\u0440\u044B\u0439 \u043F\u043E\u0438\u0441\u043A \u043F\u043E \u043B\u044E\u0431\u043E\u043C\u0443 \u0441\u043B\u043E\u0432\u0443, \u043A\u043E\u0440\u0435\u043D\u044E \u0438\u043B\u0438 \u0441\u0444\u0435\u0440\u0435...";

        html << "<!DOCTYPE html><html lang='ru'><head><meta charset='UTF-8'>\n"
            << "<title>" << escapeHtml(htmlTitle) << "</title>\n"
            << "<style>\n"
            << "* { box-sizing: border-box; }\n"
            << "body { font-family: 'Segoe UI', 'Sylfaen', 'Noto Sans Armenian', sans-serif; font-size: 13px; background: #f0f2f5; margin: 0; padding: 16px; color: #1e293b; }\n"
            << ".top-bar { display: flex; align-items: center; justify-content: space-between; background: #fff; padding: 12px 18px; border-radius: 10px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); margin-bottom: 14px; gap: 16px; flex-wrap: wrap; }\n"
            << ".title-block h1 { margin: 0; font-size: 17px; color: #0f172a; }\n"
            << ".title-block span { font-size: 12px; color: #64748b; }\n"
            << "#searchBox { min-width: 340px; padding: 8px 12px; border: 1px solid #cbd5e1; border-radius: 6px; font-size: 13px; outline: none; }\n"
            << "#searchBox:focus { border-color: #2563eb; box-shadow: 0 0 0 2px rgba(37,99,235,0.15); }\n"
            << ".table-wrap { background: #fff; border-radius: 10px; box-shadow: 0 1px 4px rgba(0,0,0,0.08); overflow: auto; max-height: 85vh; }\n"
            << "table { border-collapse: separate; border-spacing: 0; width: 100%; table-layout: fixed; }\n"
            << "th, td { border-bottom: 1px solid #e2e8f0; border-right: 1px solid #e2e8f0; padding: 7px 9px; text-align: left; vertical-align: middle; word-wrap: break-word; overflow-wrap: break-word; }\n"
            << "th { background: #1e293b; color: #f8fafc; font-weight: 600; font-size: 12px; position: sticky; top: 0; z-index: 3; }\n"
            << "th.col-hw { background: #0f172a; width: 155px; position: sticky; left: 0; z-index: 4; }\n"
            << "th.col-lang { width: 125px; }\n"
            << "th.col-dom { width: 95px; }\n"
            << "td.hw { background: #fef9c3; font-weight: 700; color: #713f12; font-size: 13.5px; position: sticky; left: 0; z-index: 2; border-right: 2px solid #eab308; cursor: help; }\n"
            << "tr:hover td { background: #f8fafc; }\n"
            << "tr:hover td.hw { background: #fef08a; }\n"
            << "td.empty { background: #f8fafc; }\n"
            << ".chip { display: inline-block; background: #eff6ff; color: #1e40af; border: 1px solid #bfdbfe; border-radius: 6px; padding: 3px 7px; font-size: 12.5px; line-height: 1.35; }\n"
            << ".chip-ru { background: #f0fdf4; color: #166534; border-color: #bbf7d0; }\n"
            << ".chip-dom { background: #f3e8ff; color: #6b21a8; border-color: #e9d5ff; font-size: 11.5px; }\n"
            << "</style></head><body>\n"
            << "<div class='top-bar'>\n"
            << "  <div class='title-block'><h1>" << escapeHtml(htmlTitle) << "</h1>"
            << "<span>" << escapeHtml(htmlSubtitle + std::to_wstring(entries.size())) << "</span></div>\n"
            << "  <input type='text' id='searchBox' placeholder='" << escapeHtml(htmlSearchPlaceholder) << "' oninput='filterRows()'>\n"
            << "</div>\n"
            << "<div class='table-wrap'><table id='dictTable'><thead><tr>"
            << "<th class='col-hw'>" << escapeHtml(colHeadword) << "</th>";

        for (const auto& col : langCols) {
            html << "<th class='col-lang'>" << escapeHtml(col.second) << "</th>";
        }
        html << "<th class='col-dom'>" << escapeHtml(colDomain) << "</th></tr></thead><tbody>\n";

        for (const auto& e : entries) {
            html << "<tr>";
            // При наведении мыши на основное слово всплывает подсказка с полным определением
            html << "<td class='hw' title='" << escapeHtml(e.definition) << "'>" << escapeHtml(e.headword) << "</td>";

            for (const auto& col : langCols) {
                auto it = e.langs.find(col.first);
                std::wstring val = (it != e.langs.end()) ? it->second : L"";
                if (val.empty()) {
                    html << "<td class='empty'></td>";
                }
                else {
                    std::string chipClass = (col.first == Lang::RUSSIAN) ? "chip chip-ru" : "chip";
                    html << "<td><span class='" << chipClass << "'>" << escapeHtml(val) << "</span></td>";
                }
            }

            std::wstring domStr;
            for (size_t i = 0; i < e.domains.size(); ++i) {
                domStr += (i > 0 ? L", " : L"") + e.domains[i];
            }
            if (domStr.empty()) {
                html << "<td class='empty'></td>";
            }
            else {
                html << "<td><span class='chip chip-dom'>" << escapeHtml(domStr) << "</span></td>";
            }
            html << "</tr>\n";
        }

        html << "</tbody></table></div>\n"
            << "<script>\n"
            << "function filterRows() {\n"
            << "  const q = document.getElementById('searchBox').value.toLowerCase();\n"
            << "  const rows = document.querySelectorAll('#dictTable tbody tr');\n"
            << "  for (let i = 0; i < rows.length; i++) {\n"
            << "    rows[i].style.display = rows[i].textContent.toLowerCase().includes(q) ? '' : 'none';\n"
            << "  }\n"
            << "}\n"
            << "</script></body></html>";
        html.close();
    }

    // ========================================================================
    // 6. ПОИСК ПО КЛЮЧЕВЫМ СЛОВАМ И ПРИЗНАКАМ В КОНСОЛИ
    // ========================================================================

    struct SearchFilter {
        std::wstring headwordPrefix;
        std::wstring domainTag;
        std::wstring foreignRootQuery;
        std::wstring keywordInDef;
    };

    std::vector<DictionaryEntry> search(const SearchFilter& filter) const {
        std::vector<DictionaryEntry> results;
        for (const auto& e : entries) {
            if (!filter.headwordPrefix.empty() && e.headword.find(filter.headwordPrefix) != 0)
                continue;

            if (!filter.domainTag.empty()) {
                bool foundDomain = false;
                for (const auto& d : e.domains) {
                    if (d.find(filter.domainTag) != std::wstring::npos) {
                        foundDomain = true;
                        break;
                    }
                }
                if (!foundDomain) continue;
            }

            if (!filter.foreignRootQuery.empty()) {
                bool foundRoot = false;
                for (const auto& kv : e.langs) {
                    if (kv.second.find(filter.foreignRootQuery) != std::wstring::npos) {
                        foundRoot = true;
                        break;
                    }
                }
                if (!foundRoot) continue;
            }

            if (!filter.keywordInDef.empty() &&
                e.definition.find(filter.keywordInDef) == std::wstring::npos &&
                e.rawEtymology.find(filter.keywordInDef) == std::wstring::npos)
                continue;

            results.push_back(e);
        }
        return results;
    }
};

// ============================================================================
// 7. АВТОМАТИЧЕСКИЙ ПОИСК ФАЙЛА "output_bararan_114_digitized.pdf" И ЗАПУСК
// ============================================================================

std::string locatePdfFile(const std::string& targetName) {
    const std::vector<fs::path> candidatePaths = {
        fs::path(targetName),
        fs::path("..") / targetName,
        fs::path("../..") / targetName,
        fs::path("../../..") / targetName
    };

    for (const auto& p : candidatePaths) {
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            return fs::absolute(p, ec).string();
        }
    }
    return targetName;
}

int main() {
    setupVisualStudioConsole();

    const std::string targetPdf = "output_bararan_114_digitized.pdf";
    std::string resolvedPdfPath = locatePdfFile(targetPdf);

    DictionaryParser parser;
    if (!parser.loadFromPdf(resolvedPdfPath)) {
        printW(L"\n\u041F\u043E\u0436\u0430\u043B\u0443\u0439\u0441\u0442\u0430, \u043F\u043E\u043B\u043E\u0436\u0438\u0442\u0435 \u0444\u0430\u0439\u043B '" + utf8ToUtf16(targetPdf) + L"' \u0432 \u043F\u0430\u043F\u043A\u0443 \u0432\u0430\u0448\u0435\u0433\u043E \u043F\u0440\u043E\u0435\u043A\u0442\u0430:");
        printW(fs::current_path().wstring());
        return 1;
    }

    printW(L"\u0410\u043D\u0430\u043B\u0438\u0437 \u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043D! \u0412\u0441\u0435\u0433\u043E \u0440\u0430\u0441\u043F\u043E\u0437\u043D\u0430\u043D\u043E \u0441\u043B\u043E\u0432\u0430\u0440\u043D\u044B\u0445 \u0441\u0442\u0430\u0442\u0435\u0439: " + std::to_wstring(parser.getEntries().size()));

    std::string csvOut = "dictionary_table.csv";
    std::string htmlOut = "dictionary_table.html";
    parser.exportToCsvAndHtml(csvOut, htmlOut);

    printW(L"\n\u0413\u043E\u0442\u043E\u0432\u044B\u0435 \u0442\u0430\u0431\u043B\u0438\u0446\u044B \u0441\u043E \u0432\u0441\u0435\u043C\u0438 \u0441\u043B\u043E\u0432\u0430\u043C\u0438 \u0443\u0441\u043F\u0435\u0448\u043D\u043E \u0441\u043E\u0437\u0434\u0430\u043D\u044B:");
    printW(L"1. CSV (\u0434\u043B\u044F Excel):   " + (fs::current_path() / csvOut).wstring());
    printW(L"2. HTML (\u0432 \u0431\u0440\u0430\u0443\u0437\u0435\u0440\u0435): " + (fs::current_path() / htmlOut).wstring());

    while (true) {
        printW(L"\n==================================================================");
        printW(L"\u041F\u041E\u0418\u0421\u041A \u041F\u041E \u0421\u041B\u041E\u0412\u0410\u0420\u042E (\u043D\u0430\u0436\u043C\u0438\u0442\u0435 Enter \u043D\u0430 \u043F\u0443\u0441\u0442\u043E\u043C \u043F\u043E\u043B\u0435, \u0447\u0442\u043E\u0431\u044B \u043F\u0440\u043E\u043F\u0443\u0441\u0442\u0438\u0442\u044C):");

        DictionaryParser::SearchFilter filter;
        std::wstring input;

        printW(L"1. \u041D\u0430\u0447\u0430\u043B\u043E \u043E\u0441\u043D\u043E\u0432\u043D\u043E\u0433\u043E \u0441\u043B\u043E\u0432\u0430 (\u043D\u0430\u043F\u0440\u0438\u043C\u0435\u0440, \u0531\u0532 \u0438\u043B\u0438 \u0531\u053F\u054F): ");
        if (!std::getline(std::wcin, input)) break;
        filter.headwordPrefix = trimW(input);

        printW(L"2. \u041F\u043E\u043C\u0435\u0442\u0430 \u0441\u0444\u0435\u0440\u044B (\u043D\u0430\u043F\u0440\u0438\u043C\u0435\u0440, \u0562\u056A\u0577\u056F., \u057F\u0565\u056D., \u0584\u056B\u0574., \u0574\u0561\u0569.): ");
        if (!std::getline(std::wcin, input)) break;
        filter.domainTag = trimW(input);

        printW(L"3. \u0418\u043D\u043E\u0441\u0442\u0440\u0430\u043D\u043D\u044B\u0439 \u043A\u043E\u0440\u0435\u043D\u044C (\u043D\u0430\u043F\u0440\u0438\u043C\u0435\u0440, abacus, aktis, \u0430\u0431\u0430\u0436\u0443\u0440): ");
        if (!std::getline(std::wcin, input)) break;
        filter.foreignRootQuery = trimW(input);

        printW(L"4. \u041A\u043B\u044E\u0447\u0435\u0432\u043E\u0435 \u0441\u043B\u043E\u0432\u043E \u0432 \u043E\u043F\u0440\u0435\u0434\u0435\u043B\u0435\u043D\u0438\u0438: ");
        if (!std::getline(std::wcin, input)) break;
        filter.keywordInDef = trimW(input);

        auto results = parser.search(filter);
        printW(L"\n\u041D\u0430\u0439\u0434\u0435\u043D\u043E \u0441\u0442\u0430\u0442\u0435\u0439: " + std::to_wstring(results.size()));

        for (size_t i = 0; i < results.size() && i < 25; ++i) {
            const auto& r = results[i];
            printW(L"------------------------------------------------------------------");
            printW(L"\u041E\u0441\u043D\u043E\u0432\u043D\u043E\u0435 \u0441\u043B\u043E\u0432\u043E: " + r.headword + (r.partOfSpeech.empty() ? L"" : L" (" + r.partOfSpeech + L")"));
            if (!r.rawEtymology.empty()) {
                printW(L"\u042D\u0442\u0438\u043C\u043E\u043B\u043E\u0433\u0438\u044F:     [" + r.rawEtymology + L"]");
            }
            printW(L"\u041E\u043F\u0440\u0435\u0434\u0435\u043B\u0435\u043D\u0438\u0435:    " + r.definition);
        }
        if (results.size() > 25) {
            printW(L"... (\u043F\u043E\u043A\u0430\u0437\u0430\u043D\u044B \u043F\u0435\u0440\u0432\u044B\u0435 25 \u0438\u0437 " + std::to_wstring(results.size()) + L" \u0440\u0435\u0437\u0443\u043B\u044C\u0442\u0430\u0442\u043E\u0432)");
        }
    }

    return 0;
}