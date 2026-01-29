# **L3KV Benchmarking Instructions (Antigravity Dev Env)**

**Target System:** L3KV Service (HTTP/1.1)

**Objective:** Verify performance claims regarding "Zero-Parse" mutations and validate standard KV throughput.

**Audience:** Antigravity Development Team

## **1\. Environment Preparation**

Before running benchmarks, ensure the L3KV service is running in **Release Mode** with logging minimized to prevent I/O bottlenecks from obscuring engine performance.

### **1.1 Server Configuration**

* **Build:** cmake \-DCMAKE\_BUILD\_TYPE=Release .. && make \-j  
* **System:** Ensure ulimit \-n is set to at least 65535\.  
* **Network:** If testing over loopback, ensure the kernel TCP stack is tuned (tcp\_tw\_reuse, tcp\_max\_syn\_backlog).  
* **Storage:** Ensure the data.wal is located on an NVMe drive for accurate persistence testing.

### **1.2 Client Machine**

* Dedicated VM/Container separate from the Server (to avoid CPU contention).  
* 10Gbps+ networking support recommended.

## **2\. Benchmark A: YCSB (Standard KV Validation)**

**Goal:** Establish baseline performance metrics comparable to Redis/Memcached/Cassandra.

**Tool:** [YCSB (Yahoo\! Cloud Serving Benchmark)](https://github.com/brianfrankcooper/YCSB)

### **2.1 Implementation Steps**

Since L3KV speaks HTTP, we will implement a custom YCSB Interface Class.

1. **Clone YCSB:**  
   git clone \[https://github.com/brianfrankcooper/YCSB.git\](https://github.com/brianfrankcooper/YCSB.git)  
   cd YCSB

2. **Create L3KV Binding:**  
   Create a new directory l3kv-binding and implement L3KVClient.java extending com.yahoo.ycsb.DB.  
   * **init()**: Initialize a connection pool (e.g., using Apache HttpClient or OkHttp).  
   * **read(table, key, fields, result)**:  
     * Execute GET /kv/{key}.  
     * Return Status.OK if 200\.  
   * **insert(table, key, values) / update(...)**:  
     * Execute PUT /kv/{key} with the binary payload.  
     * Return Status.OK if 200\.  
   * **delete(table, key)**:  
     * Execute DELETE /kv/{key} (if implemented) or use a "tombstone" mutation.  
3. **Workload Configuration:**  
   Create workloads/workload\_l3kv:  
   recordcount=1000000  
   operationcount=1000000  
   workload=com.yahoo.ycsb.workloads.CoreWorkload  
   readallfields=true

4. **Execution:**  
   * **Load Phase:** ./bin/ycsb load l3kv \-P workloads/workload\_l3kv \-p l3kv.host=localhost  
   * **Run Phase (Workload A \- 50/50):** ./bin/ycsb run l3kv \-P workloads/workload\_l3kv \-p l3kv.host=localhost \-p readproportion=0.5 \-p updateproportion=0.5

### **2.2 Success Metrics**

* **Throughput:** \> 50,000 OPS (single node).  
* **Latency (99th):** \< 5ms.

## **3\. Benchmark B: wrk2 (HTTP Concurrency & Thread Pool Scaling)**

**Goal:** Stress test the Boost.Beast networking layer and the Predictive Autoscaler (SCALING.md).

**Tool:** [wrk2](https://github.com/giltene/wrk2) (Supports Coordinated Omission correction).

### **3.1 Setup**

Install wrk2 (requires OpenSSL and Zlib).

### **3.2 The Zero-Parse Mutation Script (mutate.lua)**

This script forces the server to use the optimized PATCH path.

\-- mutate.lua  
math.randomseed(os.time())

request \= function()  
   \-- Randomly select a key from 1 to 10000  
   local key\_id \= math.random(1, 10000\)  
   local path \= "/kv/user:" .. key\_id .. "?op=set\_int\&field=score\&val=" .. math.random(1, 1000\)  
     
   \-- Send POST (PATCH)  
   return wrk.format("POST", path)  
end

### **3.3 Execution Strategy**

Run the benchmark with increasing concurrency to verify the autoscaler reacts correctly.

1. **Warm-up:**  
   wrk \-t2 \-c100 \-d30s \-R5000 http://localhost:8080/

2. **Saturation Test:**  
   wrk \-t12 \-c1000 \-d60s \-R50000 \-s mutate.lua http://localhost:8080/

### **3.4 Verification**

* **Autoscaler Check:** Monitor L3KV logs. You should see "Scaling Up" log lines as wrk ramps up.  
* **Latency Stability:** The p99 latency should not degrade significantly as connections increase from 100 to 1000\.

## **4\. Benchmark C: Twitter Trace (The "Killer Feature" Validation)**

**Goal:** Demonstrate the specific advantage of "Zero-Parse" over traditional "Read-Modify-Write" (RMW).

**Dataset:** 10,000 JSON Tweets (approx 4KB each).

### **4.1 The Comparison Logic**

You will implement a simple C++ benchmark binary (tests/bench\_twitter.cpp).

**Scenario 1: Traditional RMW (The Control Group)**

1. GET /kv/tweet:1 (Download 4KB).  
2. Parse JSON.  
3. Increment retweet\_count.  
4. Serialize JSON.  
5. PUT /kv/tweet:1 (Upload 4KB).

**Scenario 2: L3KV Zero-Parse (The Test Group)**

1. POST /kv/tweet:1?op=set\_int\&field=retweet\_count\&val=N (Upload 0 bytes body, approx 100 bytes headers).  
2. Server updates integer in place.

### **4.2 Implementation Details**

// Pseudo-code for bench\_twitter.cpp  
void run\_control\_group() {  
    auto start \= now();  
    for(int i=0; i\<10000; \++i) {  
        auto blob \= http\_get("tweet:" \+ to\_string(i));  
        // Simulate parse cost  
        std::this\_thread::sleep\_for(std::chrono::microseconds(50));   
        http\_put("tweet:" \+ to\_string(i), blob);  
    }  
    print\_metrics("Traditional", start);  
}

void run\_test\_group() {  
    auto start \= now();  
    for(int i=0; i\<10000; \++i) {  
        http\_post\_params("tweet:" \+ to\_string(i), "retweet\_count", i);  
    }  
    print\_metrics("L3KV Zero-Parse", start);  
}

### **4.3 Success Metrics**

* **Throughput Delta:** Scenario 2 should be **10x \- 50x faster** than Scenario 1\.  
* **Bandwidth Delta:** Scenario 2 should use **98% less network bandwidth**.

## **5\. Reporting**

Generate a report BENCHMARK\_RESULTS.md containing:

1. **YCSB Summary:** Throughput vs. Latency curve.  
2. **Autoscaler Graph:** Thread Count vs. Request Rate over time.  
3. **Twitter Comparison:** Bar chart showing the massive gap between RMW and Zero-Parse.