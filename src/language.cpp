/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2013-2020 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "language.h"

#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <memory>

#include <boost/algorithm/string.hpp>

#include <unicode/uvernum.h>
#include <unicode/locid.h>
#include <unicode/coll.h>
#include <unicode/utypes.h>

#include <wx/filename.h>

#include "str_helpers.h"
#include "pluralforms/pl_evaluate.h"

#ifdef HAVE_CLD2
    #ifdef HAVE_CLD2_PUBLIC_COMPACT_LANG_DET_H
        #include <cld2/public/compact_lang_det.h>
        #include <cld2/public/encodings.h>
    #else
        #include "public/compact_lang_det.h"
        #include "public/encodings.h"
    #endif
#endif

// GCC's libstdc++ didn't have functional std::regex implementation until 4.9
#if (defined(__GNUC__) && !defined(__clang__) && !wxCHECK_GCC_VERSION(4,9))
    #include <boost/regex.hpp>
    using boost::regex;
    using boost::wregex;
    using boost::regex_match;
    using boost::regex_search;
    using boost::smatch;
    using boost::cmatch;
#else
    #include <regex>
    using std::regex;
    using std::wregex;
    using std::regex_match;
    using std::regex_search;
    using std::smatch;
    using std::cmatch;
#endif

namespace
{

// see http://www.gnu.org/software/gettext/manual/html_node/Header-Entry.html
// for description of permitted formats
const wregex RE_LANG_CODE(L"([a-z]){2,3}(_([A-Z]{2}|[0-9]{3}))?(@[a-z]+)?");

// a more permissive variant of the same that TryNormalize() would fix
const wregex RE_LANG_CODE_PERMISSIVE(L"([a-zA-Z]){2,3}([_-]([a-zA-Z]{2}|[0-9]{3}))?(@[a-zA-Z]+)?");

// try some normalizations: s/-/_/, case adjustments
void TryNormalize(std::wstring& s)
{
    auto begin = s.begin();
    auto end = s.end();

    size_t pos = s.rfind('@');
    if (pos != std::wstring::npos)
    {
        for (auto x = begin + pos; x != end; ++x)
            *x = std::tolower(*x);
        end = begin + pos;
    }

    bool upper = false;
    for (auto x = begin; x != end; ++x)
    {
        if (*x == '-')
            *x = '_';
        if (*x == '_')
            upper = true;
        else if (std::isupper(*x) && !upper)
            *x = std::tolower(*x);
        else if (std::islower(*x) && upper)
            *x = std::toupper(*x);
    }
}

bool IsISOLanguage(const std::string& s)
{
    const char *test = s.c_str();
    for (const char * const* i = icu::Locale::getISOLanguages(); *i != nullptr; ++i)
    {
        if (strcmp(test, *i) == 0)
            return true;
    }
    return false;
}

bool IsISOCountry(const std::string& s)
{
    const char *test = s.c_str();
    for (const char * const* i = icu::Locale::getISOCountries(); *i != nullptr; ++i)
    {
        if (strcmp(test, *i) == 0)
            return true;
    }
    return false;
}

// Mapping of names to their respective ISO codes.
struct DisplayNamesData
{
    typedef std::unordered_map<std::wstring, std::string> Map;
    Map names, namesEng;
    std::vector<std::wstring> sortedNames;
};

std::once_flag of_namesList;

const DisplayNamesData& GetDisplayNamesData()
{
    static DisplayNamesData data;

    std::call_once(of_namesList, [=]{
        auto locEng = icu::Locale::getEnglish();
        std::vector<icu::UnicodeString> names;

        int32_t count;
        const icu::Locale *loc = icu::Locale::getAvailableLocales(count);
        names.reserve(count);
        for (int i = 0; i < count; i++, loc++)
        {
            auto language = loc->getLanguage();
            auto script = loc->getScript();
            auto country = loc->getCountry();
            auto variant = loc->getVariant();

            // TODO: for now, ignore variants here and in FormatForRoundtrip(),
            //       because translating them between gettext and ICU is nontrivial
            if (variant != nullptr && *variant != '\0')
                continue;

            icu::UnicodeString s;
            loc->getDisplayName(s);
            names.push_back(s);

            if (strcmp(language, "zh") == 0 && *country == '\0')
            {
                if (strcmp(script, "Hans") == 0)
                    country = "CN";
                else if (strcmp(script, "Hant") == 0)
                    country = "TW";
            }

            std::string code(language);
            if (*country != '\0')
            {
                code += '_';
                code += country;
            }
            if (*script != '\0')
            {
                if (strcmp(script, "Latn") == 0)
                {
                    code += "@latin";
                }
                else if (strcmp(script, "Cyrl") == 0)
                {
                    // add @cyrillic only if it's not the default already
                    if (strcmp(language, "sr") != 0)
                        code += "@cyrillic";
                }
            }
            
            s.foldCase();
            data.names[str::to_wstring(s)] = code;

            loc->getDisplayName(locEng, s);
            s.foldCase();
            data.namesEng[str::to_wstring(s)] = code;
        }

        // sort the names alphabetically for data.sortedNames:
        UErrorCode err = U_ZERO_ERROR;
        std::unique_ptr<icu::Collator> coll(icu::Collator::createInstance(err));
        if (coll)
        {
            coll->setStrength(icu::Collator::SECONDARY); // case insensitive

            std::sort(names.begin(), names.end(),
                      [&coll](const icu::UnicodeString& a, const icu::UnicodeString& b){
                          UErrorCode e = U_ZERO_ERROR;
                          return coll->compare(a, b, e) == UCOL_LESS;
                      });
        }
        else
        {
            std::sort(names.begin(), names.end());
        }

        // convert into std::wstring
        data.sortedNames.reserve(names.size());
        for (auto s: names)
            data.sortedNames.push_back(str::to_wstring(s));
    });

    return data;
}

std::string DoGetLanguageTag(const Language& lang)
{
    auto l = lang.Lang();
    auto c = lang.Country();
    auto v = lang.Variant();

    std::string tag = l;

    if (v == "latin")
    {
        tag += "-Latn";
        v.clear();
    }
    else if (v == "cyrillic")
    {
        tag += "-Cyrl";
        v.clear();
    }

    if (!c.empty())
        tag += "-" + c;

    if (!v.empty())
    {
        // Encode variant that wasn't special-handled as a private use subtag, see
        // https://tools.ietf.org/html/rfc5646#section-2.2.7 (e.g. "de-DE-x-formal")
        tag += "-x-" + v;
    }

    return tag;
}

bool DoIsRTL(const Language& lang)
{
#if U_ICU_VERSION_MAJOR_NUM >= 51
    auto locale = lang.IcuLocaleName();
    UErrorCode err = U_ZERO_ERROR;
    UScriptCode codes[10]= {USCRIPT_INVALID_CODE};
    if (uscript_getCode(locale.c_str(), codes, 10, &err) == 0 || err != U_ZERO_ERROR)
        return false; // fallback
    return uscript_isRightToLeft(codes[0]);
#else
    return false; // fallback
#endif
}

} // anonymous namespace


void Language::Init(const std::string& code)
{
    m_code = code;

    if (IsValid())
    {
        m_tag = DoGetLanguageTag(*this);
        m_icuLocale = m_tag;
        m_direction = DoIsRTL(*this) ? TextDirection::RTL : TextDirection::LTR;
    }
    else
    {
        m_tag.clear();
        m_icuLocale.clear();
        m_direction = TextDirection::LTR;
    }
}

bool Language::IsValidCode(const std::wstring& s)
{
    return regex_match(s, RE_LANG_CODE);
}

std::string Language::Lang() const
{
    return m_code.substr(0, m_code.find_first_of("_@"));
}

std::string Language::Country() const
{
    const size_t pos = m_code.find('_');
    if (pos == std::string::npos)
        return std::string();

    const size_t endpos = m_code.rfind('@');
    if (endpos == std::string::npos)
        return m_code.substr(pos+1);
    else
        return m_code.substr(pos+1, endpos - (pos+1));
}

std::string Language::LangAndCountry() const
{
    return m_code.substr(0, m_code.rfind('@'));
}

std::string Language::Variant() const
{
    const size_t pos = m_code.rfind('@');
    if (pos == std::string::npos)
        return std::string();
    else
        return m_code.substr(pos + 1);
}

Language Language::TryParse(const std::wstring& s)
{
    if (IsValidCode(s))
        return Language(s);

    if (s == "zh-Hans")
        return Language("zh_CN");
    else if (s == "zh-Hant")
        return Language("zh_TW");

    // Is it a standard language code?
    if (regex_match(s, RE_LANG_CODE_PERMISSIVE))
    {
        std::wstring s2(s);
        TryNormalize(s2);
        if (IsValidCode(s2))
            return Language(s2);
    }

    // If not, perhaps it's a human-readable name (perhaps coming from the language control)?
    auto names = GetDisplayNamesData();
    icu::UnicodeString s_icu = str::to_icu(s);
    s_icu.foldCase();
    std::wstring folded = str::to_wstring(s_icu);
    auto i = names.names.find(folded);
    if (i != names.names.end())
        return Language(i->second);

    // Maybe it was in English?
    i = names.namesEng.find(folded);
    if (i != names.namesEng.end())
        return Language(i->second);

    // Maybe it was a BCP 47 language tag?
    auto fromTag = FromLanguageTag(str::to_utf8(s));
    if (fromTag.IsValid())
        return fromTag;

    return Language(); // invalid
}


Language Language::TryParseWithValidation(const std::wstring& s)
{
    Language lang = Language::TryParse(s);
    if (!lang.IsValid())
        return Language(); // invalid

    if (!IsISOLanguage(lang.Lang()))
        return Language(); // invalid

    auto country = lang.Country();
    if (!country.empty() && !IsISOCountry(country))
        return Language(); // invalid

    return lang;
}


Language Language::FromLanguageTag(const std::string& tag)
{
    char locale[512];
    UErrorCode status = U_ZERO_ERROR;
    uloc_forLanguageTag(tag.c_str(), locale, 512, NULL, &status);
    if (U_FAILURE(status))
        return Language();

    Language lang;
    lang.m_tag = tag;
    lang.m_icuLocale = locale;

    char buf[512];
    if (uloc_getLanguage(locale, buf, 512, &status))
        lang.m_code = buf;
    if (uloc_getCountry(locale, buf, 512, &status))
        lang.m_code += "_" + std::string(buf);

    // ICU converts private use subtag into 'x' keyword, e.g. de-DE-x-formal => de_DE@x=formal
    static const regex re_private_subtag("@x=([^@]+)$");
    cmatch m;
    if (regex_search(locale, m, re_private_subtag))
        lang.m_code += "@" + m.str(1);

    return lang;
}


Language Language::FromLegacyNames(const std::string& lang, const std::string& country)
{
    if (lang.empty())
        return Language(); // invalid

    #include "language_impl_legacy.h"

    std::string code;
    auto i = isoLanguages.find(lang);
    if ( i != isoLanguages.end() )
        code = i->second;
    else
        return Language(); // invalid

    if (!country.empty())
    {
        auto iC = isoCountries.find(country);
        if ( iC != isoCountries.end() )
            code += "_" + iC->second;
    }

    return Language(code);
}


PluralFormsExpr Language::DefaultPluralFormsExpr() const
{
    if (!IsValid())
        return PluralFormsExpr();

    static const std::unordered_map<std::string, PluralFormsExpr> forms = {
        #include "language_impl_plurals.h"
    };

    auto i = forms.find(m_code);
    if ( i != forms.end() )
        return i->second;

    i = forms.find(LangAndCountry());
    if ( i != forms.end() )
        return i->second;

    i = forms.find(Lang());
    if ( i != forms.end() )
        return i->second;

    return PluralFormsExpr();
}


int Language::nplurals() const
{
    return DefaultPluralFormsExpr().nplurals();
}


icu::Locale Language::ToIcu() const
{
    if (!IsValid())
        return icu::Locale::getEnglish();

    return icu::Locale(IcuLocaleName().c_str());
}


wxString Language::DisplayName() const
{
    icu::UnicodeString s;
    ToIcu().getDisplayName(s);
    return str::to_wx(s);
}

wxString Language::LanguageDisplayName() const
{
    icu::UnicodeString s;
    ToIcu().getDisplayLanguage(s);
    return str::to_wx(s);
}

wxString Language::DisplayNameInItself() const
{
    auto loc = ToIcu();
    icu::UnicodeString s;
    loc.getDisplayName(loc, s);
    return str::to_wx(s);
}

wxString Language::FormatForRoundtrip() const
{
    // Can't show all variants nicely, but some common one can be
    auto v = Variant();
    if (!v.empty() && v != "latin" && v != "cyrillic")
        return m_code;

    wxString disp = DisplayName();
    // ICU isn't 100% reliable, some of the display names it produces
    // (e.g. "Chinese (China)" aren't in the list of known locale names
    // (here because zh-Trans is preferred to zh_CN). So make sure it can
    // be parsed back first.
    if (TryParse(disp.ToStdWstring()).IsValid())
        return disp;
    else
        return m_code;
}


const std::vector<std::wstring>& Language::AllFormattedNames()
{
    return GetDisplayNamesData().sortedNames;
}


Language Language::TryGuessFromFilename(const wxString& filename)
{
    wxFileName fn(filename);
    fn.MakeAbsolute();

    // Try matching the filename first:
    //  - entire name
    //  - suffix (foo.cs_CZ.po, wordpressTheme-cs_CZ.po)
    //  - directory name (cs_CZ, cs.lproj, cs/LC_MESSAGES)
    std::wstring name = fn.GetName().ToStdWstring();
    Language lang = Language::TryParseWithValidation(name);
            if (lang.IsValid())
                return lang;

    size_t pos = name.find_first_of(L".-_");
    while (pos != wxString::npos)
    {
        auto part = name.substr(pos+1);
        lang = Language::TryParseWithValidation(part);
        if (lang.IsValid())
            return lang;
         pos = name.find_first_of(L".-_",  pos+1);
    }

    auto dirs = fn.GetDirs();
    if (!dirs.empty())
    {
        auto d = dirs.rbegin();
        if (d->IsSameAs("LC_MESSAGES", /*caseSensitive=*/false))
        {
            if (++d == dirs.rend())
                return Language(); // failed to match
        }
        wxString rest;
        if (d->EndsWith(".lproj", &rest))
            return Language::TryParseWithValidation(rest.ToStdWstring());
        else
            return Language::TryParseWithValidation(d->ToStdWstring());
    }

    return Language(); // failed to match
}


Language Language::TryDetectFromText(const char *buffer, size_t len, Language probableLanguage)
{
#ifdef HAVE_CLD2
    using namespace CLD2;

    CLDHints hints = {NULL, NULL, UNKNOWN_ENCODING, UNKNOWN_LANGUAGE};
    if (probableLanguage.IsValid())
    {
        if (probableLanguage.Lang() == "en")
            hints.language_hint = ENGLISH;
        else
            hints.language_hint = GetLanguageFromName(probableLanguage.LanguageTag().c_str());
    }

    // three best guesses; we don't care, but they must be passed in
    CLD2::Language language3[3];
    int percent3[3];
    double normalized_score3[3];
    // more result info:
    int text_bytes;
    bool is_reliable;

    auto lang = CLD2::ExtDetectLanguageSummary(
                        buffer, (int)len,
                        /*is_plain_text=*/false,
                        &hints,
                        /*flags=*/0,
                        language3, percent3, normalized_score3,
                        /*resultchunkvector=*/nullptr,
                        &text_bytes,
                        &is_reliable);

    if (lang == UNKNOWN_LANGUAGE || !is_reliable)
        return Language();

    // CLD2 penalizes English in bilingual content in some cases as "boilerplate"
    // because it is tailored for the web. So e.g. 66% English, 33% Italian is
    // tagged as Italian.
    //
    // Poedit's bias is the opposite: English is almost always the correct answer
    // for PO source language. Fix this up manually.
    if (lang != language3[0] && language3[0] == CLD2::ENGLISH && language3[1] == lang)
        lang = language3[0];

    return Language::TryParse(LanguageCode(lang));
#else
    (void)buffer;
    (void)len;
    return probableLanguage;
#endif
}


PluralFormsExpr::PluralFormsExpr() : m_calcCreated(true)
{
}

PluralFormsExpr::PluralFormsExpr(const std::string& expr, int nplurals)
    : m_expr(expr), m_nplurals(nplurals), m_calcCreated(false)
{
}

PluralFormsExpr::~PluralFormsExpr()
{
}

int PluralFormsExpr::nplurals() const
{
    if (m_nplurals != -1)
        return m_nplurals;
    if (m_calc)
        return m_calc->nplurals();

    const regex re("^nplurals=([0-9]+)");
    smatch m;
    if (regex_match(m_expr, m, re))
        return std::stoi(m.str(1));
    else
        return -1;
}

std::shared_ptr<PluralFormsCalculator> PluralFormsExpr::calc() const
{
    auto self = const_cast<PluralFormsExpr*>(this);
    if (m_calcCreated)
        return m_calc;
    if (!m_expr.empty())
        self->m_calc = PluralFormsCalculator::make(m_expr.c_str());
    self->m_calcCreated = true;
    return m_calc;
}

bool PluralFormsExpr::operator==(const PluralFormsExpr& other) const
{
    if (m_expr == other.m_expr)
        return true;

    // do some normalization to avoid unnecessary complains when the only
    // differences are in whitespace for example:
    auto expr1 = boost::erase_all_copy(m_expr, " \t");
    auto expr2 = boost::erase_all_copy(other.m_expr, " \t");
    if (expr1 == expr2)
        return true;

    // failing that, compare the expressions semantically:
    auto calc1 = calc();
    auto calc2 = other.calc();

    if (!calc1 || !calc2)
        return false; // at least one is invalid _and_ the strings are different due to code above

    if (calc1->nplurals() != calc2->nplurals())
        return false;

    for (int i = 0; i < MAX_EXAMPLES_COUNT; i++)
    {
        if (calc1->evaluate(i) != calc2->evaluate(i))
            return false;
    }
    // both expressions are identical on all tested integers
    return true;
}

int PluralFormsExpr::evaluate_for_n(int n) const
{
    auto c = calc();
    return c ? c->evaluate(n) : 0;
}
