// SPDX-License-Identifier: Apache-2.0
// RM/CTP verbs-functional simulator. Not the legacy RTP SystemC wire/timing model.
#include "openurma_nic.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct openurma_nic {
    int fd=-1;
    std::string path,peer;
    std::atomic<bool> stop{false};
    std::atomic<int> error{0};
    std::thread rx;
    std::mutex mutex;
    openurma_data_cb_t cb=nullptr;
    void* user=nullptr;
};
static std::atomic<unsigned> active{0};
extern "C" openurma_nic* openurma_nic_create(uint32_t) {
    const char* base=getenv("OPENURMA_WIRE_PATH");
    const char* role=getenv("OPENURMA_WIRE_ROLE");
    if(!base||!role) {errno=EINVAL;return nullptr;}
    bool a=strcmp(role,"listen")==0;
    if(!a&&strcmp(role,"connect")!=0){errno=EINVAL;return nullptr;}
    auto* n=new openurma_nic;
    n->path=std::string(base)+(a?".a":".b");n->peer=std::string(base)+(a?".b":".a");
    sockaddr_un addr{};addr.sun_family=AF_UNIX;
    if(n->path.size()>=sizeof(addr.sun_path)||n->peer.size()>=sizeof(addr.sun_path)) {
        delete n;errno=ENAMETOOLONG;return nullptr;
    }
    n->fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(n->fd<0){delete n;return nullptr;}
    int size=4*1024*1024;
    setsockopt(n->fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
    setsockopt(n->fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size));
    strcpy(addr.sun_path,n->path.c_str());
    // Refuse an existing path; do not unlink a possibly live experiment.
    if(bind(n->fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0) {
        close(n->fd);delete n;return nullptr;
    }
    ++active;
    n->rx=std::thread([n]{
        std::vector<uint8_t> bytes(65536);
        while(!n->stop.load()) {
            pollfd p{n->fd,POLLIN,0};
            int ready=poll(&p,1,10);
            if(ready<0){if(errno==EINTR)continue;n->error=errno;break;}
            if(ready==0)continue;
            ssize_t len=recv(n->fd,bytes.data(),bytes.size(),0);
            if(len<0){if(errno==EAGAIN||errno==EINTR)continue;n->error=errno;break;}
            if(len<1){n->error=EPROTO;break;}
            std::lock_guard<std::mutex> guard(n->mutex);
            if(n->cb)n->cb(n->user,bytes[0],bytes.data()+1,static_cast<uint32_t>(len-1));
            else {n->error=EPROTO;break;}
        }
    });
    return n;
}
extern "C" void openurma_nic_destroy(openurma_nic* n) {
    if(!n)return;
    n->stop=true;if(n->rx.joinable())n->rx.join();
    close(n->fd);unlink(n->path.c_str());delete n;--active;
}
extern "C" int openurma_nic_shutdown_runtime(void){return active.load()==0;}
extern "C" int openurma_nic_failed(openurma_nic* n){return n?n->error.load():EINVAL;}
extern "C" int openurma_nic_tx_drained(openurma_nic* n){return n&&n->error.load()==0;}
extern "C" void openurma_nic_set_data_cb(openurma_nic* n,openurma_data_cb_t cb,void* user) {
    std::lock_guard<std::mutex> guard(n->mutex);n->cb=cb;n->user=user;
}
extern "C" int openurma_nic_data_send(openurma_nic* n,uint8_t tag,const void* buf,uint32_t len) {
    if(!n||n->error.load()!=0||len>65530||(!buf&&len))return 0;
    std::vector<uint8_t> bytes(len+1);bytes[0]=tag;
    if(len)memcpy(bytes.data()+1,buf,len);
    sockaddr_un peer{};peer.sun_family=AF_UNIX;strcpy(peer.sun_path,n->peer.c_str());
    ssize_t sent=sendto(n->fd,bytes.data(),bytes.size(),MSG_NOSIGNAL,
        reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
    if(sent==static_cast<ssize_t>(bytes.size()))return 1;
    if(sent<0&&(errno==EAGAIN||errno==EINTR))return 0;
    n->error=sent<0?errno:EIO;return 0;
}
extern "C" int openurma_nic_data_recv(openurma_nic*,uint8_t*,void*,uint32_t){return 0;}
// Calling the old RTP flit path is a hard failure in this simulation profile.
extern "C" int openurma_nic_submit_wr(openurma_nic* n,const uint8_t*) {
    n->error=EPROTONOSUPPORT;return 0;
}
extern "C" int openurma_nic_poll_cqe(openurma_nic*,uint8_t[64]){return 0;}
extern "C" void openurma_nic_pump(openurma_nic*,uint64_t){}
