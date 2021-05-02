#ifdef WIN32
#define NOMINMAX
#include "wepoll/wepoll.h"
#endif

#include "cpr/multi.h"
#include "cpr/session.h"
#include "curl/multi.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <set>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

void wsa_assert(bool c, const char* err) {
    if (!c) {
        printf("wsa_assert pas bon\n");
        throw std::runtime_error(err);
    }
}

#ifdef __APPLE__
/* Signaling channel using socketpair */
class SigChan {
  public:
    SigChan() {
        int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, m_sockets);
        wsa_assert(rc != -1, "9");
        unsigned long lval = 1;
        rc = ::ioctl(getServer(), FIONBIO, &lval);
        wsa_assert(rc != -1, "10");
    }
    ~SigChan() {
        ::close(m_sockets[0]);
        ::close(m_sockets[1]);
    }
    void notify() {
        char v = 1;
        send(m_sockets[0], &v, sizeof(v), 0);
    }
    void drain() {
        char v[256]; // allow processing of multiple client requests at once
        recv(m_sockets[1], v, sizeof(v), 0);
    }
    int getServer() const {
        return m_sockets[1];
    }

  private:
    int m_sockets[2]{};
};
#endif

#ifdef WIN32
/* Signaling channel using TCP and Windows Sockets */
class SigChan {
  public:
    SigChan() {
        SOCKET wakeUpListen = ::socket(AF_INET, SOCK_STREAM, 0);
        wsa_assert(wakeUpListen != INVALID_SOCKET, "1");
        BOOL so_reuseaddr = 1;
        int rc = setsockopt(wakeUpListen, SOL_SOCKET, SO_REUSEADDR, (char*) &so_reuseaddr, sizeof(so_reuseaddr));
        wsa_assert(rc != SOCKET_ERROR, "2");
        BOOL tcp_nodelay = 1;
        rc = setsockopt(wakeUpListen, IPPROTO_TCP, TCP_NODELAY, (char*) &tcp_nodelay, sizeof(tcp_nodelay));
        wsa_assert(rc != SOCKET_ERROR, "3");
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        //  Create the writer socket.
        m_wakeUpCli = ::socket(AF_INET, SOCK_STREAM, 0);
        wsa_assert(m_wakeUpCli != INVALID_SOCKET, "4");
        rc = setsockopt(m_wakeUpCli, IPPROTO_TCP, TCP_NODELAY, (char*) &tcp_nodelay, sizeof tcp_nodelay);
        wsa_assert(rc != SOCKET_ERROR, "5");
        rc = bind(wakeUpListen, (const struct sockaddr*) &addr, sizeof addr);
        wsa_assert(rc != SOCKET_ERROR, "6");
        int addrlen = sizeof addr;
        rc = getsockname(wakeUpListen, (struct sockaddr*) &addr, &addrlen);
        rc = listen(wakeUpListen, 1);
        rc = connect(m_wakeUpCli, (struct sockaddr*) &addr, sizeof addr);
        m_wakeUpSrv = accept(wakeUpListen, NULL, NULL);
        closesocket(wakeUpListen);
        unsigned long lval = 1;
        ::ioctlsocket(m_wakeUpSrv, FIONBIO, &lval);
    }
    ~SigChan() {
        ::closesocket(m_wakeUpCli);
        ::closesocket(m_wakeUpSrv);
    }
    void notify() {
        char v = 1;
        send(m_wakeUpCli, &v, sizeof(v), 0);
    }
    void drain() {
        char v[256]; // allow processing of multiple client requests at once
        recv(m_wakeUpSrv, v, sizeof(v), 0);
    }
    int getServer() const {
        return m_wakeUpSrv;
    }

  private:
    SOCKET m_wakeUpCli;
    SOCKET m_wakeUpSrv;
};
#endif

#ifdef __linux__
/* Signaling channel using eventfd */
class SigChan {
  public:
    SigChan() {
        m_sigevt = eventfd(0, 0);
    }
    ~SigChan() {
        close(m_sigevt);
    }
    void notify() {
        uint64_t v = 1;
        write(m_sigevt, &v, sizeof(v));
    }
    void drain() {
        uint64_t v;
        read(m_sigevt, &v, sizeof(v));
    }
    int getServer() const {
        return m_sigevt;
    }

  private:
    int m_sigevt;
};
#endif

#if defined(__APPLE__)
/* Event queue using kqueue */
class EventQueue {
  public:
    EventQueue(SigChan& chan) : m_chan(chan) {
        m_kqueue = kqueue();
        struct kevent ev;
        EV_SET(&ev, chan.getServer(), EVFILT_READ, EV_ADD, 0, 0, nullptr);
        int rc = kevent(m_kqueue, &ev, 1, &ev, 0, nullptr);
        wsa_assert(rc != -1, "kevent");
    }
    ~EventQueue() {
        close(m_kqueue);
    }
    void setCurlMultiHandle(CURLM*) {}
    void configureSocket(curl_socket_t s, int what, void*) {
        struct kevent ev[2];
        EV_SET(&ev[0], s, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&ev[1], s, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        switch (what) {
            case CURL_POLL_IN:
                ev[0].flags = EV_ADD;
                break;
            case CURL_POLL_OUT:
                ev[1].flags = EV_ADD;
                break;
            case CURL_POLL_INOUT:
                ev[0].flags = EV_ADD;
                ev[1].flags = EV_ADD;
                break;
            case CURL_POLL_REMOVE:
                ev[0].flags = EV_DELETE;
                ev[1].flags = EV_DELETE;
                break;
        }
        kevent(m_kqueue, ev, 2, ev, 0, nullptr);
    }
    template <typename TimeOutHdl, typename SockHdl, typename SigCHdl>
    void processEvents(int timeout, TimeOutHdl&& timeOutHandler, SockHdl&& socketHandler, SigCHdl&& sigChanHandler) {
        static constexpr size_t MaxEvents = 10;
        struct kevent events[MaxEvents];
        struct timespec ts = {timeout / 1000, (timeout % 1000) * 1000000};
        int kres = kevent(m_kqueue, events, 0, events, MaxEvents, timeout >= 0 ? &ts : nullptr);

        if (kres < 0) {
            // error
            // printf("error\n");
        } else if (kres == 0) {
            // printf("timeout\n");
            timeOutHandler();
        } else {
            // printf("kevent result: %d ...\n", kres);
            for (int n = 0; n < kres; ++n) {
                if (events[n].ident == m_chan.getServer()) {
                    // printf("  fd[%d] = %lu m_wakeUpSrv\n", n, events[n].ident);
                    m_chan.drain();
                    sigChanHandler();
                } else {
                    // printf("  fd[%d] = %lu filter[%d]\n", n, events[n].ident, events[n].filter);
                    int evbitmap = 0;
                    if (events[n].filter == EVFILT_READ) {
                        evbitmap |= CURL_CSELECT_IN;
                    }
                    if (events[n].filter == EVFILT_WRITE) {
                        evbitmap |= CURL_CSELECT_OUT;
                    }
                    socketHandler(events[n].ident, evbitmap);
                }
            }
        }
    }


  private:
    SigChan& m_chan;
    int m_kqueue;
    int m_runningHandles;
};
#endif

#if defined(WIN32) || defined(__linux__)
/* Event queue using epoll */
class EventQueue {
  public:
    EventQueue(SigChan& chan) : m_chan(chan) {
        m_epollfd = epoll_create1(0);
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = m_chan.getServer();
        epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_chan.getServer(), &ev);
    }
    ~EventQueue() {
#if defined(WIN32)
        epoll_close(m_epollfd);
#elif defined(__linux__)
        close(m_epollfd);
#endif
    }
    void setCurlMultiHandle(CURLM* h) {
        m_multiHd = h;
    }
    void configureSocket(curl_socket_t s, int what, void* socketp) {
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
            curl_multi_assign(m_multiHd, s, reinterpret_cast<void*>(~0));
            op = EPOLL_CTL_ADD;
        }
        // printf("socketCallback epoll_ctl s=%d op=%d events=%x\n", s, op, event.events);
        epoll_ctl(m_epollfd, op, s, &event);
    }
    template <typename TimeOutHdl, typename SockHdl, typename SigCHdl>
    void processEvents(int timeout, TimeOutHdl&& timeOutHandler, SockHdl&& socketHandler, SigCHdl&& sigChanHandler) {
        // printf("poll() sockets with timeout %d...\n", timeout);
        static constexpr size_t MaxEvents = 10;
        struct epoll_event events[MaxEvents];
        int pollres = epoll_wait(m_epollfd, events, MaxEvents, timeout);
        if (pollres < 0) {
            // error
            // printf("error\n");
        } else if (pollres == 0) {
            // printf("timeout\n");
            timeOutHandler();
        } else {
            for (int n = 0; n < pollres; ++n) {
                // printf("epoll_wait result: %d ... ", pollres);
                if (events[n].data.fd == m_chan.getServer()) {
                    // printf("  fd[%d] = %d m_wakeUpSrv\n", n, events[n].data.fd );
                    m_chan.drain();
                    sigChanHandler();
                } else {
                    // printf("  fd[%d] = %d events[%x]\n", n, events[n].data.fd, events[n].events);
                    int evbitmap = 0;
                    if (events[n].events & EPOLLIN) {
                        evbitmap |= CURL_CSELECT_IN;
                    }
                    if (events[n].events & EPOLLOUT) {
                        evbitmap |= CURL_CSELECT_OUT;
                    }
                    socketHandler(events[n].data.fd, evbitmap);
                }
            }
        }
    }

  private:
    SigChan& m_chan;
    CURLM* m_multiHd;
#if defined(WIN32)
    HANDLE m_epollfd;
#else
    int m_epollfd;
#endif
};

#endif


class Multi::Impl {
  public:
    Impl();
    ~Impl();

    void Submit(ReqUPtr req);
    void RunLoop();
    void RequestStop();

  private:
    SigChan m_chan;
    EventQueue m_queue;
    std::set<CURL*> m_easyHandles;

    std::mutex m_mutex;
    std::deque<ReqUPtr> m_requests;
    CURLM* m_multiHdl;
    bool m_stopRequested = false;

    bool m_deadlineActive = false;
    std::chrono::time_point<std::chrono::steady_clock> m_deadline;
    int m_runningHandles = 0;

    int TimerCallback(long timeout);
    void CompleteTransfer(CURL* hdl, CURLcode result);
};


Multi::Impl::Impl() : m_queue(m_chan) {
    m_multiHdl = curl_multi_init();
    assert(m_multiHdl);
    m_queue.setCurlMultiHandle(m_multiHdl);

    auto socket_callback = +[](CURL* easy, curl_socket_t s, int what, void* userp, void* socketp) {
        auto that = static_cast<Multi::Impl*>(userp);
        return that->m_queue.configureSocket(s, what, socketp);
    };
    auto timer_callback = +[](CURLM* m, long timeout, void* userp) {
        auto that = static_cast<Multi::Impl*>(userp);
        return that->TimerCallback(timeout);
    };

    curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERFUNCTION, timer_callback);
    curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERDATA, this);
}

Multi::Impl::~Impl() {
    curl_multi_cleanup(m_multiHdl);
}

void Multi::Impl::Submit(ReqUPtr req) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.push_back(std::move(req));
    m_chan.notify();
}

void Multi::Impl::RequestStop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopRequested = true;
    m_chan.notify();
}

// curl wants us to install a timer
int Multi::Impl::TimerCallback(long timeout) {
    // printf("timerCallback %d\n", timeout);
    if (timeout == -1) {
        m_deadlineActive = false;
    } else {
        m_deadlineActive = true;
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    }
    return 0;
}


void Multi::Impl::RunLoop() {
    while (!m_stopRequested) {
        // poll the sockets and the client requests fd
        int timeout = m_deadlineActive ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now()).count()) : -1;
        if (m_deadlineActive && (timeout <= 0)) {
            m_deadlineActive = false;
            curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
        } else {
            m_queue.processEvents(timeout,
                                  [&]() { // timeout
                                      curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
                                  },
                                  [&](curl_socket_t s, int evbitmap) { // socket event
                                      curl_multi_socket_action(m_multiHdl, s, evbitmap, &m_runningHandles);
                                  },
                                  [&]() { // signal from client
                                      std::lock_guard<std::mutex> lock(m_mutex);
                                      if (!m_requests.empty()) {
                                          for (auto& req : m_requests) {
                                              // the property of ReqP is transfered to the CURL handle
                                              ReqP* preq = req.release();
                                              CURL* curlHdl = preq->m_session.GetCurlHolder()->handle;
                                              curl_easy_setopt(curlHdl, CURLOPT_PRIVATE, preq);
                                              // printf("add request %p\n", preq );
                                              CURLMcode res = curl_multi_add_handle(m_multiHdl, curlHdl);
                                              m_easyHandles.insert(curlHdl);
                                          }
                                          m_requests.clear();
                                          curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
                                      }
                                  });
        }

        int msgsInQueue = 0;
        do {
            struct CURLMsg* m = curl_multi_info_read(m_multiHdl, &msgsInQueue);
            if (m && m->msg == CURLMSG_DONE) {
                CURL* eh = m->easy_handle;
                m_easyHandles.erase(eh);
                CompleteTransfer(eh, m->data.result);
            }
        } while (msgsInQueue);
    }

    for (auto eh : m_easyHandles) {
        CompleteTransfer(eh, CURLE_ABORTED_BY_CALLBACK);
    }
}

void Multi::Impl::CompleteTransfer(CURL* hdl, CURLcode result) {
    curl_multi_remove_handle(m_multiHdl, hdl);
    ReqP* transaction = nullptr;
    curl_easy_getinfo(hdl, CURLINFO_PRIVATE, &transaction);
    transaction->m_completeFn(transaction, result); // deletes transaction
}

Multi::Multi() : pimpl_(new Impl()) {}
Multi::~Multi() = default;
void Multi::RunLoop() {
    pimpl_->RunLoop();
}
void Multi::RequestStop() {
    pimpl_->RequestStop();
}
void Multi::Submit(ReqUPtr req) {
    pimpl_->Submit(std::move(req));
}
void Multi::Del::operator()(ReqP* p) {
    (p->m_completeFn)(p, CURLE_ABORTED_BY_CALLBACK);
}
