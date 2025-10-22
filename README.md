# Custom Designed Fast and Reliable File Transfer Protocol
  
**Purpose:** High-performance, reliable 1 GB file transfer over lossy, delayed, and rate-limited links. Supports variable MTUs (1500 and 9001) and cloud deployment on AWS EC2.

---

## Table of Contents
1. [Overview](#overview)
2. [Motivation](#motivation)
3. [Objectives](#objectives)
4. [Protocol Design](#protocol-design)
5. [Implementation](#implementation)
6. [AWS Deployment](#aws-deployment)
7. [Testing & Results](#testing--results)
8. [Usage](#usage)
9. [References](#references)

---

## Overview
This project implements a **custom UDP-based file transfer protocol** designed for high-throughput and reliability in adverse network conditions. Standard TCP suffers significantly under **high RTT and packet loss**, and UDP alone is unreliable. Our protocol ensures **reliable, high-speed delivery** by combining **UDP’s low overhead** with **application-layer reliability mechanisms**.

---

## Motivation
Existing protocols have limitations in the tested network conditions:

- **TCP**
  - Performs poorly on high-latency, high-loss links due to congestion control and retransmission overhead.
  - Maximum throughput drops far below the link capacity (~80 Mb/s not achievable at 20% packet loss, 200 ms RTT).

- **UDP**
  - Lightweight, high-speed, but **unreliable**—packets can be lost, duplicated, or delivered out-of-order.
  - No built-in flow control, making it unsuitable for large file transfers without extra reliability mechanisms.

**Our custom protocol improves performance under these conditions** by:
- Using **UDP** as the transport for minimal header and kernel overhead.
- Implementing **selective retransmissions** via cumulative + selective ACKs to avoid unnecessary retransmits.
- Utilizing **zero-copy send** for CPU efficiency.
- Supporting **variable MTUs**, including jumbo frames (9001 B) for high throughput.
- Maintaining **bit-exact reliability** verified via MD5 checksums.

**Target Network Conditions**:
- RTT: 10–200 ms
- Packet Loss: 0–20%
- Link Rate: 80–100 Mb/s
- MTUs: 1500 (standard) and 9001 (jumbo)

Under these conditions, our protocol **achieves 95–96 Mb/s throughput on jumbo frames**, far outperforming TCP in high-latency/loss scenarios.

---

## Objectives
- Achieve **bit-exact file transfer** verified via MD5.
- Evaluate performance under:
  - Three link configurations with varying bandwidth, RTT, and packet loss.
  - Two MTU sizes: 1500 and 9001 (jumbo frames).
- Deploy and test on **AWS EC2 nodes** replicating the same link conditions.

---

## Protocol Design
- **Packet Types**: `START`, `DATA`, `ACK`, `END`.
- **Headers**: 7 bytes (type/flags + seq32 + len16).
- **Reliability**:
  - Sliding window of outstanding segments.
  - Cumulative ACK + Selective ACK (up to K SACK blocks).
  - Per-segment retransmission with exponential backoff.
- **Zero-Copy Transmission**:
  - Linux `SO_ZEROCOPY` + `sendmsg(..., MSG_ZEROCOPY)` for efficient DMA-based transfers.
- **Payloads**:
  - MTU 1500 → ~1465 B
  - MTU 9001 → ~8966 B

---

## Implementation
- **Languages**: C (POSIX sockets)
- **Files**:
  - `Client/udp_sender_lab.c`
  - `Server/udp_receiver_lab.c`
- **Dependencies**: gcc, make, Linux kernel ≥ 4.14 (for zero-copy)
- **Control Flow**:
  1. Sender sends `START` packet.
  2. Receiver ACKs with optional SACK blocks.
  3. Sender streams `DATA` packets.
  4. Receiver maintains gap map and sends cumulative + selective ACKs.
  5. `END` packet signals transfer completion.
- **Integrity Check**: `md5sum` at sender and receiver.

---

## AWS Deployment
- **Topology**:
  - EC2 Server (Receiver)
  - EC2 Client (Sender)
  - EC2 Router (Ubuntu/Debian) with `ip_forward=1`
- **Router Configuration**:
  - Traffic shaping via `tc`/`netem` for bandwidth, delay, and loss.
  - MTU adjustment (1500/9001).
- **Deployment Scripts**:
  - `AWS/setup_ec2.sh`
  - `AWS/setup_router.sh`
- **Instructions**: See `AWS/deploy_instructions.md`

---

## Testing & Results
| Case | MTU  | Router Rate (Mb/s) | RTT  | Loss | Avg Throughput (Mb/s) |
|------|------|------------------|------|------|---------------------|
| 1    | 1500 | 100              | 10ms | 1%   | 55.78               |
| 1    | 9001 | 100              | 10ms | 1%   | 95.28               |
| 2    | 1500 | 100              | 200ms| 20%  | 55.13               |
| 2    | 9001 | 100              | 200ms| 20%  | 95.50               |
| 3    | 1500 | 80               | 200ms| 0%   | 54.72               |
| 3    | 9001 | 80               | 200ms| 0%   | 95.99               |

**Insights**:
- Jumbo frames (+71–75% speed-up) drastically reduce per-packet system overhead.
- Reliability maintained under all loss/delay scenarios.
- Throughput approaches path rate for MTU 9001 due to fewer packets/sec and reduced CPU/syscall work.

