#pragma once

#ifdef WIN32
#define NOMINMAX
#endif

#include "cpr/api.h"
#include "cpr/session.h"
#include <future>

class Multi {
  public:
    Multi();
    ~Multi();

    void RunLoop();

    void RequestStop();

    template <typename Then, typename... Ts>
    auto GetCallback(Then&& then, Ts... ts) -> std::future<decltype(then(std::declval<cpr::Response>()))> {
        cpr::Session session;
        cpr::priv::set_option(session, std::forward<Ts>(ts)...);
        // std::initializer_list<int> setopts = { (session.SetOption(std::forward<Ts>(ts)), 0)... };
        session.PrepareGet();
        return SubmitCallback(std::forward<Then>(then), std::move(session));
    }

    template <typename Then, typename... Ts>
    void GetDetached(Then&& then, Ts... ts) {
        cpr::Session session;
        cpr::priv::set_option(session, std::forward<Ts>(ts)...);
        // std::initializer_list<int> setopts = { (session.SetOption(std::forward<Ts>(ts)), 0)... };
        session.PrepareGet();
        return SubmitDetached(std::forward<Then>(then), std::move(session));
    }

    template <typename Then>
    void PerformDetached(Then&& then, cpr::Session&& session /* borrowed */) {
        using Req = TReqSession<typename std::decay<Then>::type>;
        Submit(ReqUPtr{new Req(std::forward<Then>(then), std::move(session))});
    }

  private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    // Base request type.
    // This is a polymorphic type implemented without virtual methods to limit bloats
    class ReqP {
      public:
        cpr::Session m_session;
        void (*m_completeFn)(ReqP* that, CURLcode curlError);

      protected:
        ~ReqP() = default;
    };
    struct Del {
        void operator()(ReqP* p);
    };
    using ReqUPtr = std::unique_ptr<ReqP, Del>;

    // Request with a callback which return value is returned via a promise<T>
    template <typename Then, typename Promise>
    struct TReqP : public ReqP {
        Then m_then;
        Promise m_promise;

        TReqP(Then&& then, cpr::Session&& session) : m_then(std::move(then)) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }
        TReqP(const Then& then, cpr::Session&& session) : m_then(then) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }
        static void complete(ReqP* obj, CURLcode curlError) {
            auto that = static_cast<TReqP*>(obj);
            try {
                that->m_promise.set_value(that->m_then(that->m_session.Complete(curlError)));
            } catch (...) {
                that->m_promise.set_exception(std::current_exception());
            }
            delete that;
        }
    };

    // Request with a callback which return value is returned via a promise<void>
    template <typename Then>
    struct TReqP<Then, std::promise<void>> : public ReqP {
        Then m_then;
        using Promise = std::promise<void>;
        Promise m_promise;

        TReqP(Then&& then, cpr::Session&& session) : m_then(std::move(then)) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }
        TReqP(const Then& then, cpr::Session&& session) : m_then(then) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }

        static void complete(ReqP* obj, CURLcode curlError) {
            auto that = static_cast<TReqP*>(obj);
            try {
                that->m_then(that->m_session.Complete(curlError));
                that->m_promise.set_value();
            } catch (...) {
                that->m_promise.set_exception(std::current_exception());
            }
            delete that;
        }
    };

    // Result with a callback which return value is ignored
    template <typename Then>
    struct TReqPNCB : public ReqP {
        Then m_then;

        TReqPNCB(Then&& then, cpr::Session&& session) : m_then(std::move(then)) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }
        TReqPNCB(const Then& then, cpr::Session&& session) : m_then(then) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }

        static void complete(ReqP* obj, CURLcode curlError) {
            auto that = static_cast<TReqPNCB*>(obj);
            that->m_then(that->m_session.Complete(curlError));
            delete that;
        }
    };

    // Result with a callback which return value is ignored 
    template <typename Then>
    struct TReqSession : public ReqP {
        Then m_then;

        TReqSession(Then&& then, cpr::Session&& session) : m_then(std::move(then)) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }
        TReqSession(const Then& then, cpr::Session&& session) : m_then(then) {
            m_session = std::move(session);
            m_completeFn = &complete;
        }

        static void complete(ReqP* obj, CURLcode curlError) {
            auto that = static_cast<TReqSession*>(obj);
            that->m_then(std::move(that->m_session), curlError);
            delete that;
        }
    };

    template <typename Then>
    auto SubmitCallback(Then&& then, cpr::Session&& session) -> std::future<decltype(then(std::declval<cpr::Response>()))> {
        using Promise = std::promise<decltype(then(std::declval<cpr::Response>()))>;
        using Req = TReqP<typename std::decay<Then>::type, Promise>;
        std::unique_ptr<Req> req{new Req(std::forward<Then>(then), std::move(session))};
        auto future = req->m_promise.get_future();
        Submit(std::move(req));
        return future;
    }

    template <typename Then>
    void SubmitDetached(Then&& then, cpr::Session&& session) {
        using Req = TReqPNCB<typename std::decay<Then>::type>;
        Submit(ReqUPtr{new Req(std::forward<Then>(then), std::move(session))});
    }

    void Submit(ReqUPtr req);
};
