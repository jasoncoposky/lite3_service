# **L3KV Future Optimization: The BF-Tree Integration**

**Status:** Research / Roadmap

**Reference:** Microsoft BF-Tree (VLDB '24)

**Problem Space:** Large Blob Write Amplification & High-Contention Locking

## **1\. The "Large Blob" Cliff**

Current L3KV Architecture treats a Lite³ document as a contiguous memory buffer.

* **Small Docs (\<4KB):** Performance is excellent. CPU cache hit rate is high.  
* **Large Docs (\>1MB):** Performance degrades.  
  * **Fragmentation:** std::pmr struggles to find contiguous 1MB slots.  
  * **Write Amplification:** Updating 8 bytes requires rewriting the entire 1MB blob to the WAL.  
  * **Read Waste:** Fetching a single field requires loading the whole 1MB blob into L2 cache.

## **2\. The Solution: "Mini-Page" Architecture (Inspired by BF-Tree)**

Instead of one monolithic Blob, we decompose large documents into a **Linked List of Mini-Pages**.

### **2.1 The Mini-Page Structure**

* **Concept:** A variable-sized memory chunk (e.g., 256B to 4KB).  
* **Composition:**  
  * **Header:** \[Next\_Ptr | CRC32 | Range\_Start | Range\_End\]  
  * **Body:** Raw Lite³ B-Tree nodes.  
* **Benefit:** When a user updates user:100, we only dirty the specific **Mini-Page** containing that key range, not the entire 10MB user profile.

### **2.2 The "Patch Buffer" (Delta Layer)**

BF-Tree introduces a "Patch Layer" that sits *above* the main data pages.

* **Current L3KV:** Writes go directly to the main blob.  
* **Future L3KV:** Writes go to a small **Delta Buffer** attached to the blob.  
  * **Read:** Merge *Delta* \+ *Main Blob* on the fly.  
  * **Flush:** When Delta \> 512B, merge it down into the Main Blob (Compaction).  
* **Gain:** Massive reduction in WAL traffic. We log only the delta.

## **3\. Concurrency: Moving to Epoch-Based RCU**

Current L3KV uses std::shared\_mutex (RW-Lock) per shard.

* **Issue:** Under extreme read load (10M OPS), the atomic ref-counting on the shared\_mutex causes cache-line bouncing between cores.  
* **Solution:** **Epoch-Based Reclamation (EBR)**.  
  * **Readers:** Set a thread-local flag epoch\_active \= true. **Zero atomic writes.**  
  * **Writers:** Create a *new copy* of the Mini-Page (Copy-on-Write), swap the pointer atomically.  
  * **Reclamation:** Old pages are freed only when all Readers have moved to a newer epoch.  
* **Result:** Linear scaling across 128+ cores.

## **4\. Implementation Roadmap (Post-Phase 4\)**

This architecture requires a rewrite of the Blob class in store.hpp.

### **Step 1: The PagedBlob Class**

Create an abstraction that looks like a contiguous buffer to Lite³ but manages a std::vector\<MiniPage\*\> internally.

### **Step 2: The Delta Log**

Modify Engine::patch\_int to append to a std::vector\<OpTuple\> (the Delta) instead of calling lite3\_set\_i64 immediately.

### **Step 3: RCU Integration**

Replace std::shared\_mutex with a lightweight Epoch Manager (e.g., libcds or custom implementation).

## **5\. Trade-offs**

| Feature | Current (Contiguous) | Future (Paged/BF-Tree) |
| :---- | :---- | :---- |
| **Complexity** | Low (Standard Pointer Arithmetic) | **Extreme** (Pointer swizzling, compaction) |
| **Small Doc Perf** | **Unbeatable** | Slower (Pointer chasing overhead) |
| **Large Doc Perf** | Poor (Memory bandwidth bound) | **Excellent** (IOPS bound) |
| **Concurrency** | Good (Up to \~32 cores) | **Perfect** (Linear up to 256 cores) |

**Conclusion:** We stick to the Contiguous model for now. We adopt the BF-Tree model only when we introduce **"Large Document Support"** as a specific feature tier.

## **6\. Work Breakdown Structure (WBS)**

### **Phase 1: The PagedBlob Foundation**

**Goal:** Abstract the contiguous memory requirement away from Lite³ storage.

* **1.1 Data Structures**  
  * **Task:** Define MiniPage struct (Header \+ flexible array member).  
  * **Task:** Define PageHeader (CRC, Sequence, Next Pointer, Range coverage).  
  * **Task:** Implement PagePool allocator using std::pmr for fixed-size blocks (e.g., 4KB).  
* **1.2 The PagedBlob Class**  
  * **Task:** Create PagedBlob class inheriting from a common BlobInterface.  
  * **Task:** Implement read(offset, length) logic to traverse linked MiniPages.  
  * **Task:** Implement write(offset, data) logic to split data across page boundaries.  
  * **Task:** Implement grow(size) logic to allocate new pages from the pool.  
* **1.3 Lite³ Adaptation**  
  * **Task:** Implement a Materializer helper that temporarily assembles a contiguous buffer for Read-only operations (interim step).  
  * **Investigation:** Research scatter/gather read support for Lite³ to avoid materialization.

### **Phase 2: The Delta/Patch Layer**

**Goal:** Enable \<512B writes to \>1MB documents without amplification. This delivers the highest ROI for large documents.

* **2.1 Delta Structures**  
  * **Task:** Define DeltaEntry (OpCode, Path/Key, Value).  
  * **Task:** Implement PatchBuffer (Sorted Vector or tiny in-memory B-Tree).  
* **2.2 Write Path Modification**  
  * **Task:** Modify Engine::patch\_\* to bypass the main PagedBlob.  
  * **Task:** Append operation to PatchBuffer instead of applying immediately.  
  * **Task:** Update WAL logging to log the Delta OpTuple, not the resulting blob.  
* **2.3 Read Path Modification (Overlay)**  
  * **Task:** Implement OverlayReader: Checks PatchBuffer first, falls back to PagedBlob.  
  * **Complexity:** Handle "Read-Modify-Write" where the patch depends on current state (e.g., increment).  
* **2.4 Compaction (The Flush)**  
  * **Task:** Implement CompactionTask triggered when PatchBuffer \> Threshold (e.g., 4KB).  
  * **Task:** Logic: Load PagedBlob, apply all Deltas, generate new Mini-Pages (Copy-on-Write), atomic swap.

### **Phase 3: Epoch-Based Concurrency**

**Goal:** Lock-free reads for high core counts, replacing std::shared\_mutex.

* **3.1 Epoch Manager**  
  * **Task:** Implement global current\_epoch counter.  
  * **Task:** Implement thread-local active\_epoch and quiescent\_state trackers.  
* **3.2 Safe Reclamation**  
  * **Task:** Implement LimboList: List of retired Mini-Pages waiting for safe deletion.  
  * **Task:** Implement try\_reclaim(): Frees pages in LimboList belonging to old epochs.  
* **3.3 Reader Integration**  
  * **Task:** Wrap read operations in epoch\_enter() / epoch\_exit().  
  * **Task:** Remove std::shared\_mutex from the Read path entirely.  
* **3.4 Writer Integration**  
  * **Task:** Implement Copy-on-Write (CoW) for Mini-Page updates.  
  * **Task:** Implement atomic pointer swap to publish new page chains.  
  * **Task:** Retire old pages to LimboList.

### **Phase 4: Integration & Verification**

**Goal:** Production readiness and performance tuning.

* **4.1 Integration**  
  * **Task:** Add configuration flag use\_large\_doc\_support.  
  * **Task:** Switch Store to use PagedBlob dynamically for keys \> 1MB.  
* **4.2 Benchmarking**  
  * **Benchmark A:** Small Doc Write throughput (Regression check vs. Phase 1 contiguous blobs).  
  * **Benchmark B:** 10MB Doc Single Field Update (Target: \<10us latency, Zero WAL amplification).  
  * **Benchmark C:** 100-thread Read/Write contention (Target: Linear scaling with RCU).

## **Appendix A: Reference Implementation (C++)**

This section translates the Rust BfTree architectural primitives into C++23, tailored for the L3KV project structure.

### **A.1 The Mini-Page (Variable Sized Block)**

In Rust, BF-Tree uses explicit layout management. In C++, we use a reinterpret\_cast overlay on std::pmr memory.

\#include \<cstdint\>  
\#include \<atomic\>  
\#include \<memory\_resource\>

// The header sits at the front of every Mini-Page  
struct PageHeader {  
    // Optimistic Lock / Version (for latch-free reads)  
    std::atomic\<uint64\_t\> version\_lock;  
      
    // Physical Linkage  
    MiniPage\* next\_page \= nullptr;  
      
    // Logical Coverage (What key range does this page hold?)  
    uint32\_t range\_start\_offset;  
    uint32\_t range\_end\_offset;  
      
    // Payload sizing  
    uint32\_t payload\_size;  
    uint32\_t capacity;  
      
    // Checksum for on-disk validity  
    uint32\_t crc32;  
};

// The Variable-Sized Page  
struct MiniPage {  
    PageHeader header;  
      
    // Flexible Array Member (C++ standard-compliant via casting)  
    std::byte data\[\];   
      
    // Allocator helper using our PMR pools  
    static MiniPage\* create(std::pmr::memory\_resource\* mr, uint32\_t capacity) {  
        size\_t total\_size \= sizeof(PageHeader) \+ capacity;  
        void\* mem \= mr-\>allocate(total\_size);  
        auto\* page \= new (mem) MiniPage();  
        page-\>header.capacity \= capacity;  
        return page;  
    }  
      
    static void destroy(std::pmr::memory\_resource\* mr, MiniPage\* page) {  
        size\_t total\_size \= sizeof(PageHeader) \+ page-\>header.capacity;  
        page-\>\~MiniPage();  
        mr-\>deallocate(page, total\_size);  
    }  
};

### **A.2 The Delta/Patch Layer**

This structure replaces the immediate mutation. Instead of writing to the MiniPage, we append to this log.

enum class DeltaOp : uint8\_t {  
    SET\_I64 \= 1,  
    SET\_STR \= 2,  
    DELETE\_KEY \= 3  
};

struct DeltaEntry {  
    DeltaOp op;  
    uint16\_t key\_len;  
    uint16\_t val\_len;  
    // Data follows immediately in the buffer  
};

class PatchBuffer {  
    std::pmr::vector\<std::byte\> buffer\_;  
      
public:  
    void append(DeltaOp op, std::string\_view key, std::span\<const std::byte\> val) {  
        // Serialize \[Header\]\[Key\]\[Value\] into buffer\_  
        // This is the "Log" part of LSM / BF-Tree  
    }  
      
    // During Read: Scan backwards to find latest update for 'key'  
    std::optional\<std::span\<const std::byte\>\> lookup(std::string\_view key) {  
        // ... Linear scan or binary search implementation ...  
        return std::nullopt;  
    }  
};

### **A.3 Epoch-Based RCU (Concurrency)**

This replaces std::shared\_mutex. It allows readers to proceed without atomic contention on a central lock.

struct EpochManager {  
    std::atomic\<uint64\_t\> global\_epoch{0};  
      
    // Thread-local registration (Simplified)  
    static thread\_local bool is\_active;  
    static thread\_local uint64\_t local\_epoch;

    void enter\_read() {  
        is\_active \= true;  
        local\_epoch \= global\_epoch.load(std::memory\_order\_acquire);  
    }

    void exit\_read() {  
        is\_active \= false;  
    }

    // Called by Writers to free old MiniPages  
    void reclaim(MiniPage\* old\_page) {  
        // 1\. Add to limbo list  
        // 2\. Check if all threads \> old\_page-\>creation\_epoch  
        // 3\. If yes, MiniPage::destroy(old\_page)  
    }  
};  
