﻿/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2017 Vaclav Slavik
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

#include "qa_checks.h"

#include <unicode/uchar.h>

#include <wx/translation.h>


// -------------------------------------------------------------
// QACheck implementations
// -------------------------------------------------------------

namespace QA
{

class NotAllPlurals : public QACheck
{
public:
    bool CheckItem(CatalogItemPtr item) override
    {
        if (!item->HasPlural())
            return false;

        bool foundTranslated = false;
        bool foundEmpty = false;
        for (auto& s: item->GetTranslations())
        {
            if (s.empty())
                foundEmpty = true;
            else
                foundTranslated = true;
        }

        if (foundEmpty && foundTranslated)
        {
            item->SetIssue(CatalogItem::Issue::Warning, _("Not all plural forms are translated."));
            return true;
        }

        return false;
    }
};


class CaseMismatch : public QACheck
{
public:
    bool CheckString(CatalogItemPtr item, const wxString& source, const wxString& translation) override
    {
        if (u_isupper(source[0]) && u_islower(translation[0]))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _("The translation should start as a sentence."));
            return true;
        }

        if (u_islower(source[0]) && u_isupper(translation[0]))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _("The translation should start with a lowercase character."));
            return true;
        }

        return false;
    }
};


class WhitespaceMismatch : public QACheck
{
public:
    bool CheckString(CatalogItemPtr item, const wxString& source, const wxString& translation) override
    {
        if (u_isspace(source[0]) && !u_isspace(translation[0]))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation doesn’t start with a space."));
            return true;
        }

        if (!u_isspace(source[0]) && u_isspace(translation[0]))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation starts with a space, but the source text doesn’t."));
            return true;
        }

        if (source.Last() == '\n' && translation.Last() != '\n')
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation is missing a newline at the end."));
            return true;
        }

        if (source.Last() != '\n' && translation.Last() == '\n')
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation ends with a newline, but the source text doesn’t."));
            return true;
        }

        if (u_isspace(source.Last()) && !u_isspace(translation.Last()))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation is missing a space at the end."));
            return true;
        }

        if (!u_isspace(source.Last()) && u_isspace(translation.Last()))
        {
            item->SetIssue(CatalogItem::Issue::Warning, _(L"The translation ends with a space, but the source text doesn’t."));
            return true;
        }

        return false;
    }
};


class PunctuationMismatch : public QACheck
{
public:
    bool CheckString(CatalogItemPtr item, const wxString& source, const wxString& translation) override
    {
        const UChar32 s_last = source.Last();
        const UChar32 t_last = translation.Last();
        const bool s_punct = u_ispunct(s_last);
        const bool t_punct = u_ispunct(t_last);

        if (s_punct && !t_punct)
        {
            item->SetIssue(CatalogItem::Issue::Warning,
                           wxString::Format(_(L"The translation should end with “%s”."), wxString(wxUniChar(s_last))));
            return true;
        }
        else if (!s_punct && t_punct)
        {
            item->SetIssue(CatalogItem::Issue::Warning,
                           wxString::Format(_(L"The translation should not end with “%s”."), wxString(wxUniChar(t_last))));
            return true;
        }
        else if (s_punct && t_punct && s_last != t_last)
        {
            if (t_last == L'…' && source.EndsWith("..."))
            {
                // as a special case, allow translating ... (3 dots) as … (ellipsis)
            }
            else
            {
                item->SetIssue(CatalogItem::Issue::Warning,
                               wxString::Format(_(L"The translation ends with “%s”, but the source text ends with “%s”."),
                                                wxString(wxUniChar(t_last)), wxString(wxUniChar(s_last))));
                return true;
            }
        }

        return false;
    }
};


} // namespace QA


// -------------------------------------------------------------
// QACheck support code
// -------------------------------------------------------------

bool QACheck::CheckItem(CatalogItemPtr item)
{
    if (!item->GetTranslation().empty() && CheckString(item, item->GetString(), item->GetTranslation()))
        return true;

    if (item->HasPlural())
    {
        unsigned count = item->GetNumberOfTranslations();
        for (unsigned i = 1; i < count; i++)
        {
            auto t = item->GetTranslation(i);
            if (!t.empty() && CheckString(item, item->GetPluralString(), t))
                return true;
        }
    }

    return false;
}


bool QACheck::CheckString(CatalogItemPtr /*item*/, const wxString& /*source*/, const wxString& /*translation*/)
{
    wxFAIL_MSG("not implemented - must override CheckString OR CheckItem");
    return false;
}


// -------------------------------------------------------------
// QAChecker
// -------------------------------------------------------------

int QAChecker::Check(Catalog& catalog)
{
    // TODO: parallelize this (make async tasks for chunks of the catalog)
    //       doing it per-checker is MT-unsafe with API that calls SetIssue()!

    int issues = 0;

    for (auto& i: catalog.items())
        issues += Check(i);

    return issues;
}


int QAChecker::Check(CatalogItemPtr item)
{
    int issues = 0;

    for (auto& c: m_checks)
    {
        if (item->GetString().empty() || (item->HasPlural() && item->GetPluralString().empty()))
            continue;

        if (c->CheckItem(item))
            issues++;
    }

    return issues;
}


std::shared_ptr<QAChecker> QAChecker::GetFor(Catalog& /*catalog*/)
{
    auto c = std::make_shared<QAChecker>();
    c->AddCheck<QA::NotAllPlurals>();
    c->AddCheck<QA::CaseMismatch>();
    c->AddCheck<QA::WhitespaceMismatch>();
    c->AddCheck<QA::PunctuationMismatch>();
    return c;
}
