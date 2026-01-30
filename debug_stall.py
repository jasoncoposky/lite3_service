
import requests
import time
import sys

print("Starting debug run...")

# 1. Check health
try:
    r = requests.get("http://localhost:8080/kv/metrics")
    print(f"Initial Metrics Status: {r.status_code}")
except Exception as e:
    print(f"Failed to connect: {e}")
    sys.exit(1)

# 2. Loop
for i in range(1000):
    try:
        # Just use patch_str as it triggers the Merkle path
        payload = f"val_{i}"
        
        # Using special debug_key endpoint for ease? 
        # Or standard REST API?
        # PATCH /kv/key seems not standard in l3svc?
        # l3svc supports 'op=set_str' query param on GET/POST?
        # Let's check http_server.cpp...
        # It supports:
        # PUT /kv/{key} -> body is value / json
        # POST /kv/{key}?op=patch_str&field=...&val=...
        
        url = f"http://localhost:8080/kv/key_{i}?op=set_str&field=f&val={payload}"
        r = requests.post(url)
        
        if r.status_code != 200:
            print(f"Error at {i}: {r.status_code} {r.text}")
            break
            
        if i % 100 == 0:
            print(f"Completed {i}")
            
    except Exception as e:
        print(f"Exception at {i}: {e}")
        break

print("Finished loop.")
