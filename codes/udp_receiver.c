// udp_receiver_lab.c
// Reliable-UDP receiver with Selective-Repeat + SACK and mmap()'d output.
// Build: gcc -O2 -std=gnu11 -Wall -Wextra -o udp_receiver_sack udp_receiver_sack.c
// Usage: ./udp_receiver_sack <output_file> [--port P] [--mtu M]

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#define DEFAULT_PORT 9000
#define DEFAULT_MTU  1500

enum { PKT_DATA=0x01, PKT_START=0x02, PKT_END=0x03, PKT_ACK=0x10 };

#pragma pack(push,1)
typedef struct {
    uint8_t  type;
    uint32_t seq;    // network order on wire
    uint16_t len;    // network order on wire
} pkt_hdr_t;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    uint32_t cum_ack;    // highest contiguous DATA seq received
    uint64_t sack_mask;  // bits for next 64 seqs after cum_ack
} ack_payload_t;
#pragma pack(pop)

static inline uint64_t htonll(uint64_t v){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (((uint64_t)htonl(v & 0xffffffffULL)) << 32) | htonl((uint32_t)(v >> 32));
#else
    return v;
#endif
}
static inline uint64_t ntohll(uint64_t v){ return htonll(v); }

static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec/1e9;
}
static void die(const char* msg){
    perror(msg); exit(EXIT_FAILURE);
}

typedef struct {
    uint8_t *base;
    uint64_t size;
    int fd;
} file_map_t;

static void fmap_open_wo(const char* path, uint64_t size, file_map_t* m){
    memset(m,0,sizeof(*m));
    m->fd = open(path, O_CREAT|O_TRUNC|O_RDWR, 0644);
    if (m->fd < 0) die("open output");
#ifdef __linux__
    // Pre-size file to avoid SIGBUS on mmap writes
    if (posix_fallocate(m->fd, 0, (off_t)size) != 0){
        // fallback: ftruncate
        if (ftruncate(m->fd, (off_t)size) != 0) die("ftruncate");
    }
#else
    if (ftruncate(m->fd, (off_t)size) != 0) die("ftruncate");
#endif
    m->size = size;
    m->base = mmap(NULL, size, PROT_WRITE|PROT_READ, MAP_SHARED, m->fd, 0);
    if (m->base == MAP_FAILED) die("mmap output");
}

static void fmap_close(file_map_t* m){
    if (m->base && m->base!=MAP_FAILED) msync(m->base, m->size, MS_SYNC);
    if (m->base && m->base!=MAP_FAILED) munmap(m->base, m->size);
    if (m->fd >= 0) close(m->fd);
}

static void send_ack_sack(int sock, const struct sockaddr_in* peer, socklen_t peerlen,
                          uint32_t cum_ack, uint64_t mask){
    pkt_hdr_t h; h.type = PKT_ACK; h.seq = htonl(0); h.len = htons(sizeof(ack_payload_t));
    ack_payload_t ap; ap.cum_ack = htonl(cum_ack); ap.sack_mask = htonll(mask);
    struct iovec iov[2] = { { &h, sizeof(h) }, { &ap, sizeof(ap) } };
    struct msghdr msg = {0};
    msg.msg_iov = iov; msg.msg_iovlen = 2;
    msg.msg_name = (void*)peer; msg.msg_namelen = peerlen;
    sendmsg(sock, &msg, 0);
}

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr, "Usage: %s <output_file> [--port P] [--mtu M]\n", argv[0]);
        return 2;
    }
    const char* out_path = argv[1];

    int port = DEFAULT_PORT, mtu = DEFAULT_MTU;
    for (int i=2; i<argc; ++i){
        if (!strcmp(argv[i], "--port") && i+1<argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mtu") && i+1<argc)  mtu  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rtt") || !strcmp(argv[i], "--loss")) { ++i; } // accepted & ignored
    }
    if (mtu < 576) { fprintf(stderr, "MTU too small.\n"); return 2; }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");
    int buf_sz = 8*1024*1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) die("bind");

    const int IP_UDP = 28;
    const int HDR = (int)sizeof(pkt_hdr_t);
    int payload_max = mtu - IP_UDP - HDR;
    if (payload_max < 512) payload_max = 512;

    // RX buffers
    uint8_t *buf = malloc(HDR + payload_max + 16);
    if (!buf) die("malloc");

    struct sockaddr_in peer; socklen_t peerlen = sizeof(peer);
    uint64_t expected_total = 0, received = 0;
    uint32_t total_segs = 0;
    uint32_t cum_ack = 0;     // highest contiguous seq received
    uint8_t *have = NULL;     // bitmap per segment
    file_map_t fm = {0};
    double t0 = 0.0;
    int started = 0, finished = 0;

    fprintf(stderr, "Listening on UDP %d, MTU=%d, payload<=%d …\n", port, mtu, payload_max);

    while (!finished){
        ssize_t n = recvfrom(sock, buf, HDR + payload_max, 0,
                             (struct sockaddr*)&peer, &peerlen);
        if (n < (ssize_t)HDR) continue;

        pkt_hdr_t *h = (pkt_hdr_t*)buf;
        uint8_t  type = h->type;
        uint32_t seq  = ntohl(h->seq);
        uint16_t len  = ntohs(h->len);

        if (type == PKT_START && seq == 0){
            if (!started){
                if (len != sizeof(uint64_t)) { fprintf(stderr,"Bad START len\n"); continue; }
                uint64_t fs_net = 0; memcpy(&fs_net, buf+HDR, sizeof(uint64_t));
                expected_total = ntohll(fs_net);
                total_segs = (uint32_t)((expected_total + payload_max - 1) / payload_max);
                have = calloc((size_t)total_segs + 1, 1);
                if (!have) die("alloc have");
                fmap_open_wo(out_path, expected_total, &fm);
                started = 1;
                cum_ack = 0;
                t0 = now_s();
                fprintf(stderr, "START: expecting %lu bytes in %u segments\n",
                        (unsigned long)expected_total, total_segs);
            }
            // simple START-ACK (no payload)
            pkt_hdr_t ack = { .type = PKT_ACK, .seq = htonl(0), .len = htons(sizeof(ack_payload_t)) };
            ack_payload_t ap = { htonl(cum_ack), htonll(0) };
            struct iovec iov[2] = { { &ack, sizeof(ack) }, { &ap, sizeof(ap) } };
            struct msghdr msg = {0};
            msg.msg_iov = iov; msg.msg_iovlen = 2;
            msg.msg_name = (void*)&peer; msg.msg_namelen = peerlen;
            sendmsg(sock, &msg, 0);
            continue;
        }

        if (!started) continue;

        if (type == PKT_DATA){
            if (seq == 0 || seq > total_segs){ /* ignore invalid */ }
            else {
                if (!have[seq]){
                    if (len > payload_max){ fprintf(stderr,"Bad DATA len\n"); continue; }
                    uint64_t off = (uint64_t)(seq - 1) * (uint64_t)payload_max;
                    // write into mmap at exact offset (works out-of-order)
                    memcpy(fm.base + off, buf + HDR, len);
                    received += len;
                    have[seq] = 1;

                    // advance cum_ack
                    while (cum_ack < total_segs && have[cum_ack + 1]) cum_ack++;
                }

                // build sack mask for next 64 seqs beyond cum_ack
                uint64_t mask = 0;
                for (int i=0; i<64; ++i){
                    uint32_t s = cum_ack + 1 + (uint32_t)i;
                    if (s <= total_segs && have[s]) mask |= (1ULL << i);
                }
                send_ack_sack(sock, &peer, peerlen, cum_ack, mask);
            }
            continue;
        }

        if (type == PKT_END){
            // final ACK; if we already have all, we’ll finish
            uint64_t mask = 0;
            for (int i=0; i<64; ++i){
                uint32_t s = cum_ack + 1 + (uint32_t)i;
                if (s <= total_segs && have[s]) mask |= (1ULL << i);
            }
            send_ack_sack(sock, &peer, peerlen, cum_ack, mask);
            if (cum_ack == total_segs) finished = 1;
            continue;
        }
    }

    double t1 = now_s();
    free(buf);
    if (have) free(have);
    fmap_close(&fm);

    if (expected_total && received != expected_total){
        fprintf(stderr, "Receiver WARNING: size mismatch, expected %lu got %lu\n",
                (unsigned long)expected_total, (unsigned long)received);
        return 1;
    }
    double secs = t1 - t0;
    double bits = (double)received * 8.0;
    printf("Receiver: got %lu bytes in %.3f s, avg %.3f Mb/s\n",
           (unsigned long)received, secs, (bits/1e6)/secs);
    return 0;
}
