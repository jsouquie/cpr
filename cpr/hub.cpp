#ifdef WIN32
#define NOMINMAX
#include "wepoll/wepoll.h"
#endif

#include "cpr/hub.h"

#include "cpr/session.h"
#include "curl/multi.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <vector>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#endif


class MultiHttpClient::Impl {
  public:
    Impl();
    ~Impl();

    void Submit(std::unique_ptr<ReqP> req);
    void RunLoop();
    void RequestStop();

  private:
    std::mutex m_mutex;
    std::deque<std::unique_ptr<ReqP>> m_requests;
    CURLM* m_multiHdl;
    bool m_stopRequested = false;

    bool m_deadlineActive = false;
    std::chrono::time_point<std::chrono::steady_clock> m_deadline;
    int m_runningHandles = 0;

    int socketCallback(curl_socket_t s, int what, void* socketp);
    int timerCallback(long timeout);
    bool performPoll(int timeout);

#ifdef WIN32
    HANDLE m_epollfd;
    SOCKET m_wakeUpListen;
    SOCKET m_wakeUpSrv;
    SOCKET m_wakeupCli;
#elif __linux__
    int m_epollfd;
    int m_wakeUpCli;
    int m_wakeUpSrv;
#endif
};


MultiHttpClient::Impl::Impl() {
    m_epollfd = epoll_create1(0);

#ifdef WIN32
    m_wakeUpListen = ::socket(AF_INET, SOCK_STREAM, 0);
    // wsa_assert(listener != INVALID_SOCKET);
    BOOL so_reuseaddr = 1;
    int rc = setsockopt(m_wakeUpListen, SOL_SOCKET, SO_REUSEADDR, (char*) &so_reuseaddr, sizeof(so_reuseaddr));
    // wsa_assert(rc != SOCKET_ERROR);
    BOOL tcp_nodelay = 1;
    rc = setsockopt(m_wakeUpListen, IPPROTO_TCP, TCP_NODELAY, (char*) &tcp_nodelay, sizeof(tcp_nodelay));
    // wsa_assert(rc != SOCKET_ERROR);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    //  Create the writer socket.
    m_wakeupCli = ::socket(AF_INET, SOCK_STREAM, 0);
    // wsa_assert(*w_ != INVALID_SOCKET);
    rc = setsockopt(m_wakeupCli, IPPROTO_TCP, TCP_NODELAY, (char*) &tcp_nodelay, sizeof tcp_nodelay);
    // wsa_assert(rc != SOCKET_ERROR);
    rc = bind(m_wakeUpListen, (const struct sockaddr*) &addr, sizeof addr);
    // if (rc != SOCKET_ERROR) {
    int addrlen = sizeof addr;
    rc = getsockname(m_wakeUpListen, (struct sockaddr*) &addr, &addrlen);
    rc = listen(m_wakeUpListen, 1);
    rc = connect(m_wakeupCli, (struct sockaddr*) &addr, sizeof addr);
    m_wakeUpSrv = accept(m_wakeUpListen, NULL, NULL);
    closesocket(m_wakeUpListen);
    unsigned long lval = 1;
    ::ioctlsocket(m_wakeUpSrv, FIONBIO, &lval);

#elif __linux__
    m_wakeUpSrv = eventfd(0, 0);
    m_wakeUpCli = m_wakeUpSrv;
#endif

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = m_wakeUpSrv;
    epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_wakeUpSrv, &ev);

    m_multiHdl = curl_multi_init();
    assert(m_multiHdl);

    auto socket_callback = +[](CURL* easy, curl_socket_t s, int what, void* userp, void* socketp) {
        auto that = static_cast<MultiHttpClient::Impl*>(userp);
        return that->socketCallback(s, what, socketp);
    };
    auto timer_callback = +[](CURLM* m, long timeout, void* userp) {
        auto that = static_cast<MultiHttpClient::Impl*>(userp);
        return that->timerCallback(timeout);
    };

    curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERFUNCTION, timer_callback);
    curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERDATA, this);
}

MultiHttpClient::Impl::~Impl() {
#ifdef WIN32
    closesocket(m_wakeupCli);
    closesocket(m_wakeUpSrv);
    epoll_close(m_epollfd);
#elif __linux__
    close(m_wakeUpSrv);
    close(m_epollfd);
#endif
    curl_multi_cleanup(m_multiHdl);
}

void MultiHttpClient::Impl::Submit(std::unique_ptr<ReqP> req) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(std::move(req));
#ifdef WIN32
        char v = 1;
        send(m_wakeupCli, &v, sizeof(v), 0);
#elif __linux__
        uint64_t v = 1;
        write(m_wakeUpCli, &v, sizeof(v));
#endif
    }
}

void MultiHttpClient::Impl::RequestStop() {
    {
        m_stopRequested = true;
#ifdef WIN32
        char v = 1;
        send(m_wakeupCli, &v, sizeof(v), 0);
#elif __linux__
        uint64_t v = 1;
        write(m_wakeUpCli, &v, sizeof(v));
#endif
    }
}

// curl wants us to install a timer
int MultiHttpClient::Impl::timerCallback(long timeout) {
    // printf("timerCallback %d\n", timeout);
    if (timeout == -1) {
        m_deadlineActive = false;
    } else {
        m_deadlineActive = true;
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    }
    return 0;
}

// curl wants us to perform an action on a socket
int MultiHttpClient::Impl::socketCallback(curl_socket_t s, int what, void* socketp) {
    // printf("socketCallback %d %d\n", s, what);
    struct epoll_event event;
    event.events = 0;
    event.data.fd = s;
    int op = EPOLL_CTL_MOD;
    switch (what) {
        case CURL_POLL_IN:
            event.events = EPOLLIN;
            break;
        case CURL_POLL_OUT:
            event.events = EPOLLOUT;
            break;
        case CURL_POLL_INOUT:
            event.events = EPOLLIN | EPOLLOUT;
            break;
        case CURL_POLL_REMOVE:
            op = EPOLL_CTL_DEL;
            break;
    }
    if ((op == EPOLL_CTL_MOD) && (socketp == nullptr)) {
        curl_multi_assign(m_multiHdl, s, reinterpret_cast<void*>(~0));
        op = EPOLL_CTL_ADD;
    }

    // printf("socketCallback epoll_ctl s=%d op=%d events=%x\n", s, op, event.events);
    epoll_ctl(m_epollfd, op, s, &event);

    return 0;
}

static constexpr size_t MaxEvents = 10;

/**
 * @returns true if new client requests shall be examined
 */
bool MultiHttpClient::Impl::performPoll(int timeout) {
    printf("poll() sockets with timeout %d...\n", timeout);
    bool newClientRequests = false;
    struct epoll_event events[MaxEvents];
    int pollres = epoll_wait(m_epollfd, events, MaxEvents, timeout);

    if (pollres < 0) {
        // error
        // printf("error\n");
    } else if (pollres == 0) {
        // printf("timeout\n");
        curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
    } else {
        for (int n = 0; n < pollres; ++n) {
            // printf("epoll_wait result: %d ... ", pollres);
            if (events[n].data.fd == m_wakeUpSrv) {
                // printf("  fd[%d] = %d m_wakeUpSrv\n", n, events[n].data.fd );
#ifdef WIN32
                char v[256]; // allow processing of multiple client requests at once
                recv(m_wakeUpSrv, v, sizeof(v), 0);
#else
                uint64_t v;
                read(m_wakeUpSrv, &v, sizeof(v));
#endif
                newClientRequests = true;
            } else {
                // printf("  fd[%d] = %d events[%x]\n", n, events[n].data.fd, events[n].events);
                int evbitmap = 0;
                if (events[n].events & EPOLLIN) {
                    evbitmap |= CURL_CSELECT_IN;
                }
                if (events[n].events & EPOLLOUT) {
                    evbitmap |= CURL_CSELECT_OUT;
                }
                curl_multi_socket_action(m_multiHdl, events[n].data.fd, evbitmap, &m_runningHandles);
            }
        }
    }

    return newClientRequests;
}


void MultiHttpClient::Impl::RunLoop() {
    for (;;) {
        // poll the sockets and the client requests fd
        int timeout = m_deadlineActive ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now()).count()) : -1;
        if (m_deadlineActive && (timeout <= 0)) {
            m_deadlineActive = false;
            curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
        } else {
            bool clientreq = performPoll(timeout);

            // evaluate new requests from our clients
            if (clientreq) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_requests.empty()) {
                    for (auto& req : m_requests) {
                        // the property of ReqP is transfered to the CURL handle
                        ReqP* preq = req.release();
                        CURL* curlHdl = preq->m_session.GetCurlHolder()->handle;
                        curl_easy_setopt(curlHdl, CURLOPT_PRIVATE, preq);

                        // printf("add request %p\n", preq );
                        CURLMcode res = curl_multi_add_handle(m_multiHdl, curlHdl);
                    }
                    curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
                    m_requests.clear();
                }
            }
        }

        // terminate completed transfers
        // printf("runningHandles %d\n", m_runningHandles);
        int msgsInQueue = 0;
        do {
            struct CURLMsg* m = curl_multi_info_read(m_multiHdl, &msgsInQueue);
            if (m && m->msg == CURLMSG_DONE) {
                m->data.result;
                m->data.whatever;
                CURL* e = m->easy_handle;
                curl_multi_remove_handle(m_multiHdl, e);

                // complete and delete the transaction
                ReqP* transaction;
                curl_easy_getinfo(e, CURLINFO_PRIVATE, &transaction);
                transaction->m_completeFn(transaction, m->data.result);
            }
        } while (msgsInQueue);

        /*if (0 == m_runningHandles) {
            printf("No running handles\n");
        }*/
    }
}


MultiHttpClient::MultiHttpClient() : pimpl_(new Impl()) {}
MultiHttpClient::~MultiHttpClient() = default;
void MultiHttpClient::RunLoop() {
    pimpl_->RunLoop();
}
void MultiHttpClient::RequestStop() {
    pimpl_->RequestStop();
}
void MultiHttpClient::Submit(std::unique_ptr<ReqP> req) {
    pimpl_->Submit(std::move(req));
}
