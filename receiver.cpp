/*
 * RECEIVER — FEC recovery + cascade + NACK retransmission + immediate playout
 *
 * Wire format (from relay on 47002):
 *   DATA:  [4B seq BE, MSB=0] [160B payload]          = 164 bytes
 *   FEC:   [4B (seq|0x80000000) BE] [160B xor_payload] = 164 bytes
 *          FEC covers frames (seq-1, seq); stride-1 XOR
 *
 * Key design: forward every received/recovered frame to the player
 * IMMEDIATELY.  The harness player judges by arrival time vs deadline;
 * there is no benefit to holding frames in a jitter buffer.
 *
 * Cascade recovery: when frame i is recovered, check if frame i±1 can
 * now be recovered from stored FEC — this chains through burst losses.
 *
 * NACK thread: periodically scans for gaps and sends NACK packets to
 * the sender via the relay feedback path (port 47003).
 */
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

using namespace std;

static constexpr int PAYLOAD = 160;
static constexpr int MAXF    = 8192;
static constexpr uint32_t FEC_FLAG = 0x80000000u;
static constexpr int MAX_NACK_ATTEMPTS = 3;

/* ---- frame buffer ---- */
struct Slot {
    uint8_t payload[PAYLOAD];
    uint8_t fec_xor[PAYLOAD];   /* XOR(payload[seq-1], payload[seq]) */
    bool    received;
    bool    fec_valid;
    int     nack_cnt;
    double  nack_time;
};

static Slot frames[MAXF];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int highest_seq = -1;
static double avg_transit = 0.040; /* EMA of network transit time */

/* ---- environment ---- */
static double T0, DELAY_MS, DURATION_S;
static int total_frames;

/* ---- sockets ---- */
static int out_fd;                       /* -> player 47020 */
static struct sockaddr_in player_dst;
static int nack_fd;                      /* -> relay  47003 */
static struct sockaddr_in relay_fb;

static double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Send frame to harness player (localhost, no relay, no overhead cost) */
static void to_player(uint32_t seq, const uint8_t *pay) {
    uint8_t pkt[164];
    uint32_t ns = htonl(seq);
    memcpy(pkt, &ns, 4);
    memcpy(pkt + 4, pay, PAYLOAD);
    sendto(out_fd, pkt, 164, 0,
           (struct sockaddr *)&player_dst, sizeof player_dst);
}

static void send_nack(uint32_t seq) {
    uint8_t pkt[4];
    uint32_t ns = htonl(seq);
    memcpy(pkt, &ns, 4);
    sendto(nack_fd, pkt, 4, 0,
           (struct sockaddr *)&relay_fb, sizeof relay_fb);
}

/* Forward declaration */
static void cascade(uint32_t seq);

/* Store + forward + cascade.  MUST be called with mu held. */
static void store(uint32_t seq, const uint8_t *pay, double transit_time) {
    if (seq >= (uint32_t)MAXF || frames[seq].received) return;
    memcpy(frames[seq].payload, pay, PAYLOAD);
    frames[seq].received = true;
    if ((int)seq > highest_seq) highest_seq = (int)seq;
    
    /* Update dynamic transit time tracker (AJB inspired) */
    if (transit_time > 0 && transit_time < 0.200) {
        avg_transit = 0.8 * avg_transit + 0.2 * transit_time;
    }

    to_player(seq, pay);
    cascade(seq);
}

/*
 * After storing frame `seq`, check whether stored FEC can now recover
 * frame seq-1 or seq+1.  Recurse to handle multi-frame bursts.
 */
static void cascade(uint32_t seq) {
    /* backward: recover seq-1 using FEC stored at slot[seq] */
    if (seq > 0 && seq < (uint32_t)MAXF &&
        frames[seq].fec_valid && !frames[seq - 1].received) {
        uint8_t rec[PAYLOAD];
        for (int i = 0; i < PAYLOAD; i++)
            rec[i] = frames[seq].fec_xor[i] ^ frames[seq].payload[i];
        store(seq - 1, rec, 0);       /* recursive, 0 transit time to ignore */
    }
    /* forward: recover seq+1 using FEC stored at slot[seq+1] */
    if (seq + 1 < (uint32_t)MAXF &&
        frames[seq + 1].fec_valid && !frames[seq + 1].received) {
        uint8_t rec[PAYLOAD];
        for (int i = 0; i < PAYLOAD; i++)
            rec[i] = frames[seq + 1].fec_xor[i] ^ frames[seq].payload[i];
        store(seq + 1, rec, 0);       /* recursive, 0 transit time to ignore */
    }
}

/* ---- NACK thread ---- */
static void *nack_loop(void *) {
    double end = T0 + DURATION_S + DELAY_MS / 1000.0 + 1.0;
    /* Adaptive timing based on DELAY_MS */
    double delay_s = DELAY_MS / 1000.0;
    double nack_cutoff  = delay_s * 0.20;  /* stop NACKing this close to deadline */
    double nack_spacing = 0.015;           /* 15ms between NACKs for same frame */
    if (nack_cutoff < 0.010) nack_cutoff = 0.010;

    while (now_s() < end) {
        usleep(8000);                 /* ~8 ms scan interval */
        pthread_mutex_lock(&mu);
        double now = now_s();
        int hi = highest_seq;

        /* Only scan confirmed gaps: frames below highest received */
        for (int i = 0; i <= hi && i < total_frames; i++) {
            if (frames[i].received) continue;
            /* don't NACK if deadline is already too close */
            double deadline = T0 + delay_s + i * 0.020;
            if (now > deadline - nack_cutoff) continue;
            /* wait for the frame to have had time to arrive naturally
               Dynamic tracking using AJB-inspired Exponential Moving Average */
            double dynamic_nack_wait = avg_transit + 0.015; /* avg + 15ms jitter margin */
            if (dynamic_nack_wait < 0.025) dynamic_nack_wait = 0.025;
            
            double earliest = T0 + i * 0.020 + dynamic_nack_wait;
            if (now < earliest) continue;
            /* rate-limit per frame */
            if (frames[i].nack_cnt >= MAX_NACK_ATTEMPTS) continue;
            if (now - frames[i].nack_time < nack_spacing) continue;
            /* confirm gap: at least one frame after i must have arrived */
            bool gap_ok = false;
            for (int j = i + 1; j <= hi && j <= i + 5; j++) {
                if (frames[j].received) { gap_ok = true; break; }
            }
            if (!gap_ok) continue;

            send_nack((uint32_t)i);
            frames[i].nack_cnt++;
            frames[i].nack_time = now;
        }
        pthread_mutex_unlock(&mu);
    }
    return nullptr;
}

int main() {
    const char *v;
    v = getenv("T0");         T0         = v ? atof(v) : 0;
    v = getenv("DURATION_S"); DURATION_S = v ? atof(v) : 30;
    v = getenv("DELAY_MS");   DELAY_MS   = v ? atof(v) : 60;
    total_frames = (int)(DURATION_S * 1000.0 / 20.0);

    memset(frames, 0, sizeof frames);

    /* ---- sockets ---- */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(47002);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind 47002"); return 1;
    }

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player_dst, 0, sizeof player_dst);
    player_dst.sin_family = AF_INET;
    player_dst.sin_port   = htons(47020);
    player_dst.sin_addr.s_addr = inet_addr("127.0.0.1");

    nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_fb, 0, sizeof relay_fb);
    relay_fb.sin_family = AF_INET;
    relay_fb.sin_port   = htons(47003);
    relay_fb.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* ---- threads ---- */
    pthread_t nt;
    pthread_create(&nt, nullptr, nack_loop, nullptr);

    /* ---- main receive loop ---- */
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n < 164) continue;

        uint32_t hdr;
        memcpy(&hdr, buf, 4);
        hdr = ntohl(hdr);

        pthread_mutex_lock(&mu);
        double arrival_now = now_s();

        if (hdr & FEC_FLAG) {
            /* ---- FEC packet ---- */
            uint32_t seq = hdr & ~FEC_FLAG;
            if (seq > 0 && seq < (uint32_t)MAXF) {
                memcpy(frames[seq].fec_xor, buf + 4, PAYLOAD);
                frames[seq].fec_valid = true;

                if (frames[seq].received && !frames[seq - 1].received) {
                    uint8_t rec[PAYLOAD];
                    for (int i = 0; i < PAYLOAD; i++)
                        rec[i] = frames[seq].fec_xor[i] ^ frames[seq].payload[i];
                    store(seq - 1, rec, 0);
                } else if (frames[seq - 1].received && !frames[seq].received) {
                    uint8_t rec[PAYLOAD];
                    for (int i = 0; i < PAYLOAD; i++)
                        rec[i] = frames[seq].fec_xor[i] ^ frames[seq - 1].payload[i];
                    store(seq, rec, 0);
                }
            }
        } else {
            /* ---- DATA packet ---- */
            double transit = arrival_now - (T0 + hdr * 0.020);
            store(hdr, buf + 4, transit);
        }

        pthread_mutex_unlock(&mu);
    }
}
