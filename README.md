# Custom_Designed_Fast_and_Reliable_File_Transfer_Protocol

## Overview

This project implements a **custom high-performance file transfer protocol** built over UDP, designed to achieve **TCP-like reliability** with **significantly higher throughput** under high latency and packet loss environments.  

The goal was to **reliably transfer a 1 GB file** between client and server across different network conditions and MTU sizes (1500 and 9001 bytes), maintaining data integrity verified via **MD5 checksum** while achieving the **fastest possible transfer rate**.

---

## ⚙️ Motivation

Traditional TCP struggles under high latency and loss due to congestion control and retransmission delays, often falling below 20 Mbps. UDP, on the other hand, provides raw speed but lacks reliability.  
Our custom protocol bridges this gap — combining **UDP’s speed** with **application-level reliability** and **adaptive flow control** — to outperform TCP in lossy and delayed networks.

---

## 🧠 Protocol Design

Our **Fast and Reliable File Transfer Protocol (FRFTP)** is built upon UDP with custom layers ensuring:

### 1. **Reliability Layer**
- Implements **Selective Repeat ARQ (Automatic Repeat Request)**.
- Packets are numbered using a **32-bit sequence number**, allowing out-of-order arrivals.
- Lost packets are detected through **ACK timeouts** and retransmitted individually.
- Ensures full reliability without retransmitting entire windows.

### 2. **Congestion and Flow Control**
- **Dynamic Sliding Window**: Adjusts window size based on real-time acknowledgment feedback.
- **Adaptive Timeout**: Timeout intervals are dynamically tuned using **RTT estimation**.
- **Loss-aware rate control**: Reduces send rate only when multiple consecutive losses occur to avoid overreaction.

### 3. **Packet Structure**
Each UDP datagram is structured as:

| Field | Size | Description |
|-------|------|-------------|
| Header | 12 bytes | Contains sequence number, flags (ACK/DATA), checksum |
| Payload | Variable | File data chunk |
| Checksum | 4 bytes | Ensures integrity of data |

This modular packet design supports fast parsing and efficient retransmission.

---

## 🧩 Data Structures

| Component | Description |
|------------|-------------|
| **Send Buffer (Dictionary)** | Holds packets awaiting acknowledgment, keyed by sequence number |
| **Receive Buffer (Dictionary)** | Temporarily stores out-of-order packets until sequence completion |
| **ACK Tracker (Set)** | Tracks acknowledged sequence numbers to prevent duplicate retransmissions |
| **Window Manager** | Dynamically adjusts the sending window size based on ACK density and network feedback |

---

## 🔄 Algorithm Flow

### Sender (Client)
1. Read file in chunks and encapsulate data packets with headers.
2. Transmit packets continuously within the allowed window.
3. Wait for ACKs; upon timeout, selectively retransmit missing packets.
4. Upon completion, send a “FIN” packet to indicate end of transmission.

### Receiver (Server)
1. Receive packets and send ACKs for successfully received sequence numbers.
2. Store packets in order and reassemble the complete file.
3. Compute **MD5 checksum** to verify data integrity.

---

## 🧮 Implementation Summary

The application is divided into two Python scripts:

- **`server.py`** — Receives the file, sends ACKs, verifies integrity.
- **`client.py`** — Sends the file using the custom FRFTP protocol.

