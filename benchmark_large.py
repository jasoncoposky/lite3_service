
import asyncio
import aiohttp
import time
import random
import os
import sys
import subprocess

# Configuration
SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
BASE_URL = "http://localhost:8080/kv"
NUM_USERS = 20
DURATION_SECONDS = 15
WARMUP_SECONDS = 3
KEY_PREFIX = "large_"
# 256KB payload
PAYLOAD_SIZE = 256 * 1024
PAYLOAD = "X" * PAYLOAD_SIZE

async def run_user(user_id, session, end_time):
    op_count = 0
    errors = 0
    total_latency = 0
    
    while time.time() < end_time:
        key = f"{KEY_PREFIX}{user_id}_{random.randint(0, 100)}"
        value = {"u": user_id, "data": PAYLOAD, "i": op_count}
        
        start = time.time()
        try:
            # PUT
            async with session.put(f"{BASE_URL}/{key}", json=value) as resp:
                await resp.read()
                if resp.status != 200:
                    errors += 1
            
            op_count += 1
            
        except Exception as e:
            errors += 1
            
        lat = time.time() - start
        total_latency += lat
        
    return op_count, errors, total_latency

async def main():
    print(f"Starting service: {SERVICE_EXE}")
    # Remove old wal if exists to have clean perf start
    if os.path.exists("data.wal"):
        try: os.remove("data.wal")
        except: pass

    service_process = subprocess.Popen([SERVICE_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Give it a moment to start
    time.sleep(2)
    
    print(f"Benchmarking LARGE WRITES ({PAYLOAD_SIZE/1024} KB) with {NUM_USERS} users...")
    
    async with aiohttp.ClientSession() as session:
        # Warmup
        print("Warming up...")
        end_warmup = time.time() + WARMUP_SECONDS
        tasks = [run_user(i, session, end_warmup) for i in range(5)]
        await asyncio.gather(*tasks)
        print("Warmup done.")
        
        # Measurement
        print("Running benchmark...")
        start_time = time.time()
        end_time = start_time + DURATION_SECONDS
        tasks = [run_user(i, session, end_time) for i in range(NUM_USERS)]
        
        results = await asyncio.gather(*tasks)
        total_time = time.time() - start_time
        
        total_ops = sum(r[0] for r in results)
        total_errors = sum(r[1] for r in results)
        agg_latency = sum(r[2] for r in results)
        
        avg_latency_ms = (agg_latency / total_ops) * 1000 if total_ops > 0 else 0
        
        print("\n--- Client Side Metrics (Large Writes) ---")
        print(f"Total Ops: {total_ops}")
        print(f"Errors: {total_errors}")
        print(f"Duration: {total_time:.2f}s")
        print(f"Throughput: {total_ops / total_time:.2f} req/sec")
        print(f"Avg Latency: {avg_latency_ms:.4f} ms")

        # Get Service Internal Metrics
        print("\nFetching Internal Metrics...")
        try:
            async with session.get(f"{BASE_URL}/metrics") as resp:
                if resp.status == 200:
                    print(await resp.text())
        except Exception as e:
            print(f"Error fetching metrics: {e}")

    print("Stopping service...")
    service_process.terminate()
    try:
        service_process.wait(timeout=5)
    except:
        service_process.kill()

if __name__ == "__main__":
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
