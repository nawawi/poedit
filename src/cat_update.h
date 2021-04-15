/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2000-2021 Vaclav Slavik
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

#ifndef Poedit_cat_update_h
#define Poedit_cat_update_h

#include "catalog_po.h"

class WXDLLIMPEXP_FWD_CORE wxWindow;


/// Result of PerformUpdateFromSources()
struct UpdateResultReason
{
    enum Code
    {
        CancelledByUser,
        Unspecified,
        NoSourcesFound,
        PermissionDenied
    };

    UpdateResultReason(Code c = Unspecified) : code(c) {}

    Code code;
    wxString file;
};

enum UpdateFlags
{
    Update_DontShowSummary = 1
};

/**
    Update catalog from source code, if configured, and provide UI
    during the operation.
 */
bool PerformUpdateFromSources(POCatalogPtr catalog, UpdateResultReason& reason);

bool PerformUpdateFromSourcesWithUI(wxWindow *parent,
                                    POCatalogPtr catalog,
                                    UpdateResultReason& reason,
                                    int flags = 0);

/**
    Similarly for updating from a POT file.
 */
bool PerformUpdateFromPOTWithUI(wxWindow *parent,
                                POCatalogPtr catalog,
                                const wxString& pot_file,
                                UpdateResultReason& reason);


#endif // Poedit_cat_update_h
