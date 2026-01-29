
import asyncio
import aiohttp
import time
import subprocess
import os
import sys
import random

# Configuration
SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
BASE_URL = "http://localhost:8080/kv"
NUM_USERS = 50
DURATION_SECONDS = 20
WARMUP_SECONDS = 5
KEY_PREFIX = "bench_"

async def run_user(user_id, session, end_time):
    op_count = 0
    errors = 0
    
    while time.time() < end_time:
        key = f"{KEY_PREFIX}{user_id}_{random.randint(0, 1000)}"
        value = {"u": user_id, "d": "data", "i": op_count}
        
        try:
            # PUT
            async with session.put(f"{BASE_URL}/{key}", json=value) as resp:
                await resp.read()
                if resp.status != 200: errors += 1
            
            # GET
            async with session.get(f"{BASE_URL}/{key}") as resp:
                await resp.read()
                if resp.status != 200: errors += 1
                    
            op_count += 2
        except:
            errors += 1
            
    return op_count, errors

async def benchmark_run(min_threads, max_threads, label):
    print(f"\n--- Starting Benchmark: {label} (Threads: {min_threads}-{max_threads}) ---")
    proc = subprocess.Popen([SERVICE_EXE, str(min_threads), str(max_threads)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    
    # Wait for startup
    await asyncio.sleep(2)
    
    try:
        async with aiohttp.ClientSession() as session:
            # Warmup
            print("Warming up...")
            end_warmup = time.time() + WARMUP_SECONDS
            tasks = [run_user(i, session, end_warmup) for i in range(10)]
            await asyncio.gather(*tasks)
            
            # Measurement
            print(f"Running load for {DURATION_SECONDS}s...")
            start_time = time.time()
            end_time = start_time + DURATION_SECONDS
            tasks = [run_user(i, session, end_time) for i in range(NUM_USERS)]
            results = await asyncio.gather(*tasks)
            total_time = time.time() - start_time
            
            total_ops = sum(r[0] for r in results)
            throughput = total_ops / total_time
            print(f"Result: {throughput:.2f} req/sec")
            return throughput

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()

async def main():
    print("Compiling Baseline vs dynamic comparisons...")
    
    # 1. Static/Baseline (4 Threads)
    static_rps = await benchmark_run(4, 4, "Static Pool (4 Threads)")
    
    # 2. Dynamic (4-16 Threads)
    dynamic_rps = await benchmark_run(4, 16, "Dynamic Pool (Auto-Scaling)")
    
    print("\n\n=== BENCHMARK RESULTS ===")
    print(f"Static (4 Threads):   {static_rps:.2f} req/sec")
    print(f"Dynamic (Auto-Scaling): {dynamic_rps:.2f} req/sec")
    
    delta = dynamic_rps - static_rps
    pct = (delta / static_rps) * 100
    print(f"Improvement: {delta:+.2f} req/sec ({pct:+.2f}%)")

if __name__ == "__main__":
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
