// receiver.cpp — plain-UDP baseline transport, destination side.
//
// Busy-spins on the UDP socket and republishes every datagram it gets into a
// local shared-memory ring, unchanged, so the harness's own consumer binary
// can read it and measure end-to-end latency from send_ts_ns in the payload.
// No FEC, no loss recovery: a dropped packet is just a missing seq_id
// downstream (that gap is exactly what the FEC layer is meant to fix later).
//
// Performance notes:
//  - Nonblocking socket + recvmmsg() in a spin loop: when packets are queued
//    in the kernel socket buffer we drain up to --batch of them in one
//    syscall; when none are queued we get EAGAIN immediately and _mm_pause(),
//    no blocking/sleeping.
//  - Publish into the ring happens frame-by-frame right after the syscall,
//    so a burst doesn't sit around waiting on a timer before becoming
//    visible to the consumer.
//  - No heap allocation on the hot path; buffers are pre-sized from --batch.
//
// Usage:
//   receiver --listen PORT --shm NAME [--slots N] [--rcvbuf BYTES]
//            [--batch N]
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // recvmmsg() is a GNU extension, not in plain POSIX headers
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
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
  int port = 0;
  std::string shm_name = "/fanout_ring_rx";
  uint32_t slots = 1024;
  int rcvbuf = 8 << 20;  // 8 MiB
  uint32_t batch = 64;
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
    if (a == "--listen") c.port = std::stoi(next());
    else if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--rcvbuf") c.rcvbuf = std::stoi(next());
    else if (a == "--batch") c.batch = static_cast<uint32_t>(std::stoul(next()));
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.port == 0) {
    fprintf(stderr, "need --listen PORT\n");
    std::exit(2);
  }
  if (c.batch == 0) c.batch = 1;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) { perror("socket"); return 1; }
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    return 1;
  }
  int fl = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, fl | O_NONBLOCK);

  shm::Segment seg = shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots),
                                         /*create=*/true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/true);

  const uint32_t B = cfg.batch;
  std::vector<uint8_t> bufs(static_cast<size_t>(B) * shm::kFrameCap);
  std::vector<iovec> iovs(B);
  std::vector<mmsghdr> msgs(B);
  for (uint32_t i = 0; i < B; ++i) {
    iovs[i].iov_base = &bufs[static_cast<size_t>(i) * shm::kFrameCap];
    iovs[i].iov_len = shm::kFrameCap;
    msgs[i].msg_hdr.msg_name = nullptr;
    msgs[i].msg_hdr.msg_namelen = 0;
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control = nullptr;
    msgs[i].msg_hdr.msg_controllen = 0;
  }

  uint64_t recv_pkts = 0, bad_pkts = 0;

  fprintf(stderr, "receiver: listening :%d -> shm=%s slots=%u\n", cfg.port,
          cfg.shm_name.c_str(), cfg.slots);

  while (!g_stop) {
    int n = recvmmsg(sock, msgs.data(), B, 0, nullptr);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) { CPU_PAUSE(); continue; }
      if (errno == EINTR) continue;
      perror("recvmmsg");
      continue;
    }
    // t2: one clock read for the whole recvmmsg batch -- they were all already
    // sitting in the socket buffer when the syscall returned.
    const uint64_t rx_ts = util::now_ns();
    for (int i = 0; i < n; ++i) {
      const uint32_t len = msgs[i].msg_len;
      if (len < sizeof(msg::Header) || len > shm::kFrameCap) {
        ++bad_pkts;
        continue;
      }
      std::memcpy(static_cast<uint8_t*>(iovs[i].iov_base) +
                      offsetof(msg::Header, rx_ts_ns),
                  &rx_ts, sizeof(rx_ts));
      ring.publish(iovs[i].iov_base, len);
      ++recv_pkts;
    }
  }

  fprintf(stderr, "receiver: published %llu messages (%llu bad), exiting\n",
          (unsigned long long)recv_pkts, (unsigned long long)bad_pkts);
  seg.unlink();
  close(sock);
  return 0;
}