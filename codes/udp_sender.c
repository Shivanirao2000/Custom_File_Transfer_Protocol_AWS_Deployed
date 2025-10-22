// udp_sender_lab.c
// Reliable-UDP sender with Selective-Repeat + SACK and zero-copy-ish I/O.
// Build: gcc -O2 -std=gnu11 -Wall -Wextra -o udp_sender_sack udp_sender_sack.c
// Usage: ./udp_sender_sack <server_ip> <input_file> [--port P] [--mtu M] [--rto_ms MS] [--retries N] [--win W] [--zerocopy 1|0]
// Notes: MSG_ZEROCOPY requires Linux >= 4.14; we fall back automatically.

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
#define DEFAULT_RTO_MS 400
#define DEFAULT_RETRIES 50
#define DEFAULT_WIN 64

enum { PKT_DATA=0x01, PKT_START=0x02, PKT_END=0x03, PKT_ACK=0x10 };

#pragma pack(push,1)
typedef struct {
    uint8_t  type;   // PKT_*
    uint32_t seq;    // network order on wire (for DATA/END); for ACK we keep it 0
    uint16_t len;    // payload len for DATA; sizeof(ack payload) for ACK; nw order
} pkt_hdr_t; // 7 bytes
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    uint32_t cum_ack;    // highest contiguous DATA seq received
    uint64_t sack_mask;  // bits for next 64 seqs after cum_ack (bit0 = cum_ack+1)
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
    uint8_t *base;       // mmapped file base
    uint64_t size;       // file size
    int fd;
} file_map_t;

static void fmap_open_ro(const char* path, file_map_t* m){
    struct stat st; memset(m,0,sizeof(*m));
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) die("open input");
    if (fstat(m->fd, &st) != 0) die("fstat");
    m->size = (uint64_t)st.st_size;
    if (m->size == 0){ fprintf(stderr,"Input file empty\n"); exit(1); }
    m->base = mmap(NULL, m->size, PROT_READ, MAP_SHARED, m->fd, 0);
    if (m->base == MAP_FAILED) die("mmap input");
}

static void fmap_close(file_map_t* m){
    if (m->base && m->base!=MAP_FAILED) munmap(m->base, m->size);
    if (m->fd >= 0) close(m->fd);
}

int main(int argc, char **argv){
    if (argc < 3){
        fprintf(stderr, "Usage: %s <server_ip> <input_file> [--port P] [--mtu M] [--rto_ms MS] [--retries N] [--win W] [--zerocopy 1|0]\n", argv[0]);
        return 2;
    }
    const char* server_ip = argv[1];
    const char* in_path   = argv[2];

    int port = DEFAULT_PORT, mtu = DEFAULT_MTU;
    int rto_ms = DEFAULT_RTO_MS, retries = DEFAULT_RETRIES;
    int win = DEFAULT_WIN;
    int want_zerocopy = 1; // default ON if supported

    for (int i=3; i<argc; ++i){
        if (!strcmp(argv[i], "--port") && i+1<argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mtu") && i+1<argc)  mtu  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rto_ms") && i+1<argc) rto_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--retries") && i+1<argc) retries = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--win") && i+1<argc) win = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zerocopy") && i+1<argc) want_zerocopy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rtt") || !strcmp(argv[i], "--loss")) { ++i; } // ignored legacy
    }
    if (mtu < 576) { fprintf(stderr, "MTU too small.\n"); return 2; }
    if (win < 1 || win > 256) { fprintf(stderr, "Window 1..256 recommended\n"); win = DEFAULT_WIN; }

    file_map_t fm; fmap_open_ro(in_path, &fm);

    // socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");

    // optional zerocopy
#ifdef SO_ZEROCOPY
    if (want_zerocopy){
        int one = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) != 0){
            fprintf(stderr, "SO_ZEROCOPY unsupported, continuing without it.\n");
            want_zerocopy = 0;
        }
    }
#else
    want_zerocopy = 0;
#endif

    int buf_sz = 8*1024*1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

    struct timeval tv = { .tv_sec = rto_ms/1000, .tv_usec = (rto_ms%1000)*1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1){ fprintf(stderr,"bad server ip\n"); return 2; }
    if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) != 0) die("connect");

    const int IP_UDP = 28;
    const int HDR = (int)sizeof(pkt_hdr_t);
    int payload_max = mtu - IP_UDP - HDR;
    if (payload_max < 512) payload_max = 512;

    // segmentation
    uint64_t total_bytes = fm.size;
    uint32_t total_segs = (uint32_t)((total_bytes + payload_max - 1) / payload_max);

    // per-seg state
    uint8_t *acked = calloc((size_t)total_segs + 1, 1);
    double  *sent_ts = calloc((size_t)total_segs + 1, sizeof(double));
    int     *tx_cnt  = calloc((size_t)total_segs + 1, sizeof(int));
    if (!acked || !sent_ts || !tx_cnt) die("alloc state");

    // START handshake: send filesize
    {
        pkt_hdr_t h; h.type = PKT_START; h.seq = htonl(0); h.len = htons(sizeof(uint64_t));
        uint64_t fs_net = htonll(total_bytes);
        struct iovec iov[2] = {
            { &h, sizeof(h) },
            { &fs_net, sizeof(fs_net) }
        };
        struct msghdr msg = {0};
        msg.msg_iov = iov; msg.msg_iovlen = 2;
        for (int t=0; t<retries; ++t){
            ssize_t s = sendmsg(sock, &msg, want_zerocopy ? MSG_ZEROCOPY : 0);
            if (s < 0) perror("send START");
            uint8_t abuf[64];
            ssize_t r = recv(sock, abuf, sizeof(abuf), 0);
            if (r >= (ssize_t)sizeof(pkt_hdr_t)){
                pkt_hdr_t *ah = (pkt_hdr_t*)abuf;
                if (ah->type == PKT_ACK) break;
            }
            if (t == retries-1){ fprintf(stderr,"Failed to handshake START.\n"); exit(1); }
        }
    }

    fprintf(stderr, "MTU=%d payload=%d, RTO=%dms, RETRIES=%d, Port=%d, WIN=%d, ZC=%d, total_segs=%u\n",
            mtu, payload_max, rto_ms, retries, port, win, want_zerocopy, total_segs);

    double t0 = now_s();

    uint32_t base = 1;                    // first unacked seq
    uint32_t next_to_send = 1;            // next seq to transmit
    int in_flight = 0;

    // main loop
    while (base <= total_segs){
        // 1) send new within window
        while (next_to_send <= total_segs && (int)(next_to_send - base) < win){
            uint64_t offset = (uint64_t)(next_to_send - 1) * (uint64_t)payload_max;
            uint16_t len = (uint16_t)MIN((uint64_t)payload_max, total_bytes - offset);

            pkt_hdr_t h; h.type = PKT_DATA; h.seq = htonl(next_to_send); h.len = htons(len);
            struct iovec iov[2] = {
                { &h, sizeof(h) },
                { fm.base + offset, len }
            };
            struct msghdr msg = {0};
            msg.msg_iov = iov; msg.msg_iovlen = 2;

            if (sendmsg(sock, &msg, want_zerocopy ? MSG_ZEROCOPY : 0) < 0){
                perror("sendmsg DATA");
            } else {
                if (tx_cnt[next_to_send] == 0) in_flight++;
                tx_cnt[next_to_send]++;
                sent_ts[next_to_send] = now_s();
            }
            next_to_send++;
        }

        // 2) receive ACK/SACK (non-blocking due to SO_RCVTIMEO)
        uint8_t abuf[128];
        ssize_t r = recv(sock, abuf, sizeof(abuf), 0);
        if (r >= (ssize_t)sizeof(pkt_hdr_t)){
            pkt_hdr_t *ah = (pkt_hdr_t*)abuf;
            if (ah->type == PKT_ACK && ntohs(ah->len) == sizeof(ack_payload_t)){
                ack_payload_t ap = {0};
                memcpy(&ap, abuf + sizeof(pkt_hdr_t), sizeof(ap));
                uint32_t cum = ntohl(ap.cum_ack);
                uint64_t mask = ntohll(ap.sack_mask);

                // ack all <= cum
                for (uint32_t s = base; s <= cum && s <= total_segs; ++s){
                    if (!acked[s]) { acked[s] = 1; in_flight -= (tx_cnt[s] > 0); }
                }
                // advance base
                while (base <= total_segs && acked[base]) base++;

                // ack masked beyond cum
                for (int i=0; i<64; ++i){
                    if (mask & (1ULL << i)){
                        uint32_t s = cum + 1 + (uint32_t)i;
                        if (s <= total_segs && !acked[s]){
                            acked[s] = 1;
                            if (tx_cnt[s] > 0) in_flight--;
                        }
                    }
                }
                // slide base again
                while (base <= total_segs && acked[base]) base++;
            }
        }

        // 3) retransmit timed-out gaps inside window
        double now = now_s();
        for (uint32_t s = base; s < next_to_send; ++s){
            if (s==0 || s>total_segs) continue;
            if (acked[s]) continue;
            if (tx_cnt[s] >= retries){
                fprintf(stderr,"Failed sending seq=%u after retries.\n", s);
                exit(1);
            }
            if (now - sent_ts[s] >= (double)rto_ms/1000.0){
                uint64_t offset = (uint64_t)(s - 1) * (uint64_t)payload_max;
                uint16_t len = (uint16_t)MIN((uint64_t)payload_max, total_bytes - offset);
                pkt_hdr_t h; h.type = PKT_DATA; h.seq = htonl(s); h.len = htons(len);
                struct iovec iov[2] = {
                    { &h, sizeof(h) },
                    { fm.base + offset, len }
                };
                struct msghdr msg = {0};
                msg.msg_iov = iov; msg.msg_iovlen = 2;
                if (sendmsg(sock, &msg, want_zerocopy ? MSG_ZEROCOPY : 0) < 0) perror("re-sendmsg");
                tx_cnt[s]++; sent_ts[s] = now;
            }
        }
    }

    // END: seq = total_segs + 1
    {
        uint32_t end_seq = total_segs + 1;
        pkt_hdr_t h; h.type = PKT_END; h.seq = htonl(end_seq); h.len = htons(0);
        for (int t=0; t<retries; ++t){
            if (send(sock, &h, sizeof(h), want_zerocopy ? MSG_ZEROCOPY : 0) < 0) perror("send END");
            uint8_t abuf2[64];
            ssize_t r2 = recv(sock, abuf2, sizeof(abuf2), 0);
            if (r2 >= (ssize_t)sizeof(pkt_hdr_t)){
                pkt_hdr_t *ah = (pkt_hdr_t*)abuf2;
                if (ah->type == PKT_ACK) break;
            }
            if (t == retries-1){ fprintf(stderr,"Failed to finalize END.\n"); exit(1); }
        }
    }

    double t1 = now_s();
    fmap_close(&fm);
    free(acked); free(sent_ts); free(tx_cnt);

    double secs = t1 - t0;
    double bits = (double)total_bytes * 8.0;
    printf("Sender: sent %lu bytes in %.3f s, avg %.3f Mb/s\n",
           (unsigned long)total_bytes, secs, (bits/1e6)/secs);
    return 0;
}
