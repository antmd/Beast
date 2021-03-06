//
// Copyright (c) 2013-2016 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_WEBSOCKET_ASYNC_ECHO_PEER_H_INCLUDED
#define BEAST_WEBSOCKET_ASYNC_ECHO_PEER_H_INCLUDED

#include <beast/core/placeholders.hpp>
#include <beast/core/streambuf.hpp>
#include <beast/websocket.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

namespace beast {
namespace websocket {

// Asynchronous WebSocket echo client/server
//
class async_echo_peer
{
public:
    using endpoint_type = boost::asio::ip::tcp::endpoint;
    using address_type = boost::asio::ip::address;
    using socket_type = boost::asio::ip::tcp::socket;

private:
    bool log_ = false;
    boost::asio::io_service ios_;
    socket_type sock_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::thread> thread_;

public:
    async_echo_peer(bool server,
            endpoint_type const& ep, std::size_t threads)
        : sock_(ios_)
        , acceptor_(ios_)
    {
        if(server)
        {
            error_code ec;
            acceptor_.open(ep.protocol(), ec);
            maybe_throw(ec, "open");
            acceptor_.set_option(
                boost::asio::socket_base::reuse_address{true});
            acceptor_.bind(ep, ec);
            maybe_throw(ec, "bind");
            acceptor_.listen(
                boost::asio::socket_base::max_connections, ec);
            maybe_throw(ec, "listen");
            acceptor_.async_accept(sock_,
                std::bind(&async_echo_peer::on_accept, this,
                    beast::asio::placeholders::error));
        }
        else
        {
            Peer{log_, std::move(sock_), ep};
        }
        thread_.reserve(threads);
        for(std::size_t i = 0; i < threads; ++i)
            thread_.emplace_back(
                [&]{ ios_.run(); });
    }

    ~async_echo_peer()
    {
        error_code ec;
        ios_.dispatch(
            [&]{ acceptor_.close(ec); });
        for(auto& t : thread_)
            t.join();
    }

    endpoint_type
    local_endpoint() const
    {
        return acceptor_.local_endpoint();
    }

private:
    class Peer
    {
        struct data
        {
            bool log;
            int state = 0;
            boost::optional<endpoint_type> ep;
            stream<socket_type> ws;
            boost::asio::io_service::strand strand;
            opcode op;
            beast::streambuf sb;
            int id;

            data(bool log_, socket_type&& sock_)
                : log(log_)
                , ws(std::move(sock_))
                , strand(ws.get_io_service())
                , id([]
                    {
                        static int n = 0;
                        return ++n;
                    }())
            {
            }

            data(bool log_, socket_type&& sock_,
                    endpoint_type const& ep_)
                : log(log_)
                , ep(ep_)
                , ws(std::move(sock_))
                , strand(ws.get_io_service())
                , id([]
                    {
                        static int n = 0;
                        return ++n;
                    }())
            {
            }
        };

        std::shared_ptr<data> d_;

    public:
        Peer(Peer&&) = default;
        Peer(Peer const&) = default;
        Peer& operator=(Peer&&) = delete;
        Peer& operator=(Peer const&) = delete;

        struct identity
        {
            template<class Body, class Headers>
            void
            operator()(http::message<true, Body, Headers>& req)
            {
                req.headers.replace("User-Agent", "async_echo_client");
            }

            template<class Body, class Headers>
            void
            operator()(http::message<false, Body, Headers>& resp)
            {
                resp.headers.replace("Server", "async_echo_server");
            }
        };

        template<class... Args>
        explicit
        Peer(bool log, socket_type&& sock, Args&&... args)
            : d_(std::make_shared<data>(log,
                std::forward<socket_type>(sock),
                    std::forward<Args>(args)...))
        {
            auto& d = *d_;
            d.ws.set_option(decorate(identity{}));
            d.ws.set_option(read_message_max(64 * 1024 * 1024));
            run();
        }

        void run()
        {
            auto& d = *d_;
            if(! d.ep)
            {
                d.ws.async_accept(std::move(*this));
            }
            else
            {
                d.state = 4;
                d.ws.next_layer().async_connect(
                    *d.ep, std::move(*this));
            }
        }

        template<class Streambuf, std::size_t N>
        static
        bool
        match(Streambuf& sb, char const(&s)[N])
        {
            using boost::asio::buffer;
            using boost::asio::buffer_copy;
            if(sb.size() < N-1)
                return false;
            static_string<N-1> t;
            t.resize(N-1);
            buffer_copy(buffer(t.data(), t.size()),
                sb.data());
            if(t != s)
                return false;
            sb.consume(N-1);
            return true;
        }

        void operator()(error_code ec, std::size_t)
        {
            (*this)(ec);
        }

        void operator()(error_code ec)
        {
            using boost::asio::buffer;
            using boost::asio::buffer_copy;
            auto& d = *d_;
            switch(d.state)
            {
            // did accept
            case 0:
                if(ec)
                    return fail(ec, "async_accept");

            // start
            case 1:
                if(ec)
                    return fail(ec, "async_handshake");
                d.sb.consume(d.sb.size());
                // read message
                d.state = 2;
                d.ws.async_read(d.op, d.sb,
                    d.strand.wrap(std::move(*this)));
                return;

            // got message
            case 2:
                if(ec == error::closed)
                    return;
                if(ec)
                    return fail(ec, "async_read");
                if(match(d.sb, "RAW"))
                {
                    d.state = 1;
                    boost::asio::async_write(d.ws.next_layer(),
                        d.sb.data(), d.strand.wrap(std::move(*this)));
                    return;
                }
                else if(match(d.sb, "TEXT"))
                {
                    d.state = 1;
                    d.ws.set_option(message_type{opcode::text});
                    d.ws.async_write(
                        d.sb.data(), d.strand.wrap(std::move(*this)));
                    return;
                }
                else if(match(d.sb, "PING"))
                {
                    ping_data payload;
                    d.sb.consume(buffer_copy(
                        buffer(payload.data(), payload.size()),
                            d.sb.data()));
                    d.state = 1;
                    d.ws.async_ping(payload,
                        d.strand.wrap(std::move(*this)));
                    return;
                }
                else if(match(d.sb, "CLOSE"))
                {
                    d.state = 1;
                    d.ws.async_close({},
                        d.strand.wrap(std::move(*this)));
                    return;
                }
                // write message
                d.state = 1;
                d.ws.set_option(message_type(d.op));
                d.ws.async_write(d.sb.data(),
                    d.strand.wrap(std::move(*this)));
                return;

            // connected
            case 4:
                if(ec)
                    return fail(ec, "async_connect");
                d.state = 1;
                d.ws.async_handshake(
                    d.ep->address().to_string() + ":" +
                        std::to_string(d.ep->port()),
                            "/", d.strand.wrap(std::move(*this)));
                return;
            }
        }

    private:
        void
        fail(error_code ec, std::string what)
        {
            auto& d = *d_;
            if(d.log)
            {
                if(ec != error::closed)
                    std::cerr << "#" << d_->id << " " <<
                        what << ": " << ec.message() << std::endl;
            }
        }
    };

    void
    fail(error_code ec, std::string what)
    {
        if(log_)
            std::cerr << what << ": " <<
                ec.message() << std::endl;
    }

    void
    maybe_throw(error_code ec, std::string what)
    {
        if(ec)
        {
            fail(ec, what);
            throw ec;
        }
    }

    void
    on_accept(error_code ec)
    {
        if(! acceptor_.is_open())
            return;
        if(ec == boost::asio::error::operation_aborted)
            return;
        maybe_throw(ec, "accept");
        socket_type sock(std::move(sock_));
        acceptor_.async_accept(sock_,
            std::bind(&async_echo_peer::on_accept, this,
                beast::asio::placeholders::error));
        Peer{false, std::move(sock)};
    }
};

} // websocket
} // beast

#endif
