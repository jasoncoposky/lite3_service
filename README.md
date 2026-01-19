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

---

## ðŸ§¨ Short-Term Testing Plan

We have identified the following areas for further testing to enhance the robustness and coverage of both the `lite3++` library and the `lite3_service`:

### 1. Expand `lite3++` Unit Tests (C++)
*   **Objective:** Increase coverage for the `lite3::Buffer` class and `lite3::lite3_json` functions.
*   **Specifics:**
    *   Add tests for all supported data types (Int64, Float64, Bool, Null, Bytes).
    *   Implement tests for complex JSON structures (nested objects, arrays, mixed types).
    *   Cover edge cases like empty strings, very long strings, special characters, maximum nesting depths.
    *   Test error handling for `lite3::Buffer` operations and invalid JSON inputs.

### 2. Add More Functional Tests to Python Client
*   **Objective:** Cover a wider range of `lite3_service` API operations.
*   **Specifics:**
    *   Implement tests for other `PATCH` operations (if available, e.g., `set_str`, `delete_key`).
    *   Add tests for `DELETE` operations.
    *   Test interactions with more complex JSON structures from the client side.

### 3. Integrate a More Sophisticated Load Testing Framework
*   **Objective:** Perform more rigorous stress and performance testing.
*   **Specifics:**
    *   Evaluate and integrate a framework like [Locust](https://locust.io/) or similar tools.
    *   Define realistic user behavior scenarios for load simulation.
    *   Collect and visualize detailed performance metrics under high load.

### 4. Focus on Specific Error Conditions
*   **Objective:** Verify the service's behavior under erroneous conditions.
*   **Specifics:**
    *   Test how the service responds to malformed HTTP requests.
    *   Send invalid JSON payloads to `PUT` and `PATCH` endpoints.
    *   Test requests for non-existent keys.
    *   Simulate network errors or service disruptions.