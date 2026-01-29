
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
PAYLOAD_SIZE = 1024 * 1024 # 1MB
PAYLOAD = "X" * PAYLOAD_SIZE

async def wait_for_service():
    for _ in range(10):
        try:
            async with aiohttp.ClientSession() as session:
                async with session.get(f"{BASE_URL}/metrics", timeout=1) as resp:
                    if resp.status == 200:
                        return True
        except:
            pass
        await asyncio.sleep(0.5)
    return False

def kill_service():
    subprocess.run(["taskkill", "/F", "/IM", "l3svc.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

async def test_crash_during_write(cycle):
    print(f"\n--- Crash Test Cycle {cycle} ---")
    
    # 1. Clean start
    if os.path.exists("data.wal"):
        try: os.remove("data.wal")
        except: pass
        
    proc = subprocess.Popen([SERVICE_EXE, "4"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not await wait_for_service():
        print("Failed to start service")
        return

    print("Service started. Writing sentinel record...")
    async with aiohttp.ClientSession() as session:
        await session.put(f"{BASE_URL}/sentinel_graceful", json={"data": "persisted"})
        
        print("Triggering LARGE WRITE and killing service INSTANTLY...")
        try:
            write_task = asyncio.create_task(session.put(f"{BASE_URL}/crash_test_key", json={"data": PAYLOAD}))
            await asyncio.sleep(0.01) # Tiny wait
            kill_service()
            print("Process KILLED forcefully.")
        except Exception as e:
            print(f"Request error during kill: {e}")

    # 2. Restart and Verify
    print("Restarting service to verify recovery...")
    proc = subprocess.Popen([SERVICE_EXE, "4"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not await wait_for_service():
        print("Failed to restart service")
        return

    async with aiohttp.ClientSession() as session:
        print("Verifying sentinel...")
        async with session.get(f"{BASE_URL}/sentinel_graceful") as resp:
            if resp.status == 200:
                print("SUCCESS: Sentinel recovered.")
            else:
                print("FAILURE: Sentinel LOST.")
        
        print("Verifying crash_test_key...")
        async with session.get(f"{BASE_URL}/crash_test_key") as resp:
            if resp.status == 200:
                print("INFO: Large record recovered (libconveyor flushed some data).")
            else:
                print("INFO: Large record LOST (Buffered data lost as expected).")

    kill_service()

if __name__ == "__main__":
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(test_crash_during_write(1))
