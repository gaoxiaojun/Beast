//
// Copyright (c) 2013-2016 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_WEBSOCKET_IMPL_STREAM_IPP
#define BEAST_WEBSOCKET_IMPL_STREAM_IPP

#include <beast/websocket/teardown.hpp>
#include <beast/websocket/detail/hybi13.hpp>
#include <beast/http/read.hpp>
#include <beast/http/write.hpp>
#include <beast/http/reason.hpp>
#include <beast/http/rfc7230.hpp>
#include <beast/core/buffer_cat.hpp>
#include <beast/core/buffer_concepts.hpp>
#include <beast/core/consuming_buffers.hpp>
#include <beast/core/prepare_buffers.hpp>
#include <beast/core/static_streambuf.hpp>
#include <beast/core/stream_concepts.hpp>
#include <boost/assert.hpp>
#include <boost/endian/buffers.hpp>
#include <algorithm>
#include <memory>
#include <utility>

namespace beast {
namespace websocket {

template<class NextLayer>
template<class... Args>
stream<NextLayer>::
stream(Args&&... args)
    : stream_(std::forward<Args>(args)...)
{
}

//------------------------------------------------------------------------------

template<class NextLayer>
void
stream<NextLayer>::
reset()
{
    failed_ = false;
    rd_need_ = 0;
    rd_cont_ = false;
    wr_close_ = false;
    wr_.cont = false;
    wr_block_ = nullptr;    // should be nullptr on close anyway
    pong_data_ = nullptr;   // should be nullptr on close anyway

    stream_.buffer().consume(
        stream_.buffer().size());
}

template<class NextLayer>
http::request<http::empty_body>
stream<NextLayer>::
build_request(boost::string_ref const& host,
    boost::string_ref const& resource, std::string& key)
{
    http::request<http::empty_body> req;
    req.url = { resource.data(), resource.size() };
    req.version = 11;
    req.method = "GET";
    req.fields.insert("Host", host);
    req.fields.insert("Upgrade", "websocket");
    key = detail::make_sec_ws_key(maskgen_);
    req.fields.insert("Sec-WebSocket-Key", key);
    req.fields.insert("Sec-WebSocket-Version", "13");
    (*d_)(req);
    http::prepare(req, http::connection::upgrade);
    return req;
}

template<class NextLayer>
template<class Body, class Fields>
http::response<http::string_body>
stream<NextLayer>::
build_response(http::request<Body, Fields> const& req)
{
    auto err =
        [&](std::string const& text)
        {
            http::response<http::string_body> res;
            res.status = 400;
            res.reason = http::reason_string(res.status);
            res.version = req.version;
            res.body = text;
            (*d_)(res);
            prepare(res,
                (is_keep_alive(req) && keep_alive_) ?
                    http::connection::keep_alive :
                    http::connection::close);
            return res;
        };
    if(req.version < 11)
        return err("HTTP version 1.1 required");
    if(req.method != "GET")
        return err("Wrong method");
    if(! is_upgrade(req))
        return err("Expected Upgrade request");
    if(! req.fields.exists("Host"))
        return err("Missing Host");
    if(! req.fields.exists("Sec-WebSocket-Key"))
        return err("Missing Sec-WebSocket-Key");
    if(! http::token_list{req.fields["Upgrade"]}.exists("websocket"))
        return err("Missing websocket Upgrade token");
    {
        auto const version =
            req.fields["Sec-WebSocket-Version"];
        if(version.empty())
            return err("Missing Sec-WebSocket-Version");
        if(version != "13")
        {
            http::response<http::string_body> res;
            res.status = 426;
            res.reason = http::reason_string(res.status);
            res.version = req.version;
            res.fields.insert("Sec-WebSocket-Version", "13");
            prepare(res,
                (is_keep_alive(req) && keep_alive_) ?
                    http::connection::keep_alive :
                    http::connection::close);
            return res;
        }
    }
    http::response<http::string_body> res;
    res.status = 101;
    res.reason = http::reason_string(res.status);
    res.version = req.version;
    res.fields.insert("Upgrade", "websocket");
    {
        auto const key =
            req.fields["Sec-WebSocket-Key"];
        res.fields.insert("Sec-WebSocket-Accept",
            detail::make_sec_ws_accept(key));
    }
    res.fields.replace("Server", "Beast.WSProto");
    (*d_)(res);
    http::prepare(res, http::connection::upgrade);
    return res;
}

template<class NextLayer>
template<class Body, class Fields>
void
stream<NextLayer>::
do_response(http::response<Body, Fields> const& res,
    boost::string_ref const& key, error_code& ec)
{
    // VFALCO Review these error codes
    auto fail = [&]{ ec = error::response_failed; };
    if(res.version < 11)
        return fail();
    if(res.status != 101)
        return fail();
    if(! is_upgrade(res))
        return fail();
    if(! http::token_list{res.fields["Upgrade"]}.exists("websocket"))
        return fail();
    if(! res.fields.exists("Sec-WebSocket-Accept"))
        return fail();
    if(res.fields["Sec-WebSocket-Accept"] !=
        detail::make_sec_ws_accept(key))
        return fail();
    open(detail::role_type::client);
}

template<class NextLayer>
void
stream<NextLayer>::
do_read_fh(detail::frame_streambuf& fb,
    close_code::value& code, error_code& ec)
{
    fb.commit(boost::asio::read(
        stream_, fb.prepare(2), ec));
    if(ec)
        return;
    auto const n = read_fh1(fb, code);
    if(code != close_code::none)
        return;
    if(n > 0)
    {
        fb.commit(boost::asio::read(
            stream_, fb.prepare(n), ec));
        if(ec)
            return;
    }
    read_fh2(fb, code);
}

} // websocket
} // beast

#endif
