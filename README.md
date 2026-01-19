# L3KV: The Zero-Parse Key-Value Store

**L3KV** is a high-performance, persistent Key-Value service built on **Modern C++23** and the **Lite³** serialization library. It leverages zero-deserialization editing to modify large JSON documents in-place with microsecond latency.

## 🚀 Features
* **Zero-Parse Mutations:** Update a single field in a 10MB document in **< 1 µs**.
* **Zero-Copy Architecture:** Data stays in the buffer; no intermediate object trees.
* **HTTP/1.1 Interface:** Standard REST API (`GET`, `PUT`, `DELETE`, `PATCH`).
* **Observability:** Built-in metrics endpoint for latency and operation counts.
* **Durability:** WAL-based persistence (Work In Progress).

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
```powershell
.\Release\l3svc.exe
```
The service listens on port `8080` by default.

## 🔌 API Reference

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/kv/{key}` | Retrieve document as JSON. |
| `PUT` | `/kv/{key}` | Store JSON document. |
| `DELETE` | `/kv/{key}` | Delete document. |
| `POST` | `/kv/{key}?op=set_int&field={path}&val={v}` | fast-path integer update. |
| `GET` | `/kv/metrics` | View internal performance metrics. |
| `GET` | `/kv/health` | Health check (returns 200 OK). |


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