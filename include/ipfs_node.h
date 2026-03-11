#pragma once
#include <string>
#include <functional>
#include <memory>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/utility/string_view.hpp>
#include "ipfs_config.h"

namespace asio_ipfs {

    // Wraps a move-only completion handler in a shared_ptr so the resulting
    // callable satisfies CopyConstructible, as required by std::function.
    // Needed because boost::asio::detail::spawn_handler is move-only (Boost >=1.88 / MSVC 14.44).
    template<class H>
    auto make_copyable_handler(H&& h) {
        auto sp = std::make_shared<std::decay_t<H>>(std::forward<H>(h));
        return [sp](auto&&... args) mutable { (*sp)(std::forward<decltype(args)>(args)...); };
    }

    struct node_impl;
    class node {
        using Timer = boost::asio::steady_timer;
        using Cancel = std::function<void()>;

        template<class Token, class... Ret>
        using Result = typename boost::asio::async_result<std::decay_t<Token>, void(boost::system::error_code, Ret...)>;

        using string_view = boost::string_view;

    public:
        static const uint32_t CID_SIZE = 46;
        using StateCB = std::function<void (const std::string& error, uint32_t peercnt)>;

        using LogCB = std::function<void (const char*)>;

        [[maybe_unused]] static void redirect_logs(LogCB);

    public:
        // This constructor may do repository initialization disk IO and as such
        // may block for a second or more. If that is undesired, use the static
        // async `node::build` function instead.
        node(boost::asio::io_context&, StateCB, config);

        node(node&&) noexcept;
        node& operator=(node&&) noexcept;

        node(const node&) = delete;
        node& operator=(const node&) = delete;

        boost::asio::io_context& get_io_service();
        void free();
        ~node();

        template<class Token>
        static typename Result<Token, std::unique_ptr<node>>::return_type
        build(boost::asio::io_context&, StateCB, config, Token&&);

        template<class Token>
        static typename Result<Token, std::unique_ptr<node>>::return_type
        build(boost::asio::io_context&, StateCB, config, Cancel&, Token&&);

        // Returns this node's IPFS ID
        [[nodiscard]] std::string id() const;

        template<class Token>
        typename Result<Token, std::string>::return_type
        add(const uint8_t* data, size_t size, bool pin, Token&&);

        template<class Token>
        typename Result<Token, std::string>::return_type
        add(const uint8_t* data, size_t size, bool pin, Cancel&, Token&&);

        template<class Token>
        typename Result<Token, std::string>::return_type
        calc_cid(const uint8_t* data, size_t size, Token&&);

        template<class Token>
        typename Result<Token, std::string>::return_type
        calc_cid(const uint8_t* data, size_t size, Cancel&, Token&&);

        template<class Token>
        typename Result<Token, std::vector<uint8_t>>::return_type
        cat(const std::string& cid, Token&&);

        template<class Token>
        typename Result<Token, std::vector<uint8_t>>::return_type
        cat(const std::string& cid, Cancel& cancel, Token&&);

        template<class Token>
        void publish(const std::string& cid, Timer::duration, Token&&);

        template<class Token>
        void publish(const std::string& cid, Timer::duration, Cancel&, Token&&);

        template<class Token>
        typename Result<Token, std::string>::return_type
        resolve(const std::string& ipns_id, Token&&);

        template<class Token>
        typename Result<Token, std::string>::return_type
        resolve(const std::string& ipns_id, Cancel&, Token&&);

        template<class Token>
        void pin(const std::string& cid, Token&&);

        template<class Token>
        void pin(const std::string& cid, Cancel&, Token&&);

        template<class Token>
        void unpin(const std::string& cid, Token&&);

        template<class Token>
        void unpin(const std::string& cid, Cancel&, Token&&);

        template<class Token>
        void gc(Cancel&, Token&&);

    private:
        static void build_(boost::asio::io_context&, StateCB, config, Cancel*,
                           std::function<void( const boost::system::error_code&, std::unique_ptr<node>)>);
        void add_(const uint8_t* data, size_t size, bool pin, Cancel*, std::function<void(boost::system::error_code, std::string)>&&);
        void calc_cid_(const uint8_t* data, size_t size, Cancel*, std::function<void(boost::system::error_code, std::string)>&&);
        void cat_(const std::string& cid, Cancel*, std::function<void(boost::system::error_code, std::vector<uint8_t>)>);
        void publish_(const std::string& cid, Timer::duration, Cancel*, std::function<void(boost::system::error_code)>);
        void resolve_(const std::string& ipns_id, Cancel*, std::function<void(boost::system::error_code, std::string)>);
        void pin_(const std::string& cid, Cancel*, std::function<void(boost::system::error_code)>);
        void unpin_(const std::string& cid, Cancel*, std::function<void(boost::system::error_code)>);
        void gc_(Cancel*, std::function<void(boost::system::error_code)>);

    private:
        node();
        std::unique_ptr<node_impl> _impl;
    };

    template<class Token>
    inline typename node::Result<Token, std::unique_ptr<node>>::return_type
    node::build( boost::asio::io_context& ios
               , StateCB scb
               , config cfg
               , Token&& token)
    {
        using BackendP = std::unique_ptr<node>;
        return boost::asio::async_initiate<Token, void(boost::system::error_code, BackendP)>(
            [&ios, scb=std::move(scb), cfg](auto handler) mutable {
                build_(ios, std::move(scb), cfg, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::unique_ptr<node>>::return_type
    node::build( boost::asio::io_context& ios
               , StateCB scb
               , config cfg
               , Cancel& cancel
               , Token&& token)
    {
        using BackendP = std::unique_ptr<node>;
        return boost::asio::async_initiate<Token, void(boost::system::error_code, BackendP)>(
            [&ios, &cancel, scb=std::move(scb), cfg](auto handler) mutable {
                build_(ios, std::move(scb), cfg, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::add(const uint8_t* data, size_t size, bool pin, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, data, size, pin](auto handler) mutable {
                add_(data, size, pin, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::add(const uint8_t* data, size_t size, bool pin, Cancel& cancel, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, data, size, pin, &cancel](auto handler) mutable {
                add_(data, size, pin, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::calc_cid(const uint8_t* data, size_t size, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, data, size](auto handler) mutable {
                calc_cid_(data, size, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::calc_cid(const uint8_t* data, size_t size, Cancel& cancel, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, data, size, &cancel](auto handler) mutable {
                calc_cid_(data, size, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::vector<uint8_t>>::return_type
    node::cat(const std::string& cid, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::vector<uint8_t>)>(
            [this, cid](auto handler) mutable {
                cat_(cid, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::vector<uint8_t>>::return_type
    node::cat(const std::string& cid, Cancel& cancel, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::vector<uint8_t>)>(
            [this, cid, &cancel](auto handler) mutable {
                cat_(cid, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::publish(const std::string& cid, Timer::duration d, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid, d](auto handler) mutable {
                publish_(cid, d, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::publish(const std::string& cid, Timer::duration d, Cancel& cancel, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid, d, &cancel](auto handler) mutable {
                publish_(cid, d, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::resolve(const std::string& ipns_id, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, ipns_id](auto handler) mutable {
                resolve_(ipns_id, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline typename node::Result<Token, std::string>::return_type
    node::resolve(const std::string& ipns_id, Cancel& cancel, Token&& token)
    {
        return boost::asio::async_initiate<Token, void(boost::system::error_code, std::string)>(
            [this, ipns_id, &cancel](auto handler) mutable {
                resolve_(ipns_id, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::pin(const std::string& cid, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid](auto handler) mutable {
                pin_(cid, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::pin(const std::string& cid, Cancel& cancel, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid, &cancel](auto handler) mutable {
                pin_(cid, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::unpin(const std::string& cid, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid](auto handler) mutable {
                unpin_(cid, nullptr, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::unpin(const std::string& cid, Cancel& cancel, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, cid, &cancel](auto handler) mutable {
                unpin_(cid, &cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }

    template<class Token>
    inline void node::gc(Cancel& cancel, Token&& token)
    {
        boost::asio::async_initiate<Token, void(boost::system::error_code)>(
            [this, &cancel](auto handler) mutable {
                gc_(&cancel, make_copyable_handler(std::move(handler)));
            },
            token
        );
    }
}
