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

#include "http_client.h"

#include "version.h"
#include "str_helpers.h"

#include <cstdlib>

#include <boost/algorithm/string/predicate.hpp>

#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>

#include <boost/throw_exception.hpp>

#include <cpprest/asyncrt_utils.h>
#include <cpprest/http_client.h>
#include <cpprest/http_msg.h>
#include <cpprest/filestream.h>

#include <regex>


#ifdef _WIN32
    #include <windows.h>
    #include <winhttp.h>
    #include <netlistmgr.h>
    #pragma comment(lib, "ole32.lib")

    // can't include both winhttp.h and wininet.h, so put a declaration here
    //#include <wininet.h>
    EXTERN_C DECLSPEC_IMPORT BOOL STDAPICALLTYPE InternetGetConnectedState(__out LPDWORD lpdwFlags, __reserved DWORD dwReserved);

    #ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
        #define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
    #endif
    #ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
        #define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
    #endif
#endif

namespace
{

using namespace web;
using utility::string_t;
using utility::conversions::to_string_t;

#ifdef _UTF16_STRINGS
inline string_t to_string_t(const wxString& s) { return s.ToStdWstring(); }
#else
inline string_t to_string_t(const wxString& s) { return s.ToStdString(); }
inline string_t to_string_t(const std::wstring& s) { return str::to_utf8(s); }
#endif

class gzip_compression_support : public http::http_pipeline_stage
{
public:
    pplx::task<http::http_response> propagate(http::http_request request) override
    {
        request.headers().add(http::header_names::accept_encoding, _XPLATSTR("gzip"));

        return next_stage()->propagate(request).then([](http::http_response response) -> pplx::task<http::http_response>
        {
            string_t encoding;
            if (response.headers().match(http::header_names::content_encoding, encoding) && encoding == _XPLATSTR("gzip"))
            {
                return response.extract_vector().then([response](std::vector<unsigned char> compressed) mutable -> http::http_response
                {
                    namespace io = boost::iostreams;

                    io::array_source source(reinterpret_cast<char*>(compressed.data()), compressed.size());
                    io::filtering_istream in;
                    in.push(io::gzip_decompressor());
                    in.push(source);

                    std::vector<char> decompressed;
                    io::back_insert_device<std::vector<char>> sink(decompressed);
                    io::copy(in, sink);

                    response.set_body(concurrency::streams::bytestream::open_istream(std::move(decompressed)), decompressed.size());
                    return response;
                });
            }
            else
            {
                return pplx::task_from_result(response);
            }
        });
    }
};

} // anonymous namespace


class http_client::impl
{
public:
    impl(http_client& owner, const std::string& url_prefix, int flags)
        : m_owner(owner),
          m_native(sanitize_url(url_prefix, flags), get_client_config())
    {
        #define make_wide_str(x) make_wide_str_(x)
        #define make_wide_str_(x) L ## x

        #if defined(_WIN32)
            #define USER_AGENT_PLATFORM L" (Windows NT " + windows_version() + L")"
        #elif defined(__unix__)
            #define USER_AGENT_PLATFORM L" (Unix)"
        #else
            #define USER_AGENT_PLATFORM
        #endif
        m_userAgent = L"Poedit/" make_wide_str(POEDIT_VERSION) USER_AGENT_PLATFORM;

        std::shared_ptr<http::http_pipeline_stage> gzip_stage = std::make_shared<gzip_compression_support>();
        m_native.add_handler(gzip_stage);
    }

    static string_t ui_language;

    void set_authorization(const std::string& auth)
    {
        m_auth = std::wstring(auth.begin(), auth.end());
    }

    dispatch::future<::json> get(const std::string& url, const headers& hdrs)
    {
        auto req = build_request(http::methods::GET, url, hdrs);

        return
        m_native.request(req)
        .then([=](http::http_response response)
        {
            handle_error(response);
            return ::json::parse(response.extract_utf8string().get());
        });
    }

    dispatch::future<downloaded_file> download(const std::string& url, const headers& hdrs)
    {
        using namespace concurrency::streams;

        auto req = build_request(http::methods::GET, url, hdrs);

        return
        m_native.request(req)
        .then([=](http::http_response response)
        {
            handle_error(response);

            downloaded_file file(extract_attachment_filename(req, response));

            return
            fstream::open_ostream(to_string_t(file.filename().GetFullPath()))
            .then([=](ostream outFile)
            {
                return
                response.body().read_to_end(outFile.streambuf())
                .then([=](size_t)
                {
                    return outFile.close();
                });
            })
            .then([file{std::move(file)}]()
            {
                return file;
            });
        });
    }

    dispatch::future<::json> post(const std::string& url, const http_body_data& data, const headers& hdrs)
    {
        auto req = build_request(http::methods::POST, url, hdrs);

        auto body = data.body();
        req.set_body(body, data.content_type());
        req.headers().set_content_length(body.size());

        return
        m_native.request(req)
        .then([=](http::http_response response)
        {
            handle_error(response);
            return ::json::parse(response.extract_utf8string().get());
        });
    }

private:
    http::http_request build_request(http::method method, const std::string& relative_url, const headers& hdrs)
    {
        http::http_request req(method);
        req.set_request_uri(to_string_t(relative_url));

        req.headers().add(http::header_names::accept, _XPLATSTR("application/json"));
        req.headers().add(http::header_names::user_agent, m_userAgent);
        req.headers().add(http::header_names::accept_language, ui_language);
        if (!m_auth.empty())
            req.headers().add(http::header_names::authorization, m_auth);
        for (const auto& h: hdrs)
        {
            req.headers().add(to_string_t(h.first), to_string_t(h.second));
        }

        return req;
    }

    // handle non-OK responses:
    void handle_error(http::http_response r)
    {
        if (r.status_code() >= 200 && r.status_code() < 300)
            return; // not an error

        int status_code = r.status_code();
        std::string msg;
        if (r.headers().content_type() == _XPLATSTR("application/json"))
        {
            try
            {
                auto json = ::json::parse(r.extract_utf8string().get());
                msg = m_owner.parse_json_error(json);
            }
            catch (...) {} // report original error if parsing broken
        }
        if (msg.empty())
            msg = str::to_utf8(r.reason_phrase());
        m_owner.on_error_response(status_code, msg);
        BOOST_THROW_EXCEPTION(http::http_exception(status_code, msg));
    }

    // convert to wstring
    static string_t sanitize_url(const std::string& url, int /*flags*/)
    {
        return to_string_t(url);
    }

    static std::string extract_attachment_filename(const http::http_request& request, const http::http_response& response)
    {
        // extract from Content-Disposition attachment filename:
        auto hdr = response.headers().find(http::header_names::content_disposition);
        if (hdr != response.headers().end())
        {
            static const std::basic_regex<utility::char_t> RE_FILENAME(_XPLATSTR("attachment; *filename=\"(.*)\""), std::regex_constants::icase);
            std::match_results<string_t::const_iterator> match;
            if (std::regex_search(hdr->second, match, RE_FILENAME))
                return str::to_utf8(match.str(1));
        }

        // failing that, use the URL:
        auto path = request.absolute_uri().path();
        auto slash = path.find_last_of('/');
        if (slash != string_t::npos)
            path = path.substr(slash + 1);

        return str::to_utf8(path);
    }

    static http::client::http_client_config get_client_config()
    {
        http::client::http_client_config c;

    #ifdef _WIN32
        // prepare WinHttp configuration:
        c.set_nativesessionhandle_options([](http::client::native_handle handle){
            DWORD dwOption = WINHTTP_PROTOCOL_FLAG_HTTP2;
            WinHttpSetOption(handle, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &dwOption, sizeof(dwOption));
        });
    #else
        // setup proxy on Unix platforms:
        const char *proxy = std::getenv("https_proxy");
        if (!proxy)
            proxy = std::getenv("http_proxy");
        if (proxy)
            c.set_proxy(web::uri(proxy));
    #endif

        return c;
    }

#ifdef _WIN32
    static std::wstring windows_version()
    {
        OSVERSIONINFOEX info = { 0 };
        info.dwOSVersionInfoSize = sizeof(info);

        NTSTATUS(WINAPI *fRtlGetVersion)(LPOSVERSIONINFOEXW);
        fRtlGetVersion = reinterpret_cast<decltype(fRtlGetVersion)>(GetProcAddress(GetModuleHandleA("ntdll"), "RtlGetVersion"));
        if (fRtlGetVersion)
            fRtlGetVersion(&info);

        return std::to_wstring(info.dwMajorVersion) + L"." + std::to_wstring(info.dwMinorVersion);
    }
#endif

    http_client& m_owner;
    http::client::http_client m_native;
    std::wstring m_userAgent;
    std::wstring m_auth;
};


string_t http_client::impl::ui_language;


http_client::http_client(const std::string& url_prefix, int flags)
    : m_impl(new impl(*this, url_prefix, flags))
{
}

http_client::~http_client()
{
}

void http_client::set_ui_language(const std::string& lang)
{
    impl::ui_language = to_string_t(lang);
}

void http_client::set_authorization(const std::string& auth)
{
    m_impl->set_authorization(auth);
}

dispatch::future<::json> http_client::get(const std::string& url, const headers& hdrs)
{
    return m_impl->get(url, hdrs);
}

dispatch::future<downloaded_file> http_client::download(const std::string& url, const headers& hdrs)
{
    return m_impl->download(url, hdrs);
}

dispatch::future<::json> http_client::post(const std::string& url, const http_body_data& data, const headers& hdrs)
{
    return m_impl->post(url, data, hdrs);
}


#ifdef _WIN32

class http_reachability::impl
{
public:
    impl()
    {
        m_networkListManager = nullptr;
        CoCreateInstance(CLSID_NetworkListManager, NULL, CLSCTX_ALL, IID_INetworkListManager, (LPVOID *)&m_networkListManager);
    }

    ~impl()
    {
        if (m_networkListManager)
            m_networkListManager->Release();
    }

    bool is_reachable() const
    {
        if (m_networkListManager)
        {
            NLM_CONNECTIVITY result;
            HRESULT hr = m_networkListManager->GetConnectivity(&result);
            if (SUCCEEDED(hr))
                return result & (NLM_CONNECTIVITY_IPV4_INTERNET|NLM_CONNECTIVITY_IPV6_INTERNET);
        }
        // manager fallback (IPv6 ignorant):
        DWORD flags;
        return ::InternetGetConnectedState(&flags, 0);
    }

private:
    INetworkListManager *m_networkListManager;
};

#else // !WIN32

class http_reachability::impl
{
public:
    impl() {}
    bool is_reachable() const { return true; } // TODO
};

#endif


http_reachability::http_reachability(const std::string& /*url*/)
    : m_impl(new impl)
{
}

http_reachability::~http_reachability()
{
}

bool http_reachability::is_reachable() const
{
    return m_impl->is_reachable();
}
