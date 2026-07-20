/*
 * SENDER — Hybrid FEC + NACK retransmission
 *
 * Wire format (all to relay on 47001):
 *   DATA:  [4B seq BE, MSB=0] [160B payload]          = 164 bytes
 *   FEC:   [4B (seq|0x80000000) BE] [160B xor_payload] = 164 bytes
 *          FEC covers frames (seq-1, seq); stride-1 XOR
 *
 * Feedback from receiver (port 47004):
 *   NACK:  [4B missing_seq BE]  = 4 bytes
 *
 * FEC rate: 6 out of every 7 frames (skip when seq%7==0 && seq>=7)
 * Budget: (164 + 6/7*164) / 160 = 1.904x  [cap 2.0x]
 */
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

using namespace std;

static constexpr int PAYLOAD = 160;
static constexpr int RING = 4096;
static constexpr int RMASK = RING - 1;
static constexpr uint32_t FEC_FLAG = 0x80000000u;

/* Ring buffer for sent frames (retransmission + FEC source) */
static uint8_t  ring_pay[RING][PAYLOAD];
static uint32_t ring_seq[RING];
static bool     ring_ok[RING];

static int relay_fd;
static struct sockaddr_in relay_dst;

static inline void emit(const void *p, size_t n) {
    sendto(relay_fd, p, n, 0,
           (struct sockaddr *)&relay_dst, sizeof relay_dst);
}

static void send_data(uint32_t seq, const uint8_t *pay) {
    uint8_t pkt[164];
    uint32_t ns = htonl(seq);
    memcpy(pkt, &ns, 4);
    memcpy(pkt + 4, pay, PAYLOAD);
    emit(pkt, 164);
}

static void send_fec(uint32_t seq, const uint8_t *prev, const uint8_t *cur) {
    uint8_t pkt[164];
    uint32_t ns = htonl(seq | FEC_FLAG);
    memcpy(pkt, &ns, 4);
    for (int i = 0; i < PAYLOAD; i++)
        pkt[4 + i] = prev[i] ^ cur[i];
    emit(pkt, 164);
}

int main() {
    memset(ring_ok, 0, sizeof ring_ok);

    /* ---- sockets ---- */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(47010);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind 47010"); return 1;
    }

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(47004);
    if (bind(fb_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind 47004"); return 1;
    }

    relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_dst, 0, sizeof relay_dst);
    relay_dst.sin_family = AF_INET;
    relay_dst.sin_port   = htons(47001);
    relay_dst.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    int maxfd = (in_fd > fb_fd ? in_fd : fb_fd) + 1;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        FD_SET(fb_fd, &rfds);
        struct timeval tv = {1, 0};

        if (select(maxfd, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        /* ---- new frame from harness ---- */
        if (FD_ISSET(in_fd, &rfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
            if (n < 164) continue;

            uint32_t seq;
            memcpy(&seq, buf, 4);
            seq = ntohl(seq);
            const uint8_t *pay = buf + 4;

            /* store */
            int idx = seq & RMASK;
            ring_seq[idx] = seq;
            memcpy(ring_pay[idx], pay, PAYLOAD);
            ring_ok[idx] = true;

            /* forward DATA (send frame 0 twice — no backward FEC) */
            send_data(seq, pay);
            if (seq == 0) send_data(seq, pay); /* duplicate: 164B, negligible */

            /* FEC: XOR(seq-1, seq), skip every 7th to stay in budget */
            if (seq >= 1 && !(seq >= 7 && seq % 7 == 0)) {
                int pi = (seq - 1) & RMASK;
                if (ring_ok[pi] && ring_seq[pi] == seq - 1)
                    send_fec(seq, ring_pay[pi], pay);
            }
        }

        /* ---- NACK from receiver (via relay feedback) ---- */
        if (FD_ISSET(fb_fd, &rfds)) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, nullptr, nullptr);
            if (n < 4) continue;

            uint32_t missing;
            memcpy(&missing, buf, 4);
            missing = ntohl(missing);

            int idx = missing & RMASK;
            if (ring_ok[idx] && ring_seq[idx] == missing)
                send_data(missing, ring_pay[idx]);
        }
    }
}
