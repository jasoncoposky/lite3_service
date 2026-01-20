# L3KV: The Zero-Parse Key-Value Store

**L3KV** is a high-performance, persistent Key-Value service built on **Modern C++23** and the **Lite³** serialization library. It leverages zero-deserialization editing to modify large JSON documents in-place with microsecond latency.

## 🚀 Features
*   **Dynamic Scaling:** Predictive thread pool using a Kalman Filter to auto-scale resources.
*   **Buffered Persistence:** Write-Ahead Log with **0 ms hot-path latency** (Buffered I/O).
*   **Graceful Durability:** Guaranteed persistence on shutdown (`SIGINT`, `SIGTERM`).
*   **Zero-Parse Mutations:** Update a single field in a 10MB document in **< 1 µs**.
*   **Zero-Copy Architecture:** Data stays in the buffer; no intermediate object trees.
*   **HTTP/1.1 Interface:** Standard REST API (`GET`, `PUT`, `DELETE`, `PATCH`).
*   **Observability:** Built-in metrics endpoint and **HTML Dashboard**.

## 💾 Durability & Persistence

L3KV uses a high-performance **Buffered Write-Ahead Log (WAL)**.

*   **Fast Path:** Writes are appended to a 20MB in-memory buffer (`libconveyor`), achieving **0 ms latency** for the database engine.
*   **Flush Policy:**
    *   **Background:** The OS flushes dirty pages to disk asynchronously.
    *   **Shutdown:** On receiving a signal (`SIGINT`, `SIGTERM`, `SIGBREAK`), the server forcefully flushes all buffers to `data.wal`.
*   **Trade-off:**
    *   **Graceful Shutdown:** 100% Data Durability GUARANTEED.
    *   **Hard Crash / Power Loss:** Potential loss of buffered data (up to 20MB) that hasn't been flushed by the OS. The WAL integrity remains protected by CRC32, so no corruption occurs—only lost recent writes.

### Crash Recovery
*   **Startup:** The service scans `data.wal` using a fast-read buffer.
*   **Corrupt Entries:** Partial writes at the end of the log (from a hard crash) are detected via CRC32 mismatch and discarded, verifying the database to the last consistent state.

## 📊 Observability

L3KV includes a comprehensive observability suite for real-time monitoring.

### Dashboard
A zero-dependency, real-time visual monitor is available at `/dashboard`.
*   **Live Charts:** Visualizes throughput (bytes in/out) and write latency.
*   **KPI Cards:** Tracks active connections, total errors (4xx/5xx), and current throughput.
*   **Dark Mode:** Sleek, modern UI.

[Access Dashboard](http://localhost:8080/dashboard)

### Metrics API
`GET /metrics` produces a JSON payload compatible with monitoring systems.
```json
{
  "system": { "active_connections": 5, "thread_count": 8 },
  "throughput": { "bytes_received_total": 10240, "http_errors_4xx": 0 }
}
```

## 🛠️ Build & Run (Windows)

### Prerequisites
*   CMake (3.20+)
*   Visual Studio 2022 (C++20/23 support)
*   Boost Libraries (1.70+): `asio`, `beast`, `system`
*   `lite3-cpp` library (linked via CMake)

### Build
```powershell
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release --target l3svc
```

### Run
### One-step Run
```powershell
.\Release\l3svc.exe [config_path]
# Defaults to config.json if not specified
```
Ensure `config.json` is present in the working directory or provide a path.

#### Configuration (`config.json`)
```json
{
  "server": {
    "address": "0.0.0.0",
    "port": 8080,
    "min_threads": 4,
    "max_threads": 16
  },
  "storage": {
    "wal_path": "data.wal"
  }
}
```
The service listens on port `8080` (or as configured).

## 🔌 API Reference

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/kv/{key}` | Retrieve document as JSON. |
| `PUT` | `/kv/{key}` | Store JSON document. |
| `DELETE` | `/kv/{key}` | Delete document. |
| `POST` | `/kv/{key}?op=set_int&field={path}&val={v}` | fast-path integer update. |
| `GET` | `/metrics` | **(NEW)** Real-time JSON metrics. |
| `GET` | `/dashboard` | **(NEW)** Visual Dashboard. |
| `GET` | `/kv/health` | Health check (returns 200 OK). |
| `GET` | `/kv/metrics` | (Deprecated) Legacy metrics text. |


## ⚡ Performance Metrics

Benchmark results (Windows 11, Ryzen 7 5700X, Loopback):

| Operation | Latency (Server Internal) | Description |
| :--- | :--- | :--- |
| **Document Write** | **< 1 µs** | `lite3-cpp` zero-parse update. |
| **JSON Serialization** | **~3 µs** | Converting buffer to JSON string. |
| **Request Handler** | **~50 µs** | Total time in `http_server::handle_request`. |
| **Network RTT** | **~1-130 ms** | Dependent on Client/OS (Keep-Alive vs Close). |

**Note on Network Latency:**
The service enables `TCP_NODELAY` to minimize latency. However, observed latency on Windows localhost with some clients (e.g., Python `aiohttp`) can be high due to OS scheduling and driver overhead. The core engine processes requests in microseconds. For maximum throughput, use persistent connections (Keep-Alive).