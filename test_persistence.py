import requests
import time
import subprocess
import os
import sys

SERVICE_EXE = os.path.join("build", "Release", "l3svc.exe")
WAL_FILE = "data.wal"
BASE_URL = "http://localhost:8080/kv"

def start_service():
    print("Starting service...")
    outfile = open("service_test_log.txt", "w")
    proc = subprocess.Popen([SERVICE_EXE], stdout=outfile, stderr=subprocess.STDOUT)
    time.sleep(1) # Wait for startup
    return proc

def kill_service(proc):
    print("Stopping service...")
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()

def test_persistence():
    # 1. Clean state
    if os.path.exists(WAL_FILE):
        try:
            os.remove(WAL_FILE)
        except OSError:
            pass # Maybe locked or gone

    # 2. Start
    proc = start_service()

    try:
        # 3. Write data
        print("Writing data...")
        kv = {"persist": True, "id": 12345}
        resp = requests.put(f"{BASE_URL}/p1", json=kv)
        if resp.status_code != 200:
            print(f"Failed to PUT: {resp.text}")
            sys.exit(1)
        
        # Verify write
        resp = requests.get(f"{BASE_URL}/p1")
        if resp.json() != kv:
            print("Initial read mismatch!")
            sys.exit(1)

    finally:
        kill_service(proc)

    print("Service stopped. Restarting...")
    # 4. Restart
    proc = start_service()

    try:
        # 5. Read back
        print("Reading data after restart...")
        resp = requests.get(f"{BASE_URL}/p1")
        if resp.status_code != 200:
            print(f"Failed to GET after restart: {resp.status_code} {resp.text}")
            sys.exit(1)
        
        data = resp.json()
        print(f"Got: {data}")
        
        kv = {"persist": True, "id": 12345}
        if data == kv:
            print("SUCCESS: Data persisted correctly!")
        else:
            print(f"FAILURE: Data mismatch. Expected {kv}, got {data}")
            sys.exit(1)

    finally:
        kill_service(proc)

if __name__ == "__main__":
    test_persistence()
