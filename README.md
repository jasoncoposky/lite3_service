# L3KV: The Zero-Parse Key-Value Store

**L3KV** is a high-performance, persistent Key-Value service built on **Modern C++23** and the **LiteÂ³** serialization library.

## ðŸš€ Features
* **Zero-Parse Mutations:** Update a single field in a 10MB document in microseconds.
* **HTTP Interface:** Use `curl`, Python, or any HTTP client.
* **ACID Compliance:** Full durability via Write-Ahead Logging (WAL).

## ðŸ“¦ Quick Start
1. Install CMake, C++23 Compiler, Boost.
2. `mkdir build && cd build`
3. `cmake .. && make`
4. `./l3svc`

## âš–ï¸ License
BSD 3-Clause License.
