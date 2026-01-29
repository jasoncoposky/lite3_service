import requests
import random
import time
import string
import threading
import sys

def random_string(length):
    return ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(length))

def worker(base_url):
    while True:
        try:
            # Vary payload size from 100 bytes to 100KB
            size_kb = random.choices([1, 5, 20, 100], weights=[70, 20, 8, 2])[0]
            payload = random_string(size_kb * 1024)
            key = f"demo_key_{random.randint(0, 1000)}"
            
            # 80% PUT, 20% GET
            if random.random() < 0.8:
                requests.put(f"{base_url}/kv/{key}", data=payload, timeout=10)
            else:
                requests.get(f"{base_url}/kv/{key}", timeout=10)
                
            time.sleep(random.uniform(0.05, 0.2)) # Slightly slower
        except Exception as e:
            print(f"Error: {e}")
            time.sleep(1.0)

if __name__ == "__main__":
    port = 8080
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    
    base_url = f"http://localhost:{port}"
    print(f"Starting traffic generation for {base_url}... Ctrl+C to stop.")
    
    threads = []
    for i in range(5): # 5 concurrent clients
        t = threading.Thread(target=worker, args=(base_url,))
        t.daemon = True
        t.start()
        threads.append(t)
    
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Stopping...")
