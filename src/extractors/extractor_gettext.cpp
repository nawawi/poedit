/*
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

#include "extractor.h"

#include "gexecute.h"

#include <wx/textfile.h>

namespace
{

// This list is synced with EXTENSIONS_* macros in deps/gettext/gettext-tools/src/x-*.h files:
const char * const GETTEXT_EXTENSIONS[] = {
    "appdata.xml",                                        // appdata - ITS

    "awk", "gawk", "twjr",                                // awk

    "c", "h",                                             // C
    "C", "c++", "cc", "cxx", "cpp", "hh", "hxx", "hpp",   // C++
    "m",                                                  // ObjectiveC
    // FIXME: handling of .h files as C++? (req. separate pass)
    // FIXME: .mm files for Objective-C++ (add gettext-tools support first)

    "cs",                                                 // C#

    "desktop",                                            // Desktop

    "el",                                                 // EmacsLisp

    "glade", "glade2", "ui",                              // glade - ITS

    "gschema.xml",                                        // GSettings - ITS

    "java",                                               // Java

    "js",                                                 // JavaScript

    "jl",                                                 // librep

    "lisp",                                               // Lisp

    "lua",                                                // Lua

    "pl", "PL", "pm", "perl", /*"cgi" - too generic,*/    // perl

    "php", "php3", "php4",                                // PHP
    // NOTE: .phtml shouldn't be used by modern PHP, but maybe it is?

    "py",                                                 // Python

    // "rst",                                             // RST
    // NOTE: conflicts with restructured text, dangerous

    "scm",                                                // Scheme

    // "sh", "bash",                                      // Shell
    // NOTE: disabled in Poedit, rarely if ever used

    "st",                                                 // Smalltalk

    "tcl",                                                // Tcl

    "vala",                                               // Vala

    "ycp",                                                // YCP

    nullptr
};


} // anonymous namespace


/// Extractor implementation for standard GNU gettext
class GettextExtractor : public Extractor
{
public:
    GettextExtractor()
    {
        for (const char * const *e = GETTEXT_EXTENSIONS; *e != nullptr; e++)
            RegisterExtension(*e);
    }

    wxString GetId() const override { return "gettext"; }

    wxString Extract(TempDirectory& tmpdir,
                     const SourceCodeSpec& sourceSpec,
                     const std::vector<wxString>& files) const override
    {
        auto basepath = sourceSpec.BasePath;
#ifdef __WXMSW__
        basepath.Replace("\\", "/");
#endif

        wxTextFile filelist;
        filelist.Create(tmpdir.CreateFileName("gettext_filelist.txt"));
        for (auto fn: files)
        {
#ifdef __WXMSW__
            // Gettext tools can't handle Unicode filenames well (due to using
            // char* arguments), so work around this by using the short names.
            if (!fn.IsAscii())
            {
                fn = wxFileName(fn).GetShortPath();
                fn.Replace("\\", "/");
            }
#endif
            filelist.AddLine(fn);
        }
        filelist.Write(wxTextFileType_Unix, wxConvFile);

        auto outfile = tmpdir.CreateFileName("gettext.pot");

        wxString cmdline;
        cmdline.Printf
        (
            "xgettext --force-po -o %s --directory=%s --files-from=%s --from-code=%s",
            QuoteCmdlineArg(outfile),
            QuoteCmdlineArg(basepath),
            QuoteCmdlineArg(filelist.GetName()),
            QuoteCmdlineArg(!sourceSpec.Charset.empty() ? sourceSpec.Charset : "UTF-8")
        );

        for (auto& kw: sourceSpec.Keywords)
        {
            cmdline += wxString::Format(" -k%s", QuoteCmdlineArg(kw));
        }

        wxString extraFlags;
        try
        {
            extraFlags = sourceSpec.XHeaders.at("X-Poedit-Flags-xgettext");
        }
        catch (std::out_of_range) {}

        if (!extraFlags.Contains("--add-comments"))
            cmdline += " --add-comments=TRANSLATORS:";

        if (!extraFlags.empty())
            cmdline += " " + extraFlags;

        if (!ExecuteGettext(cmdline))
            return "";

        return outfile;
    }
};


std::shared_ptr<Extractor> Extractor::CreateGettextExtractor()
{
    return std::make_shared<GettextExtractor>();
}
