
import asyncio
import aiohttp
import time
import json
import random
import subprocess
import os
import signal
import sys

# Configuration
SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
BASE_URL = "http://localhost:8080/kv"
NUM_USERS = 50
DURATION_SECONDS = 30
WARMUP_SECONDS = 5
KEY_PREFIX = "bench_"

async def run_user(user_id, session, end_time):
    op_count = 0
    errors = 0
    total_latency = 0
    
    while time.time() < end_time:
        key = f"{KEY_PREFIX}{user_id}_{random.randint(0, 1000)}"
        value = {"u": user_id, "d": "some data", "i": op_count}
        
        start = time.time()
        try:
            # PUT
            async with session.put(f"{BASE_URL}/{key}", json=value, headers={"Connection": "close"}) as resp:
                await resp.read()
                if resp.status != 200:
                    errors += 1
            
            # GET
            async with session.get(f"{BASE_URL}/{key}", headers={"Connection": "close"}) as resp:
                await resp.read()
                if resp.status != 200:
                    errors += 1
                    
            op_count += 2 # PUT + GET
            
        except Exception as e:
            errors += 1
            # print(f"User {user_id} error: {e}")
            
        lat = time.time() - start
        total_latency += lat
        
        # Small sleep to avoid overwhelming local network stack immediately
        # await asyncio.sleep(0.001) 

    return op_count, errors, total_latency

async def main():
    print(f"Starting service: {SERVICE_EXE}")
    service_process = subprocess.Popen([SERVICE_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Give it a moment to start
    time.sleep(2)
    
    print(f"Benchmarking with {NUM_USERS} users for {DURATION_SECONDS} seconds...")
    
    async with aiohttp.ClientSession() as session:
        # Warmup
        print("Warming up...")
        end_warmup = time.time() + WARMUP_SECONDS
        tasks = [run_user(i, session, end_warmup) for i in range(10)]
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
        agg_latency = sum(r[2] for r in results) # Sum of average latencies per user block? No, total time spent.
        
        # This latency calculation is crude (total time / total ops across all users).
        # AIOHTTP overhead is included.
        avg_latency_ms = (agg_latency / total_ops) * 1000 if total_ops > 0 else 0
        
        print("\n--- Client Side Metrics ---")
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
                else:
                    print(f"Failed to get metrics: {resp.status}")
        except Exception as e:
            print(f"Error fetching metrics: {e}")

    print("Stopping service...")
    service_process.terminate()
    try:
        service_process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        service_process.kill()

if __name__ == "__main__":
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
