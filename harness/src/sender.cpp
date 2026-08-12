// sender.cpp — plain-UDP baseline transport, source side.
//
// Reads events from the producer's shared-memory ring and fans them out over
// UDP to one or more destinations, unchanged (raw harness frame as payload,
// one message per datagram). No FEC, no retransmission, no metrics: this is
// meant as the transport floor to build FEC / counters on top of.
//
// Performance notes:
//  - Ring draining and sendmmsg() are opportunistic, not batched-by-timer: we
//    pull whatever is immediately ready (up to --batch) and flush right away.
//    Under low load that's effectively one message per syscall (no added
//    latency); under bursty load the ring naturally has several messages
//    ready and we amortize the syscall over all of them and every dst.
//  - One sendmmsg() call emits (messages_drained * num_dst) datagrams.
//  - Busy-spins on empty ring (_mm_pause() in the idle branch, same pattern
//    as shm_ring.h's own advice) instead of blocking/sleeping.
//  - No per-packet heap allocation on the hot path; all buffers are
//    pre-sized from --batch at startup.
//
// Usage:
//   sender --shm NAME [--slots N] --dst HOST:PORT [--dst HOST:PORT ...]
//          [--from-edge | --from-start] [--batch N] [--sndbuf BYTES]
//          [--count N] [--idle-ms MS]
//
// Fan-out example (1 source -> 3 receivers):
//   taskset -c 3 ./bin/sender --shm /fanout_ring --from-edge --dst 10.0.1.11:9000 --dst 10.0.1.12:9000 --dst 10.0.1.13:9000
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // sendmmsg() is a GNU extension, not in plain POSIX headers
#endif
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() ((void)0)
#endif

#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  std::vector<std::string> dsts;
  bool from_edge = false;
  uint32_t batch = 64;
  int sndbuf = 8 << 20;  // 8 MiB
  uint64_t count = 0;    // 0 = unlimited
  uint64_t idle_ms = 0;  // 0 = disabled; see shm_ring producer/consumer note
                         // on why auto-exit-on-idle defaults off for burst
                         // traffic patterns.
};

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--dst") c.dsts.push_back(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--from-start") c.from_edge = false;
    else if (a == "--batch") c.batch = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--sndbuf") c.sndbuf = std::stoi(next());
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.dsts.empty()) {
    fprintf(stderr, "need at least one --dst HOST:PORT\n");
    std::exit(2);
  }
  if (c.batch == 0) c.batch = 1;
  return c;
}

bool resolve(const std::string& hostport, sockaddr_in* out) {
  auto pos = hostport.rfind(':');
  if (pos == std::string::npos) {
    fprintf(stderr, "bad --dst %s (want host:port)\n", hostport.c_str());
    return false;
  }
  std::string host = hostport.substr(0, pos);
  std::string port = hostport.substr(pos + 1);

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    fprintf(stderr, "cannot resolve %s\n", hostport.c_str());
    return false;
  }
  *out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
  freeaddrinfo(res);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::vector<sockaddr_in> dst_addrs;
  dst_addrs.reserve(cfg.dsts.size());
  for (auto& d : cfg.dsts) {
    sockaddr_in sa{};
    if (!resolve(d, &sa)) return 1;
    dst_addrs.push_back(sa);
  }

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) { perror("socket"); return 1; }
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &cfg.sndbuf, sizeof(cfg.sndbuf));

  shm::Segment seg = shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots),
                                         /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t lapped_events = 0, sent_msgs = 0, sent_pkts = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  const uint32_t B = cfg.batch;
  const size_t N = dst_addrs.size();
  std::vector<uint8_t> bufs(static_cast<size_t>(B) * shm::kFrameCap);
  std::vector<iovec> iovs(B);
  std::vector<mmsghdr> msgs(static_cast<size_t>(B) * N);

  while (!g_stop && (cfg.count == 0 || sent_msgs < cfg.count)) {
    uint32_t got = 0;
    uint32_t want = B;
    if (cfg.count != 0) {
      uint64_t remaining = cfg.count - sent_msgs;
      if (remaining < want) want = static_cast<uint32_t>(remaining);
    }
    while (got < want) {
      uint32_t len = 0;
      uint64_t resume = 0;
      uint8_t* slot = &bufs[static_cast<size_t>(got) * shm::kFrameCap];
      auto st = ring.read(read_index, slot, &len, &resume);
      if (st == shm::Ring::FrameStatus::kOk) {
        iovs[got].iov_base = slot;
        iovs[got].iov_len = len;
        ++got;
        ++read_index;
      } else if (st == shm::Ring::FrameStatus::kLapped) {
        ++lapped_events;
        read_index = resume;
      } else {
        break;  // kEmpty: nothing more ready right now, flush what we have
      }
    }

    size_t m = 0;
    for (uint32_t i = 0; i < got; ++i) {
      for (size_t d = 0; d < N; ++d) {
        msghdr& h = msgs[m].msg_hdr;
        h.msg_name = &dst_addrs[d];
        h.msg_namelen = sizeof(sockaddr_in);
        h.msg_iov = &iovs[i];
        h.msg_iovlen = 1;
        h.msg_control = nullptr;
        h.msg_controllen = 0;
        h.msg_flags = 0;
        msgs[m].msg_len = 0;
        ++m;
      }
    }

    size_t off = 0;
    while (off < m) {
      int rc = sendmmsg(sock, msgs.data() + off, static_cast<unsigned>(m - off), 0);
      if (rc < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN) { CPU_PAUSE(); continue; }
        perror("sendmmsg");
        break;
      }
      off += static_cast<size_t>(rc);
    }
    sent_msgs += got;
    sent_pkts += m;
  }

  fprintf(stderr,
          "sender: sent %llu messages (%llu packets) to %zu dst(s), lapped %llu\n",
          (unsigned long long)sent_msgs, (unsigned long long)sent_pkts, N,
          (unsigned long long)lapped_events);
  close(sock);
  return 0;
}