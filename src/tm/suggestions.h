/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2014-2021 Vaclav Slavik
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

#ifndef Poedit_suggestions_h
#define Poedit_suggestions_h

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "concurrency.h"
#include "language.h"

class SuggestionsBackend;
class SuggestionsProviderImpl;

/// A query for suggestions
struct SuggestionQuery
{
    /// Language of the source text.
    Language srclang;
    /// Language of the desired translation.
    Language lang;
    /// Source text.
    std::wstring source;
};


/// A single translation suggestion
struct Suggestion
{
    /// Possible types of suggestion sources
    enum class Source
    {
        LocalTM
    };

    /// Ctor
    Suggestion() : score(0.), localScore(0), source(Source::LocalTM) {}
    Suggestion(const std::wstring& text_,
               double score_,
               int localScore_ = 0,
               Source source_ = Source::LocalTM)
        : text(text_), score(score_), localScore(localScore_), source(source_)
    {}

    /// Text of the suggested translation
    std::wstring text;

    /// Quality score (1.0 = exact match, 0 = no score assigned)
    double score;

    /// Time when the suggestion was stored
    int localScore;

    /// Source of the suggestion
    Source source;

    /// Optional ID of the suggestion, for use with up/downvoting
    std::string id;

    bool HasScore() const { return score != 0.0; }
    bool IsExactMatch() const { return score == 1.0; }
};

inline bool operator<(const Suggestion& a, const Suggestion& b)
{
    if (std::fabs(a.score - b.score) <= std::numeric_limits<double>::epsilon())
        return a.localScore > b.localScore;
    else
        return a.score > b.score;
}

typedef std::vector<Suggestion> SuggestionsList;

/**
    Provides suggestions for translations.

    Under the hood, the translation memory is used, but the API is more
    generic and allows for other implementations.
    
    This is a relatively lightweight object and shouldn't be shared between
    users (e.g. opened documents/windows) -- create one instance per user.
 */
class SuggestionsProvider
{
public:
    /// Initializes the provider.
    SuggestionsProvider();
    ~SuggestionsProvider();

    /**
        Query for suggested translations.
        
        This function asynchronously calls either @a onSuccess or @a onError
        callback, exactly once, from a worked thread.

        If no suggestions are found, @a onSuccess is called with an empty
        list as its argument.

        @param backend    Suggestions backend to use, e.g. TranslationMemory::Get().
        @param q          Source text and its metadata.
     */
    dispatch::future<SuggestionsList> SuggestTranslation(SuggestionsBackend& backend, const SuggestionQuery&& q);

    /// Mark a suggestion as good. Called when a suggestion is used.
    static void Delete(const Suggestion& s);

private:
    std::unique_ptr<SuggestionsProviderImpl> m_impl;
};



/**
    Implements a source of suggestions for SuggestionsProvider.

    This class is abstraction that doesn't depend on a specific source
    (such as the translation memory DB).
    
    @note Implementations must be thread-safe!
 */
class SuggestionsBackend
{
public:
    virtual ~SuggestionsBackend() {}

    /**
        Query for suggested translations.
        
        This function asynchronously calls either @a onSuccess or @a onError
        callback, exactly once.
        
        @note No guarantees are made about the thread the callbacks are called
              from; they may be called immediately before SuggestTranslation()
              returns (even on the same thread) or at a later time. This is
              a difference from SuggestionsProvider, which guarantees that
              the callback is called asynchronously from another thread.

        If no suggestions are found, @a onSuccess is called with an empty
        list as its argument.
        
        @param q     Source text and its metadata.
     */
    virtual dispatch::future<SuggestionsList> SuggestTranslation(const SuggestionQuery&& q) = 0;

    /// Delete suggestion with given ID from the database
    virtual void Delete(const std::string& id) = 0;
};

#endif // Poedit_suggestions_h
