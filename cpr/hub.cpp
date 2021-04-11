#ifdef WIN32
#define NOMINMAX
#endif

#include "cpr/hub.h"

#include "curl/multi.h"
#include "cpr/session.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <deque>
#include <future>

#ifdef __linux__
#include <thread>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif


struct WorkItem
{
   void operator()(cpr::Response&& r)
   {
      printf("Request completed WorkItem: %s %d\n", r.url.c_str(), r.status_code);
   }
};

class MultiHttpClient::Impl
{
public:
   Impl();
   ~Impl();

   void Submit(std::unique_ptr<ReqP> req);
   void RunLoop();
   void RequestStop();

private:
   std::mutex m_mutex;
   std::deque<std::unique_ptr<ReqP>> m_requests;
   CURLM *m_multiHdl;
   bool m_stopRequested = false;

   bool m_deadlineActive = false;
   std::chrono::time_point<std::chrono::steady_clock> m_deadline;
   int m_runningHandles = 0;

   int socketCallback(curl_socket_t s, int what);
   int timerCallback(long timeout);
   bool performPoll(int timeout);

#ifdef WIN32
   /**
    * Array of WSA Events and their associated sockets.
    *
    * As Windows is limited to WSA_MAXIMUM_WAIT_EVENTS events, several sockets
    * may have to share an event.
    * - m_wsaEvHandle[0] is reserved for communication with the users
    * - m_wsaEvSockets[i] contains all sockets associated to the m_wsaEvHandle[i] event
    *
    * We use a very simple hashing scheme to assign a socket to an event:
    * - socket handles are always multiple of 4 https://devblogs.microsoft.com/oldnewthing/20050121-00/?p=36633
    *   so we get rid of the least significant 2 bits
    * - handles are assigned sequentially and freed handles are reused, so sockets are typically low numbers
    *   and we can use a simple modulo
    */
   static const size_t NbHandles = WSA_MAXIMUM_WAIT_EVENTS;
   std::array<WSAEVENT, NbHandles> m_wsaEvHandle;
   std::array<std::vector<SOCKET>, NbHandles> m_wsaEvSockets;
   static size_t HashSocket(SOCKET s)
   {
      return (s >> 2) % (NbHandles - 1) + 1;
   }
#elif __linux__
   std::vector<pollfd> m_pollfds;
   int m_wakeupEvt;
#endif
};


MultiHttpClient::Impl::Impl()
{
#ifdef WIN32
   for (auto& evt : m_wsaEvHandle)
   {
      evt = WSACreateEvent();
      if (evt == INVALID_HANDLE_VALUE)
      {
         throw(std::runtime_error("WSACreateEvent failure"));
      }
   }
#elif __linux__
   m_wakeupEvt = eventfd(0, 0);
   m_pollfds.push_back({ m_wakeupEvt, POLLIN, 0 });
#endif

   m_multiHdl = curl_multi_init();
   assert(m_multiHdl);

   auto socket_callback = +[](CURL *easy, curl_socket_t s, int what, void *userp, void * /*socketp*/) {
      auto that = static_cast<MultiHttpClient::Impl *>(userp);
      return that->socketCallback(s, what);
   };
   auto timer_callback = +[](CURLM *m, long timeout, void *userp) {
      auto that = static_cast<MultiHttpClient::Impl *>(userp);
      return that->timerCallback(timeout);
   };

   curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETFUNCTION, socket_callback);
   curl_multi_setopt(m_multiHdl, CURLMOPT_SOCKETDATA, this);
   curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERFUNCTION, timer_callback);
   curl_multi_setopt(m_multiHdl, CURLMOPT_TIMERDATA, this);
}

MultiHttpClient::Impl::~Impl()
{
   curl_multi_cleanup(m_multiHdl);
#ifdef WIN32
   for (auto evt : m_wsaEvHandle)
   {
      WSACloseEvent(evt);
   }
#elif __linux__
   close(m_wakeupEvt);
#endif
}

void MultiHttpClient::Impl::Submit(std::unique_ptr<ReqP> req)
{
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_requests.push_back(std::move(req));
#ifdef WIN32
      WSASetEvent(m_wsaEvHandle[0]);
#elif __linux__
      uint64_t v = 1;
      write(m_wakeupEvt, &v, sizeof(v));
#endif
   }
}

void MultiHttpClient::Impl::RequestStop()
{
   {
      m_stopRequested = true;
#ifdef WIN32
      WSASetEvent(m_wsaEvHandle[0]);
#elif __linux__
      uint64_t v = 1;
      write(m_wakeupEvt, &v, sizeof(v));
#endif
   }
}

// curl wants us to install a timer
int MultiHttpClient::Impl::timerCallback(long timeout)
{
   //printf("timerCallback %d\n", timeout);
   if (timeout == -1)
   {
      m_deadlineActive = false;
   }
   else
   {
      m_deadlineActive = true;
      m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
   }
   return 0;
}

// curl wants us to poll a socket
#ifdef WIN32
int MultiHttpClient::Impl::socketCallback(curl_socket_t s, int what)
{
   long lNetworkEvents = 0;
   switch (what)
   {
   case CURL_POLL_IN:
      lNetworkEvents = FD_READ;
      break;
   case CURL_POLL_OUT:
      lNetworkEvents = FD_WRITE | FD_CONNECT;
      break;
   case CURL_POLL_INOUT:
      lNetworkEvents = FD_READ | FD_WRITE | FD_CONNECT;
      break;
   case CURL_POLL_REMOVE:
      lNetworkEvents = 0;
      break;
   }

   int evtIndex = HashSocket(s);
   std::vector<SOCKET>& sockets = m_wsaEvSockets[evtIndex];
   auto iterSocket = std::find(sockets.begin(), sockets.end(), s);
   bool unknownSocket = (iterSocket == sockets.end());
   if (lNetworkEvents)
   {
      if (unknownSocket)
      {
         sockets.push_back(s);
      }
   }
   else
   {
      if (unknownSocket)
      {
         return 0;
      }
      sockets.erase(iterSocket);
   }
   WSAEventSelect(s, m_wsaEvHandle[evtIndex], lNetworkEvents);

   //printf( "WSAEventSelect socket[%d] evtIndex[%d] nwevt[%08x]\n", s, evtIndex, lNetworkEvents );
   return 0;
}

/**
 * @returns true if new client requests shall be examined
 */
bool MultiHttpClient::Impl::performPoll(int timeout)
{
   //printf("WSAWaitForMultipleEvents timeout %d ...\n", timeout);
   DWORD waitres = WSAWaitForMultipleEvents(m_wsaEvHandle.size(), m_wsaEvHandle.data(),
      FALSE /*waitAll*/, timeout, FALSE /*fAlertable*/);

   bool result = false;
   if (waitres == WSA_WAIT_TIMEOUT)
   {
      //printf("                         timed out\n");
      curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
   }
   else if (waitres == WSA_WAIT_EVENT_0)
   {
      WSAResetEvent(m_wsaEvHandle[0]);
      result = true;
   }
   if ((waitres > WSA_WAIT_EVENT_0) && (waitres < WSA_WAIT_EVENT_0 + m_wsaEvHandle.size()))
   {
      int index = waitres - WSA_WAIT_EVENT_0;
      WSAResetEvent(m_wsaEvHandle[index]);
      for (SOCKET socket : m_wsaEvSockets[index])
      {
         WSANETWORKEVENTS  nwevent;
         WSAEnumNetworkEvents(socket, nullptr, &nwevent);
         //printf("                         index %d socket %d nwevent %x\n", index, socket, nwevent.lNetworkEvents);
         int evbitmap = 0;
         if (nwevent.lNetworkEvents &  FD_READ)
         {
            evbitmap |= CURL_CSELECT_IN;
         }
         if (nwevent.lNetworkEvents &  (FD_WRITE | FD_CONNECT))
         {
            evbitmap |= CURL_CSELECT_OUT;
         }
         curl_multi_socket_action(m_multiHdl, socket, evbitmap, &m_runningHandles);
      }
   }
   return result;
}
#endif

#ifdef __linux
int MultiHttpClient::Impl::socketCallback(curl_socket_t s, int what)
{
   //printf("socketCallback %d %d\n", s, what);
   auto i = std::find_if(m_pollfds.begin() + 1, m_pollfds.end(), [s](const pollfd& pfd) { return  s == pfd.fd; });
   if (what == CURL_POLL_IN || what == CURL_POLL_OUT || what == CURL_POLL_INOUT)
   {
      if (i == m_pollfds.end())
      {
         m_pollfds.push_back({ s, 0, 0 });
         i = m_pollfds.end() - 1;
      }
      size_t index = std::distance(m_pollfds.begin(), i);
      switch (what)
      {
      case CURL_POLL_IN:
         i->events = POLLIN;
         break;
      case CURL_POLL_OUT:
         i->events = POLLOUT;
         break;
      case CURL_POLL_INOUT:
         i->events = POLLIN | POLLOUT;
         break;
      }
   }
   else if (what == CURL_POLL_REMOVE)
   {
      if (i != m_pollfds.end())
      {
         m_pollfds.erase(i);
      }
   }

   return 0;
}

/**
 * @returns true if new client requests shall be examined
 */
bool MultiHttpClient::Impl::performPoll(int timeout)
{
   //printf("poll() %u sockets with timeout %d... ", m_pollfds.size(), timeout);
   int pollres = poll(m_pollfds.data(), m_pollfds.size(), timeout);

   bool result = false;
   if (pollres < 0)
   {
      // error
      //printf("error\n");
   }
   else if (pollres == 0)
   {
      //printf("timeout\n");
      curl_multi_socket_action(m_multiHdl, CURL_SOCKET_TIMEOUT, 0, &m_runningHandles);
   }
   else
   {
      pollfd* pfd = &m_pollfds[0];

      // fd[0] is requests from clients
      if (pfd->revents != 0)
      {
         uint64_t v;
         read(m_wakeupEvt, &v, sizeof(v));
         //printf("client ");
         result = true;
         pfd->revents = 0;
         --pollres;
      }
      ++pfd;

      // next fd are sockets
      for (auto pollfdend = &m_pollfds[m_pollfds.size()]; (pfd < pollfdend) && pollres; ++pfd, --pollres)
      {
         if (pfd->revents != 0)
         {
            //printf("fd[%d] ", pfd->fd);
            int evbitmap = 0;
            if (pfd->revents & POLLIN)
            {
               evbitmap |= CURL_CSELECT_IN;
            }
            if (pfd->revents &  POLLOUT)
            {
               evbitmap |= CURL_CSELECT_OUT;
            }
            pfd->revents = 0;
            curl_multi_socket_action(m_multiHdl, pfd->fd, evbitmap, &m_runningHandles);
         }
      }
      //printf("\n");
   }

   return result;
}
#endif

void MultiHttpClient::Impl::RunLoop()
{
   for (;; )
   {
      int prevRunningHandles = m_runningHandles;

      // poll the sockets and the client requets fd
      int timeout = -1;
      if (m_deadlineActive)
      {
         m_deadlineActive = false;
         timeout = static_cast<int>(std::max(std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now()),
            std::chrono::milliseconds(1)).count());
      }
      bool clientreq = performPoll(timeout);

      // evaluate new requests from our clients
      if (clientreq)
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         for (auto& req : m_requests)
         {
            // the property of ReqP is transfered to the CURL handle
            ReqP* preq = req.release();
            CURL* curlHdl = preq->m_session.GetCurlHolder()->handle;
            curl_easy_setopt(curlHdl, CURLOPT_PRIVATE, preq);

            //printf("add request %p\n", preq );
            CURLMcode res = curl_multi_add_handle(m_multiHdl, curlHdl);
         }
         m_requests.clear();
      }

      // terminate completed transfers
      //printf("runningHandles %d\n", m_runningHandles);
      int msgsInQueue = 0;
      do
      {
         struct CURLMsg *m = curl_multi_info_read(m_multiHdl, &msgsInQueue);
         if (m && m->msg == CURLMSG_DONE)
         {
            m->data.result;
            m->data.whatever;
            CURL *e = m->easy_handle;
            curl_multi_remove_handle(m_multiHdl, e);

            // complete and delete the transaction
            ReqP* transaction;
            curl_easy_getinfo(e, CURLINFO_PRIVATE, &transaction);
            transaction->m_completeFn(transaction, m->data.result);
         }
      } while (msgsInQueue);

      if ( 0 == m_runningHandles )
      {
         printf("No running handles\n");
      }
   }
}


MultiHttpClient::MultiHttpClient() : pimpl_(new Impl()) {}
MultiHttpClient::~MultiHttpClient() = default;
void MultiHttpClient::RunLoop() { pimpl_->RunLoop(); }
void MultiHttpClient::RequestStop() { pimpl_->RequestStop(); }
void MultiHttpClient::Submit(std::unique_ptr<ReqP> req) { pimpl_->Submit(std::move(req)); }
//Session::Session(Session&& old) noexcept = default;
//Session& Session::operator=(Session&& old) noexcept = default;



void main_s()
{
   MultiHttpClient engine;
   std::thread thread(&MultiHttpClient::RunLoop, &engine);

   /*
   printf("top depart\n");
   for (unsigned int i = 0; i < 40; ++i)
   {
      engine.GetDetached(WorkItem{}, cpr::Url{ "http://127.0.0.1:1234" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
      engine.GetDetached(WorkItem{}, cpr::Url{ "http://127.0.0.1" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
      engine.GetDetached(WorkItem{}, cpr::Url{ "http://127.0.0.1/ah_que_coucou" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
      engine.GetDetached(WorkItem{}, cpr::Url{ "http://nonexistent.nonexistent/" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
   }
   */
   /////////////////////////////////////////////////

   auto l = [](cpr::Response&& r)
   {
      printf( "Request 1 completed %d %s\n", r.error.code, r.status_line.c_str() );
      return 42;
   };
   auto r = engine.GetCallback(l,
      cpr::Url{ "http://127.0.0.1" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
   int x = r.get();

   /////////////////////////////////////////////////

   auto l2 = [](cpr::Response&& r)
   {
      printf( "Request 2 completed %d %s\n", r.error.code, r.status_line.c_str() );
   };
   auto r2 = engine.GetCallback(l2,
      cpr::Url{ "http://127.0.0.1" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });
   r2.wait();

   /////////////////////////////////////////////////


   auto l3 = [](cpr::Response&& r) -> void
   {
      printf( "Request 3 completed %d %s\n", r.error.code, r.status_line.c_str() );
   };
   engine.GetDetached(l3,
      cpr::Url{ "http://192.168.163.128" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });

   /////////////////////////////////////////////////

   engine.GetDetached(WorkItem(),
      cpr::Url{ "http://192.168.163.128" }, cpr::ConnectTimeout{ std::chrono::seconds{2} });

   /////////////////////////////////////////////////

   cpr::Session s4;
   s4.SetUrl( "http://192.168.163.128" );
   s4.PrepareGet();
   engine.PerformDetached([](cpr::Session&& session, CURLcode code) {
      auto r = session.Complete( code );
      printf( "Perform detached %s\n", r.status_line.c_str());
   }, std::move(s4) );

   thread.join();
   puts("fin\n");
}


