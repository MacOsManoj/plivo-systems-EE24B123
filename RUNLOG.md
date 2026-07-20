# Final Execution & Stress Test Log

This document records the final performance sweep and stress testing logs for the C++ implementation.

## 1. Primary Profile Validation

### Profile A (`delay_ms = 50`, `duration = 30s`)
```
=== PROFILE A @ 50ms ===
endpoints done
relay done: {'up_bytes': 456904, 'down_bytes': 0, 'up_pkts': 2786, 'down_pkts': 0, 'dropped': 66, 'duplicated': 19}
================ SCORE ================
  frames               : 1500
  deadline misses      : 4  (0.27%)   [cap 1.00%]
  playout delay        : 50 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.90x   [cap 2.00x]   (up 456904B, feedback 0B)
  RESULT               : VALID
```

### Profile B (`delay_ms = 90`, `duration = 30s`)
```
=== PROFILE B @ 90ms ===
endpoints done
relay done: {'up_bytes': 466416, 'down_bytes': 240, 'up_pkts': 2844, 'down_pkts': 60, 'dropped': 160, 'duplicated': 31}
================ SCORE ================
  frames               : 1500
  deadline misses      : 6  (0.40%)   [cap 1.00%]
  playout delay        : 90 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.94x   [cap 2.00x]   (up 466416B, feedback 240B)
  RESULT               : VALID
```

---

## 2. Playout Delay Sweep / Stress Test (Profile A)

Iterative sweep on Profile A decreasing `delay_ms` to locate the zero-miss threshold:

```
=== delay_ms 120 ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 110 ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 100 ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 95  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 90  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 85  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 80  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 75  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 70  ===  deadline misses: 0 (0.00%) [VALID]
=== delay_ms 65  ===  deadline misses: 0 (0.00%) [VALID] <-- ABSOLUTE MINIMUM FOR 0 MISSES
=== delay_ms 60  ===  deadline misses: 1 (0.13%) [VALID]
=== delay_ms 55  ===  deadline misses: 2 (0.27%) [VALID]
=== delay_ms 50  ===  deadline misses: 3 (0.40%) [VALID]
=== delay_ms 45  ===  deadline misses: 6 (0.80%) [VALID]
=== delay_ms 40  ===  deadline misses: 61 (8.13%) [INVALID]
```

### Key Observation:
*   **65 ms** is the absolute minimum playout delay threshold with **zero missing frames**.
*   The implementation remains fully **VALID** (&lt;1.00% deadline misses) all the way down to **45 ms**.
