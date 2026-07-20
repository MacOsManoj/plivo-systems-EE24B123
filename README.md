# The Flaky Network: High-Performance UDP Recovery

This repository contains my final C++17 solution for the Plivo Systems Track Assignment ("The Flaky Network"). 

The implementation achieves zero-buffer immediate playout with minimal playout delay by pairing **Stride-1 XOR Forward Error Correction (FEC)** with an **Adaptive Jitter Buffer-inspired NACK retransmission engine**.

## 🚀 Core Architecture

### 1. Low-Overhead Wire Format (`sender.cpp`)
Packet types are encoded inside the existing sequence number by taking advantage of the Most Significant Bit (MSB):
*   **DATA:**  `[4B seq BE, MSB=0] [160B payload]`
*   **FEC:**   `[4B (seq|0x80000000) BE] [160B xor_payload]`
This zero-header-overhead format perfectly isolates protocol semantics without adding any extra byte-weight. `DATA` frames remain precisely $164$ bytes on the wire.

### 2. Forward Error Correction (FEC) Bounding (`sender.cpp`)
A stride-1 XOR parity block is generated continuously. For every frame $i$, it computes $XOR(payload_{i-1}, payload_i)$.
*   We transmit this FEC frame alongside the DATA frame for **6 out of every 7 frames**.
*   This specific $6/7$ ratio bounds the base bandwidth overhead to $1.90$x, which gives exactly $0.10$x of overhead headroom for NACK retransmissions, keeping the system safely under the strict $2.00$x cap.

### 3. Multi-Stage Receiver Pipeline (`receiver.cpp`)
1.  **Immediate Playout:** The receiver doesn't use a traditional jitter buffer. Because early frames are harmless (the harness player scores against a strict deadline), any recovered or newly received frame is blasted directly to `127.0.0.1:47020`.
2.  **FEC Cascade Recovery:** Upon receiving *or reconstructing* frame $i$, the receiver checks adjacent slots $i-1$ and $i+1$. It evaluates if an accumulated FEC XOR parity block can now uniquely recover the other missing packet. This recurses infinitely, effectively patching multi-packet consecutive burst losses instantly.
3.  **Adaptive Selective NACKs (AJB-Inspired):** A background thread scans for gaps in the buffer. Instead of using static NACK guess-timers, the thread acts as an Adaptive Jitter Tracker:
    *   **Dynamic Jitter Tracking:** Every time a DATA packet arrives, it calculates its exact transit time through the relay and updates an Exponential Moving Average (EMA).
    *   **Wait Delay (`nack_wait`):** The thread waits dynamically (`avg_transit + 15ms margin`) after a frame's theoretical production time before issuing a NACK. This ensures that delayed packets aren't prematurely flagged as missing, drastically reducing unnecessary feedback bandwidth while catching true drops faster.
    *   **Deadline Cutoff:** NACKs cease $20\%$ of `DELAY_MS` before the frame's true deadline, preserving bandwidth when retransmissions would logically arrive too late.

## 📊 Stress Test & Minimum Threshold Analysis
A parameter sweep was conducted by gradually decreasing `delay_ms` on Profile A to determine the absolute minimum playout delay required for **zero missing frames**.

| Playout Delay (`delay_ms`) | Deadline Misses | Miss Rate (%) | Status |
| :--- | :---: | :---: | :--- |
| **&ge; 65 ms** | **0** | **0.00%** | **PERFECT (0 Misses)** |
| **60 ms** | 1 | 0.13% | **VALID** |
| **55 ms** | 2 | 0.27% | **VALID** |
| **50 ms** | 3 | 0.40% | **VALID** |
| **45 ms** | 6 | 0.80% | **VALID** |

The system achieves 100% loss-free recovery at **65 ms**, and safely passes the `< 1.00%` miss cap down to **45 ms**.

## 🛠 Building & Running
```bash
# Build sender and receiver
make clean && make

# Run testing harness
python3 run.py --profile profiles/A.json --delay_ms 50 --duration 30
python3 run.py --profile profiles/B.json --delay_ms 90 --duration 30
```
