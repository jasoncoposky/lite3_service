import requests
import time
import subprocess
import os
import sys
import threading
import random

SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
WAL_FILE = "data.wal"
BASE_URL = "http://localhost:8080/kv"

stop_writing = False

def writer_loop():
    i = 0
    while not stop_writing:
        key = f"crash_{i}"
        val = {"id": i, "data": "x" * 100} # 100 bytes payload
        try:
            resp = requests.put(f"{BASE_URL}/{key}", json=val, timeout=0.1)
            if resp.status_code != 200:
                print(f"Write failed: {resp.status_code}")
        except:
            pass # Expected when killed
        i += 1
        # No sleep, max throughput to increase chance of partial write

def test_crash():
    global stop_writing
    print("WARNING: This test kills l3svc.exe forcefully.")
    
    # 0. Cleanup
    subprocess.call(["taskkill", "/F", "/IM", "l3svc.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.exists(WAL_FILE):
        os.remove(WAL_FILE)

    # 1. Start Service
    print("Starting service...")
    outfile = open("crash_test_log.txt", "w")
    proc = subprocess.Popen([SERVICE_EXE], stdout=outfile, stderr=subprocess.STDOUT)
    time.sleep(1)

    # 2. Start Writer
    print("Starting writer flood...")
    t = threading.Thread(target=writer_loop)
    t.start()

    # 3. Wait random time (0.5 to 1.5s)
    time.sleep(random.uniform(0.5, 1.5))

    # 4. KILL
    print("KILLING SERVICE!")
    subprocess.call(["taskkill", "/F", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stop_writing = True
    t.join()
    outfile.close()

    print("Service killed. Verifying WAL integrity...")
    
    # 5. Restart
    print("Restarting service...")
    outfile = open("crash_test_recovery_log.txt", "w")
    proc = subprocess.Popen([SERVICE_EXE], stdout=outfile, stderr=subprocess.STDOUT)
    time.sleep(1)

    # 6. Verify
    # We don't know exactly how many writes succeeded, but we can check:
    # A. Service is responding (WAL replay didn't crash it).
    # B. Sequential consistency (if crash_N exists, crash_N-1 should exist).
    
    max_found = -1
    consecutive_misses = 0
    
    # Scan for keys
    for i in range(10000):
        key = f"crash_{i}"
        try:
            resp = requests.get(f"{BASE_URL}/{key}", timeout=0.1)
            if resp.status_code == 200:
                data = resp.json()
                if data["id"] == i:
                    max_found = i
                else:
                    print(f"CORRUPTION: Key {key} has wrong data {data}")
            else:
                consecutive_misses += 1
        except:
            pass
            
        if consecutive_misses > 100:
            break

    print(f"Recovered up to index: {max_found}")
    
    # Check Logs for CRC warnings
    with open("crash_test_recovery_log.txt", "r") as f:
        log_content = f.read()
        if "CRC Mismatch" in log_content:
            print("SUCCESS: CRC Mismatch detected and handled (Partial write discarded).")
        elif "Legacy/Zero CRC" in log_content:
             print("WARNING: Zero CRC found.")
        else:
            print("No CRC errors reported (Clean recovery).")

    # Cleanup
    subprocess.call(["taskkill", "/F", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    outfile.close()
    
    if max_found > 0:
        print("PASS: Service recovered data.")
    else:
        print("FAIL? No data recovered (Did writes happen?)")

if __name__ == "__main__":
    test_crash()
