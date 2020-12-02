/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2015-2020 Vaclav Slavik
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

#include "customcontrols.h"

#include "concurrency.h"
#include "errors.h"
#include "hidpi.h"
#include "utility.h"

#include <wx/app.h>
#include <wx/artprov.h>
#include <wx/clipbrd.h>
#include <wx/dcmemory.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#if wxCHECK_VERSION(3,1,0)
    #include <wx/activityindicator.h>
#else
    #include "wx_backports/activityindicator.h"
#endif

#include <unicode/brkiter.h>
#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include "str_helpers.h"
#include "unicode_helpers.h"

#include <map>
#include <memory>

namespace
{

wxString WrapTextAtWidth(const wxString& text_, int width, Language lang, wxWindow *wnd)
{
    if (text_.empty())
        return text_;

#ifdef BIDI_NEEDS_DIRECTION_ON_EACH_LINE
    wchar_t directionMark = 0;
    if (bidi::is_direction_mark(*text_.begin()))
        directionMark = *text_.begin();
#endif
        
    auto text = str::to_icu(text_);

    static std::map<std::string, std::shared_ptr<icu::BreakIterator>> lang_iters;
    std::shared_ptr<icu::BreakIterator> iter;
    auto lang_name = lang.IcuLocaleName();
    auto li = lang_iters.find(lang_name);
    if (li == lang_iters.end())
    {
        UErrorCode err = U_ZERO_ERROR;
        iter.reset(icu::BreakIterator::createLineInstance(lang.IsValid() ? lang.ToIcu() : icu::Locale(), err));
        if (!iter)
            iter.reset(icu::BreakIterator::createLineInstance(icu::Locale::getEnglish(), err));
        lang_iters[lang_name] = iter;
    }
    else
    {
        iter = li->second;
    }

    iter->setText(text);

    wxString out;
    out.reserve(text_.length() + 10);

    int32_t lineStart = 0;
    wxString previousSubstr;

    for (int32_t pos = iter->next(); pos != icu::BreakIterator::DONE; pos = iter->next())
    {
        auto substr = str::to_wx(text.tempSubStringBetween(lineStart, pos));

        if (wnd->GetTextExtent(substr).x > width)
        {
            auto previousPos = iter->previous();
            if (previousPos == lineStart || previousPos == icu::BreakIterator::DONE)
            {
                // line is too large but we can't break it, so have no choice but not to wrap
                out += substr;
                lineStart = pos;
            }
            else
            {
                // need to wrap at previous linebreak position
                out += previousSubstr;
                lineStart = previousPos;
            }

            out += '\n';
#ifdef BIDI_NEEDS_DIRECTION_ON_EACH_LINE
            if (directionMark)
                out += directionMark;
#endif

            previousSubstr.clear();
        }
        else if (pos > 0 && text[pos-1] == '\n') // forced line feed
        {
            out += substr;
            lineStart = pos;
            previousSubstr.clear();
        }
        else
        {
            previousSubstr = substr;
        }
    }

    if (!previousSubstr.empty())
    {
        out += previousSubstr;
    }

    if (out.Last() == '\n')
        out.RemoveLast();

    return out;
}


} // anonymous namespace


HeadingLabel::HeadingLabel(wxWindow *parent, const wxString& label)
    : wxStaticText(parent, wxID_ANY, label)
{
#ifdef __WXGTK3__
    // This is needed to avoid missizing text with bold font. See
    // https://github.com/vslavik/poedit/pull/411 and https://trac.wxwidgets.org/ticket/16088
    SetLabelMarkup("<b>" + EscapeMarkup(label) + "</b>");
#else
    SetFont(GetFont().Bold());
#endif
}


AutoWrappingText::AutoWrappingText(wxWindow *parent, const wxString& label)
    : wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE),
      m_text(label),
      m_wrapWidth(-1)
{
    m_text.Replace("\n", " ");

    SetInitialSize(wxSize(10,10));
    Bind(wxEVT_SIZE, &AutoWrappingText::OnSize, this);
}

void AutoWrappingText::SetLanguage(Language lang)
{
    m_language = lang;
    SetAlignment(m_language.Direction());
}

void AutoWrappingText::SetAlignment(TextDirection dir)
{
    // a quirk of wx API: if the current locale is RTL, the meaning of L and R is reversed
    // for alignments
    bool isRTL = (dir == TextDirection::RTL);
    if (GetLayoutDirection() == wxLayout_RightToLeft)
        isRTL = !isRTL;

    const int align = isRTL ? wxALIGN_RIGHT : wxALIGN_LEFT;
    if (HasFlag(align))
        return;
    SetWindowStyleFlag(wxST_NO_AUTORESIZE | align);
}

void AutoWrappingText::SetAndWrapLabel(const wxString& label)
{
    m_text = bidi::platform_mark_direction(label);
    if (!m_language.IsValid())
        SetAlignment(bidi::get_base_direction(m_text));

    wxWindowUpdateLocker lock(this);
    m_wrapWidth = GetSize().x;
    SetLabelText(WrapTextAtWidth(label, m_wrapWidth, m_language, this));

    InvalidateBestSize();
    SetMinSize(wxDefaultSize);
    SetMinSize(GetBestSize());
}

void AutoWrappingText::OnSize(wxSizeEvent& e)
{
    e.Skip();
    int w = wxMax(0, e.GetSize().x - PX(4));
    if (w == m_wrapWidth)
        return;

    // refuse to participate in crazy-small sizes sizing (will be undone anyway):
    if (w < 50)
        return;

    wxWindowUpdateLocker lock(this);

    m_wrapWidth = w;
    SetLabel(WrapTextAtWidth(m_text, w, m_language, this));

    InvalidateBestSize();
    SetMinSize(wxDefaultSize);
    SetMinSize(GetBestSize());
}


SelectableAutoWrappingText::SelectableAutoWrappingText(wxWindow *parent, const wxString& label)
    : AutoWrappingText(parent, label)
{
#if defined(__WXOSX__)
    NSTextField *view = (NSTextField*)GetHandle();
    [view setSelectable:YES];
#elif defined(__WXGTK__)
    GtkLabel *view = GTK_LABEL(GetHandle());
    gtk_label_set_selectable(view, TRUE);
#else
    // at least allow copying
    static wxWindowID idCopy = wxNewId();
    Bind(wxEVT_CONTEXT_MENU, [=](wxContextMenuEvent&){
        wxMenu *menu = new wxMenu();
        menu->Append(idCopy, _("&Copy"));
        PopupMenu(menu);
    });
    Bind(wxEVT_MENU, [=](wxCommandEvent&){
        wxClipboardLocker lock;
        wxClipboard::Get()->SetData(new wxTextDataObject(m_text));
    }, idCopy);
#endif
}


ExplanationLabel::ExplanationLabel(wxWindow *parent, const wxString& label)
    : AutoWrappingText(parent, label)
{
#if defined(__WXOSX__)
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#elif defined(__WXGTK__)
    #if wxCHECK_VERSION(3,1,0)
        SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    #else
        SetFont(GetFont().Smaller());
    #endif
#endif
#ifndef __WXGTK__
    ColorScheme::SetupWindowColors(this, [=]
    {
        SetForegroundColour(GetTextColor());
    });
#endif
}


SecondaryLabel::SecondaryLabel(wxWindow *parent, const wxString& label)
    : wxStaticText(parent, wxID_ANY, label)
{
#if defined(__WXOSX__)
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#elif defined(__WXGTK__)
    #if wxCHECK_VERSION(3,1,0)
        SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    #else
        SetFont(GetFont().Smaller());
    #endif
#endif
#ifndef __WXGTK__
    ColorScheme::SetupWindowColors(this, [=]
    {
        SetForegroundColour(GetTextColor());
    });
#endif
}


LearnMoreLink::LearnMoreLink(wxWindow *parent, const wxString& url, wxString label, wxWindowID winid)
{
    if (label.empty())
    {
#ifdef __WXMSW__
        label = _("Learn more");
#else
        label = _("Learn More");
#endif
    }

    wxHyperlinkCtrl::Create(parent, winid, label, url);

#ifdef __WXOSX__
    if (@available(macOS 10.14, *))
    {
        wxColour normal(NSColor.linkColor);
        wxColour hover([NSColor.linkColor colorWithSystemEffect:NSColorSystemEffectRollover]);
        SetNormalColour(normal);
        SetVisitedColour(normal);
        SetHoverColour(hover);
    }
    else
#endif
    {
        SetNormalColour("#2F79BE");
        SetVisitedColour("#2F79BE");
        SetHoverColour("#3D8DD5");
    }

#ifdef __WXOSX__
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    SetFont(GetFont().Underlined());
#endif
}


wxObject *LearnMoreLinkXmlHandler::DoCreateResource()
{
    auto w = new LearnMoreLink(m_parentAsWindow, GetText("url"), GetText("label"), GetID());
    w->SetName(GetName());
    SetupWindow(w);
    return w;
}

bool LearnMoreLinkXmlHandler::CanHandle(wxXmlNode *node)
{
    return IsOfClass(node, "LearnMoreLink");
}


ActivityIndicator::ActivityIndicator(wxWindow *parent, int flags)
    : wxWindow(parent, wxID_ANY), m_running(false)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    SetSizer(sizer);

    m_spinner = new wxActivityIndicator(this, wxID_ANY);
    m_spinner->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_label = new wxStaticText(this, wxID_ANY, "");
#ifdef __WXOSX__
    m_label->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif

    if (flags & Centered)
        sizer->AddStretchSpacer();
    sizer->Add(m_spinner, wxSizerFlags().Center().Border(wxRIGHT, PX(4)));
    sizer->Add(m_label, wxSizerFlags(1).Center());
    if (flags & Centered)
        sizer->AddStretchSpacer();

    wxWeakRef<ActivityIndicator> self(this);
    HandleError = [self](dispatch::exception_ptr e){
        dispatch::on_main([self,e]{
            if (self)
                self->StopWithError(DescribeException(e));
        });
    };
}


void ActivityIndicator::UpdateLayoutAfterTextChange()
{
    m_label->Wrap(GetSize().x);

    Layout();

    if (GetSizer()->IsShown(m_label))
    {
        InvalidateBestSize();
        SetMinSize(wxDefaultSize);
        SetMinSize(GetBestSize());

        GetParent()->Layout();
    }
}


void ActivityIndicator::Start(const wxString& msg)
{
    m_running = true;

    m_label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    m_label->SetLabel(msg);

    auto sizer = GetSizer();
    sizer->Show(m_spinner);
    sizer->Show(m_label, !msg.empty());

    UpdateLayoutAfterTextChange();

    m_spinner->Start();
}

void ActivityIndicator::Stop()
{
    m_running = false;

    m_spinner->Stop();
    m_label->SetLabel("");

    auto sizer = GetSizer();
    sizer->Hide(m_spinner);
    sizer->Hide(m_label);

    UpdateLayoutAfterTextChange();
}

void ActivityIndicator::StopWithError(const wxString& msg)
{
    m_running = false;

    m_spinner->Stop();
    m_label->SetForegroundColour(ColorScheme::Get(Color::ErrorText));
    m_label->SetLabel(msg);
    m_label->SetToolTip(msg);

    auto sizer = GetSizer();
    sizer->Hide(m_spinner);
    sizer->Show(m_label);

    UpdateLayoutAfterTextChange();
}



ImageButton::ImageButton(wxWindow *parent, const wxString& bitmapName)
    : wxBitmapButton(parent, wxID_ANY,
                     bitmapName.empty() ? wxNullBitmap : wxArtProvider::GetBitmap(bitmapName),
                     wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT),
      m_bitmapName(bitmapName)
{
#ifdef __WXOSX__
    // don't light up the background when clicked:
    NSButton *view = (NSButton*)GetHandle();
    view.buttonType = NSMomentaryChangeButton;
#else
    // refresh template icons on theme change (macOS handles automatically):
    if (bitmapName.EndsWith("Template"))
    {
        ColorScheme::SetupWindowColors(this, [=]
        {
            SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
        });
    }
#endif
}


StaticBitmap::StaticBitmap(wxWindow *parent, const wxString& bitmapName)
    : wxStaticBitmap(parent, wxID_ANY,
                     bitmapName.empty() ? wxNullBitmap : wxArtProvider::GetBitmap(bitmapName)),
      m_bitmapName(bitmapName)
{
#ifndef __WXOSX__
    // refresh template icons on theme change (macOS handles automatically):
    if (bitmapName.EndsWith("Template"))
    {
        ColorScheme::SetupWindowColors(this, [=]
        {
            SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
        });
    }
#endif
}

void StaticBitmap::SetBitmapName(const wxString& bitmapName)
{
    m_bitmapName = bitmapName;
    SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
}


AvatarIcon::AvatarIcon(wxWindow *parent, const wxSize& size) : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
{
    InitForSize();
    ColorScheme::RefreshOnChange(this);

    Bind(wxEVT_PAINT, &AvatarIcon::OnPaint, this);
}

void AvatarIcon::SetUserName(const wxString& name)
{
    m_placeholder.clear();
    for (auto& s: wxSplit(name, ' '))
    {
        if (!s.empty())
            m_placeholder += s[0];
    }
    Refresh();
}

void AvatarIcon::LoadIcon(const wxFileName& f)
{
#ifdef __WXOSX__
    NSString *path = str::to_NS(f.GetFullPath());
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:path];
    if (img != nil)
        m_bitmap = wxBitmap(img);
#else
    wxLogNull null;
    wxImage img(f.GetFullPath());
    if (img.IsOk())
        m_bitmap = wxBitmap(img);
#endif

    Refresh();
}

void AvatarIcon::InitForSize()
{
    auto size = GetSize();
    wxBitmap bmp(size);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetBrush(*wxBLACK_BRUSH);
    dc.SetPen(*wxBLACK_PEN);
    wxRect r(wxPoint(0,0), size);
    r.Deflate(PX(3));
    dc.DrawEllipse(r);
    dc.SelectObject(wxNullBitmap);
    m_clipping = wxRegion(bmp, *wxWHITE);

    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetWeight(wxFONTWEIGHT_BOLD);
    font.SetPixelSize(wxSize(0, size.y / 4));
    SetFont(font);
}

void AvatarIcon::OnPaint(wxPaintEvent&)
{
    auto r = GetClientRect();
    r.Deflate(PX(2));

    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetInterpolationQuality(wxINTERPOLATION_BEST);

    gc->Clip(m_clipping);

    if (m_bitmap.IsOk())
    {
        gc->DrawBitmap(m_bitmap, r.x, r.y, r.width, r.height);
    }
    else
    {
        gc->SetBrush(wxColour(128,128,128,50));
        gc->SetPen(wxNullPen);
        gc->SetFont(GetFont(), ColorScheme::Get(Color::SecondaryLabel));

        gc->DrawEllipse(r.x, r.y, r.width, r.height);

        wxDouble tw, th;
        gc->GetTextExtent(m_placeholder, &tw, &th);
        gc->DrawText(m_placeholder, r.x + (r.width - tw) / 2, r.y + (r.height - th) / 2);
    }

    gc->ResetClip();

    // mark out jagged, pixelated clipping due to low-resolution wxRegion:
    auto outline = GetBackgroundColour();
    outline = outline.ChangeLightness(ColorScheme::GetAppMode() == ColorScheme::Light ? 98 : 110);
    gc->SetPen(wxPen(outline, PX(2)));
    gc->DrawEllipse(r.x + 0.5, r.y + 0.5, r.width, r.height);
}
