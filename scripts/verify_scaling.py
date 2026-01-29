
import asyncio
import aiohttp
import time
import subprocess
import os
import sys

# Configuration
SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
BASE_URL = "http://localhost:8080"
NUM_USERS = 50
DURATION_SECONDS = 15
MIN_THREADS = 4
MAX_THREADS = 16

async def poll_metrics(session, stop_event):
    print("Starting metrics poller...")
    max_threads_seen = 0
    while not stop_event.is_set():
        try:
            async with session.get(f"{BASE_URL}/metrics") as resp:
                if resp.status == 200:
                    data = await resp.json()
                    # Check for system.thread_count (JSON format)
                    # Implementation details suggest it's under "system" -> "thread_count"
                    # But wait, my get_json implementation puts it under "system" -> "thread_count"
                    thread_count = data.get("system", {}).get("thread_count", 0)
                    active_conns = data.get("system", {}).get("active_connections", 0)
                    
                    if thread_count > max_threads_seen:
                        max_threads_seen = thread_count
                        
                    print(f"[METRICS] Threads: {thread_count} | Active Conns: {active_conns}")
        except Exception as e:
            pass # Service might not be ready
        await asyncio.sleep(0.5)
    return max_threads_seen

async def load_generator(session, stop_event):
    print("Starting load generator...")
    while not stop_event.is_set():
        try:
            async with session.get(f"{BASE_URL}/kv/health") as resp:
                await resp.read()
        except:
            pass
        # No sleep -> hammering

async def main():
    print(f"Launching Service: {SERVICE_EXE} {MIN_THREADS} {MAX_THREADS}")
    proc = subprocess.Popen([SERVICE_EXE, str(MIN_THREADS), str(MAX_THREADS)])
    
    # Wait for startup
    await asyncio.sleep(2)
    
    stop_event = asyncio.Event()
    
    async with aiohttp.ClientSession() as session:
        # Start metrics poller task
        metrics_task = asyncio.create_task(poll_metrics(session, stop_event))
        
        # Start load
        load_tasks = [asyncio.create_task(load_generator(session, stop_event)) for _ in range(NUM_USERS)]
        
        print(f"Running load for {DURATION_SECONDS} seconds...")
        await asyncio.sleep(DURATION_SECONDS)
        
        # Stop load
        stop_event.set()
        await asyncio.gather(*load_tasks)
        
        max_threads = await metrics_task
        print(f"\nMax Threads Observed: {max_threads}")
        
        if max_threads > MIN_THREADS:
            print("SUCCESS: Thread pool scaled up!")
        else:
            print("WARNING: Thread pool did NOT scale up. Load might be insufficient or logic conservative.")

    print("Terminating service...")
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except:
        proc.kill()

if __name__ == "__main__":
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
