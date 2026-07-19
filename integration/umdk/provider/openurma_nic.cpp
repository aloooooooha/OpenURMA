// SPDX-License-Identifier: Apache-2.0
//
// openurma_nic (SC backend) — drives one in-process OpenURMA SystemC NIC and
// carries the UB "wire" to a peer over a UNIX stream socket.
//
// Threading model (so one-sided ops work even against a *passive* responder
// that makes no verb calls):
//   - The app thread owns the SystemC kernel: submit_wr/poll_cqe/pump touch the
//     NIC directly and call sc_start. Only this thread touches SystemC.
//   - A background thread per NIC owns the wire socket. It ships outbound flit/
//     data frames and ingests inbound ones, exchanging flits with the SC kernel
//     ONLY through lock-protected queues (never touching SystemC itself), and
//     invokes the provider's data callback for inbound RDMA payload frames.
// This lets a passive responder service one-sided WRITE/READ and SEND purely on
// the background thread (memcpy + completion queue), while the initiator drives
// the SC pipeline for protocol + timing.
//
// Env: OPENURMA_WIRE_PATH + OPENURMA_WIRE_ROLE (listen|connect); absent → self-loop.

#include <systemc.h>
#include "openclicknp/flit.hpp"
#include <ostream>
#include <string>
namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&, const std::string&) {}

#include "openurma/openurma_sc_facade.hpp"
#include "openurma/ub_flit.hpp"
#include "openurma_nic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace {
int g_log_level = -1;
inline int provider_log_level()
{
    if (g_log_level < 0) {
        const char *value = getenv("OPENURMA_PROVIDER_LOG");
        if (value == nullptr || *value == '\0' || strcmp(value, "0") == 0 ||
            strcmp(value, "off") == 0 || strcmp(value, "error") == 0) {
            g_log_level = 0;
        } else if (strcmp(value, "1") == 0 || strcmp(value, "summary") == 0) {
            g_log_level = 1;
        } else if (strcmp(value, "trace") == 0) {
            g_log_level = 2;
        } else {
            fprintf(stderr, "[openurma-nic] invalid OPENURMA_PROVIDER_LOG=%s; using off\n", value);
            g_log_level = 0;
        }
    }
    return g_log_level;
}
#define NLOG(...) do{if(provider_log_level()>=1){fprintf(stderr,"[openurma-nic] " __VA_ARGS__);fputc('\n',stderr);}}while(0)
typedef void (*data_cb_t)(void*, uint8_t, const uint8_t*, uint32_t);
}

struct openurma_nic {
    openurma::sc::NIC* nic = nullptr;
    std::string rx_path, tx_path; int role = 0;  // 0 self-loop,1 listen,2 connect
    int rx_fd = -1;                                // bound DGRAM socket (our inbox)
    std::atomic<bool> stop{false};
    std::atomic<int> fatal_errno{0};
    std::atomic<size_t> pending_tx{0};
    std::thread bg;
    std::mutex lk;
    std::deque<std::vector<uint8_t>> tx_q;    // outbound frames [type][payload]
    std::deque<openclicknp::flit_t> rx_flits;  // inbound 'F' flits for the app/SC
    data_cb_t cb = nullptr; void* cb_user = nullptr;
    long wtx=0, wrx=0;
    uint32_t local_cna=0;
};

static constexpr size_t MAX_TX_QUEUE_FRAMES = 65536;
static constexpr size_t MAX_WIRE_FRAME_BYTES = 1u << 16;

static void mark_wire_failed(openurma_nic* h, int error, size_t bytes){
    if(error==0) error=EIO;
    int expected=0;
    if(h->fatal_errno.compare_exchange_strong(expected,error,std::memory_order_relaxed)){
        h->pending_tx.store(0,std::memory_order_relaxed);
        fprintf(stderr,"[openurma-nic] wire failure bytes=%zu errno=%d (%s)\n",
                bytes,error,strerror(error));
    }
}

// SystemC does not allow construction of new modules after simulation starts.
// Keep one inactive NIC runtime per process and reuse it across sequential
// contexts. The provider enforces a single active context for this backend.
static std::mutex g_runtime_lk;
static openurma_nic* g_runtime_nic=nullptr;
static unsigned g_runtime_refs=0;

// UNIX-datagram wire: each NIC binds its own rx_path and sendto()s the peer's
// tx_path. One frame = one datagram (boundaries preserved), so there is no
// length framing and no connection rendezvous — robust against startup races.
// Frame: [1 byte type]['F'→64B flit | 'D'→[1B tag][payload]].
static void bg_run(openurma_nic* h){
    uint8_t buf[1<<16];
    while(!h->stop){
        std::deque<std::vector<uint8_t>> out;
        { std::lock_guard<std::mutex> g(h->lk); out.swap(h->tx_q); }
        for (auto& f : out){
            if(h->fatal_errno.load(std::memory_order_relaxed)!=0) break;
            sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,h->tx_path.c_str(),sizeof(a.sun_path)-1);
            bool sent=false;
            for(int t=0;t<2000;t++){ ssize_t w=sendto(h->rx_fd,f.data(),f.size(),0,(sockaddr*)&a,sizeof a);
                if(w==(ssize_t)f.size()){
                    h->wtx++; h->pending_tx.fetch_sub(1,std::memory_order_relaxed);
                    sent=true; break;
                }
                if(w>=0){ mark_wire_failed(h,EMSGSIZE,f.size()); break; }
                if(errno==ENOENT||errno==ECONNREFUSED||errno==EAGAIN||errno==EWOULDBLOCK){ usleep(200); continue; }
                mark_wire_failed(h,errno,f.size());
                break; }
            if(!sent && h->fatal_errno.load(std::memory_order_relaxed)==0)
                mark_wire_failed(h,ETIMEDOUT,f.size());
        }
        bool any=false;
        for(;;){ ssize_t n=recv(h->rx_fd,buf,sizeof buf,0); if(n<=0) break; any=true;
            uint8_t type=buf[0];
            if(type=='F'&&n==65){ openclicknp::flit_t rf; memcpy(rf.raw.data(),buf+1,64);
                std::lock_guard<std::mutex> g(h->lk); h->rx_flits.push_back(rf); h->wrx++; }
            else if(type=='D'&&n>=2){
                data_cb_t cb=nullptr; void* user=nullptr;
                { std::lock_guard<std::mutex> g(h->lk); cb=h->cb; user=h->cb_user; }
                if(cb) cb(user, buf[1], buf+2, (uint32_t)n-2);
            }
        }
        if(!any && out.empty()) usleep(50);
    }
}

static void sc_init_once(openurma_nic* h){
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
    h->nic->configure_mr_permissive();
}

extern "C" struct openurma_nic* openurma_nic_create(uint32_t local_cna){
    std::lock_guard<std::mutex> runtime_guard(g_runtime_lk);
    if(g_runtime_nic){
        if(g_runtime_refs!=0 || g_runtime_nic->local_cna!=local_cna ||
           g_runtime_nic->fatal_errno.load(std::memory_order_relaxed)!=0){
            NLOG("reject concurrent/different NIC cna=0x%06x active_refs=%u",local_cna,g_runtime_refs);
            return nullptr;
        }
        g_runtime_refs=1;
        NLOG("reuse cna=0x%06x role=%d rx=%s tx=%s",local_cna,g_runtime_nic->role,
             g_runtime_nic->rx_path.c_str(),g_runtime_nic->tx_path.c_str());
        return g_runtime_nic;
    }
    auto* h=new openurma_nic();
    h->local_cna=local_cna;
    openurma::sc::NICConfig cfg; cfg.local_cna=local_cna;
    h->nic=new openurma::sc::NIC("openurma_nic",cfg);
    const char* wp=getenv("OPENURMA_WIRE_PATH"); const char* wr=getenv("OPENURMA_WIRE_ROLE");
    if(wp&&*wp){
        bool listen = wr && std::string(wr)=="listen";
        h->role = listen?1:2;
        std::string base = wp;
        h->rx_path = base + (listen?".listen":".connect");
        h->tx_path = base + (listen?".connect":".listen");
        h->rx_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if(h->rx_fd<0 || h->rx_path.size()>=sizeof(sockaddr_un{}.sun_path) ||
           h->tx_path.size()>=sizeof(sockaddr_un{}.sun_path)){
            if(h->rx_fd>=0) close(h->rx_fd);
            delete h->nic; delete h; return nullptr;
        }
        int bufsz = 8*1024*1024;
        setsockopt(h->rx_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
        setsockopt(h->rx_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
        sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,h->rx_path.c_str(),sizeof(a.sun_path)-1);
        unlink(h->rx_path.c_str());
        if(bind(h->rx_fd,(sockaddr*)&a,sizeof a)<0){
            close(h->rx_fd); unlink(h->rx_path.c_str()); delete h->nic; delete h; return nullptr;
        }
        int fl=fcntl(h->rx_fd,F_GETFL,0); fcntl(h->rx_fd,F_SETFL,fl|O_NONBLOCK);
    } else h->role=0;
    sc_init_once(h);
    if(h->role!=0) h->bg=std::thread(bg_run,h);
    g_runtime_nic=h; g_runtime_refs=1;
    NLOG("create cna=0x%06x role=%d rx=%s tx=%s",local_cna,h->role,h->rx_path.c_str(),h->tx_path.c_str());
    return h;
}

extern "C" void openurma_nic_destroy(struct openurma_nic* h){
    if(!h) return;
    std::lock_guard<std::mutex> runtime_guard(g_runtime_lk);
    if(h!=g_runtime_nic || g_runtime_refs==0){
        NLOG("destroy ignored for inactive/unknown NIC");
        return;
    }
    // A completed WR must have put its data on the wire: wait for the bg thread
    // to drain the tx queue, then linger so the peer can read it.
    for(int t=0;t<20000;t++){ size_t q; { std::lock_guard<std::mutex> g(h->lk); q=h->tx_q.size(); } if(q==0) break; usleep(100); }
    usleep(50000);
    { std::lock_guard<std::mutex> g(h->lk); h->cb=nullptr; h->cb_user=nullptr; }
    g_runtime_refs=0;
    NLOG("release-for-reuse wtx=%ld wrx=%ld",h->wtx,h->wrx);
}

extern "C" int openurma_nic_shutdown_runtime(void){
    std::lock_guard<std::mutex> runtime_guard(g_runtime_lk);
    if(g_runtime_refs!=0) return 0;
    openurma_nic* h=g_runtime_nic;
    if(!h) return 1;
    g_runtime_nic=nullptr;
    h->stop=true; if(h->bg.joinable()) h->bg.join();
    if(h->rx_fd>=0){ close(h->rx_fd); unlink(h->rx_path.c_str()); h->rx_fd=-1; }
    delete h->nic; h->nic=nullptr;
    delete h;
    NLOG("runtime shutdown");
    return 1;
}

// True once all queued outbound frames have been shipped by the bg thread.
extern "C" int openurma_nic_tx_drained(struct openurma_nic* h){
    if(!h || h->fatal_errno.load(std::memory_order_relaxed)!=0) return 0;
    return h->pending_tx.load(std::memory_order_relaxed)==0 ? 1 : 0;
}

extern "C" int openurma_nic_failed(struct openurma_nic* h){
    return h ? h->fatal_errno.load(std::memory_order_relaxed) : EINVAL;
}

extern "C" int openurma_nic_submit_wr(struct openurma_nic* h, const uint8_t flit[64]){
    if(!h || !flit || h->fatal_errno.load(std::memory_order_relaxed)!=0) return 0;
    openclicknp::flit_t f; memcpy(f.raw.data(),flit,64); return h->nic->submit_wr(f)?1:0;
}
extern "C" int openurma_nic_poll_cqe(struct openurma_nic* h, uint8_t out[64]){
    openclicknp::flit_t f; if(h->nic->pop_cqe(f)){ memcpy(out,f.raw.data(),64); return 1; } return 0;
}

extern "C" void openurma_nic_pump(struct openurma_nic* h, uint64_t budget_ns){
    openclicknp::flit_t f;
    if(h->role==0){ while(h->nic->pop_wire_tx(f)) h->nic->push_wire_rx(f); }
    else {
        // app thread ↔ SC ↔ bg-thread queues (no socket here)
        while(h->nic->pop_wire_tx(f)){
            if(h->fatal_errno.load(std::memory_order_relaxed)!=0) break;
            std::vector<uint8_t> v(1+64); v[0]='F'; memcpy(v.data()+1,f.raw.data(),64);
            std::lock_guard<std::mutex> g(h->lk);
            if(h->tx_q.size()>=MAX_TX_QUEUE_FRAMES){ mark_wire_failed(h,ENOBUFS,v.size()); break; }
            h->tx_q.push_back(std::move(v)); h->pending_tx.fetch_add(1,std::memory_order_relaxed);
        }
        for(;;){ openclicknp::flit_t rf; { std::lock_guard<std::mutex> g(h->lk); if(h->rx_flits.empty())break; rf=h->rx_flits.front(); h->rx_flits.pop_front(); } h->nic->push_wire_rx(rf); }
    }
    if(budget_ns==0) budget_ns=1;
    sc_core::sc_start((double)budget_ns, sc_core::SC_NS);
}

extern "C" int openurma_nic_data_send(struct openurma_nic* h, uint8_t tag, const void* buf, uint32_t len){
    if(!h || (len!=0 && !buf) || (size_t)len+2>MAX_WIRE_FRAME_BYTES ||
       h->fatal_errno.load(std::memory_order_relaxed)!=0) return 0;
    if(h->role==0){ if(h->cb) h->cb(h->cb_user,tag,(const uint8_t*)buf,len); return 1; }
    std::vector<uint8_t> v; v.reserve(2+len); v.push_back('D'); v.push_back(tag);
    v.insert(v.end(),(const uint8_t*)buf,(const uint8_t*)buf+len);
    std::lock_guard<std::mutex> g(h->lk);
    if(h->tx_q.size()>=MAX_TX_QUEUE_FRAMES) return 0;
    h->tx_q.push_back(std::move(v)); h->pending_tx.fetch_add(1,std::memory_order_relaxed); return 1;
}
// data_recv retained for ABI; with the callback model it returns nothing.
extern "C" int openurma_nic_data_recv(struct openurma_nic* h, uint8_t* tag, void* buf, uint32_t maxlen){
    (void)h;(void)tag;(void)buf;(void)maxlen; return 0;
}
extern "C" void openurma_nic_set_data_cb(struct openurma_nic* h, data_cb_t cb, void* user){
    if(!h) return;
    std::lock_guard<std::mutex> g(h->lk); h->cb=cb; h->cb_user=user;
}
