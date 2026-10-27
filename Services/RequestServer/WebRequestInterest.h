/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibRequests/WebRequest.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace RequestServer {

// The requests extensions' webRequest listeners want to hear about, as the UI process last sent them. With none, no
// request is reported, so a page load with no listening extension takes the same path as with none installed.
class WebRequestInterestSet {
public:
    static WebRequestInterestSet& the();

    void set(Vector<Requests::WebRequestInterest>);
    bool is_empty() const { return m_entries.is_empty(); }

    [[nodiscard]] bool should_report(Requests::WebRequestEvent, URL::URL const&, StringView type) const;
    // Whether a listener that may change the request hears it, so the request waits for the answer.
    [[nodiscard]] bool wants_answer(Requests::WebRequestEvent, URL::URL const&, StringView type) const;

private:
    struct Entry {
        Requests::WebRequestEvent event;
        Vector<Web::Extensions::MatchPattern> urls;
        Vector<String> types;
        bool blocking { false };
    };

    Entry const* find(Requests::WebRequestEvent, URL::URL const&, StringView type, bool blocking_only) const;

    Vector<Entry> m_entries;
};

}
