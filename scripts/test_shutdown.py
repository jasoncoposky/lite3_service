import subprocess
import time
import requests
import sys
import signal
import os

def test_graceful_shutdown():
    print("Starting service...")
    # Start service in a new process group to allow sending CTRL_C_EVENT
    proc = subprocess.Popen(["build\\Release\\l3svc.exe"], 
                            stdout=subprocess.PIPE, 
                            stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    
    time.sleep(2) # Wait for startup

    print("Sending PUT...")
    try:
        requests.put("http://localhost:8080/kv/shutdown_test", data="test_val")
    except Exception as e:
        print(f"Failed to connect: {e}")
        proc.kill()
        sys.exit(1)

    print("Sending CTRL_BREAK_EVENT...")
    os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
    
    # os.kill(proc.pid, signal.CTRL_C_EVENT) # Windows specific
    # But checking if regular taskkill (SIGTERM equivalent) works with asio::signal_set

    try:
        stdout, stderr = proc.communicate(timeout=5)
        print("Service exited.")
        print("STDOUT:", stdout.decode())
        print("STDERR:", stderr.decode())
        
        if b"Stopping server..." in stdout or b"Server stopped" in stdout:
            print("SUCCESS: Graceful shutdown detected.")
            if b"WAL Metrics" in stdout or b"Bytes Written" in stdout:
                 print("SUCCESS: Metrics dumped.")
        else:
            print("FAILURE: No graceful shutdown message.")
            sys.exit(1)

    except subprocess.TimeoutExpired:
        print("FAILURE: Service did not exit in time.")
        proc.kill()
        sys.exit(1)

if __name__ == "__main__":
    test_graceful_shutdown()
