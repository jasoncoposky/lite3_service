import requests
import json
import time
import threading
import subprocess
import os
import sys
import signal # Import the signal module

BASE_URL = "http://localhost:8080/kv"
NUM_ITERATIONS = 100
NUM_CONCURRENT_USERS = 10
SERVICE_PATH = "C:/Users/jason/playground/lite3_service/build/Debug/l3svc.exe"
LOG_DIR = "C:/Users/jason/.gemini/tmp/1e7997a9e83de61b69763069443d8b71edccf7771d2770393c537c52f9ac39d8"
OUTPUT_LOG = os.path.join(LOG_DIR, "l3svc_output.log")
ERROR_LOG = os.path.join(LOG_DIR, "l3svc_error.log")

service_process = None

def start_service():
    global service_process
    print(f"Starting service: {SERVICE_PATH}")
    # Ensure log directory exists
    os.makedirs(LOG_DIR, exist_ok=True)
    with open(OUTPUT_LOG, "w") as out_file, open(ERROR_LOG, "w") as err_file:
        service_process = subprocess.Popen(
            [SERVICE_PATH],
            stdout=out_file,
            stderr=err_file,
            creationflags=subprocess.DETACHED_PROCESS if sys.platform == "win32" else 0
        )
    print(f"Service started with PID: {service_process.pid}")
    time.sleep(2) # Give the service a moment to start up
    # Verify service is listening
    try:
        # Use a simple GET to root or a health check endpoint if available
        # For now, just attempt to connect
        requests.get(f"{BASE_URL}/health", timeout=1) # A health endpoint is ideal
    except requests.exceptions.ConnectionError:
        print("Service did not start listening in time. Check logs for errors.")
        sys.exit(1)


def stop_service():
    global service_process
    if service_process and service_process.poll() is None:
        print(f"Stopping service with PID: {service_process.pid}")
        try:
            if sys.platform == "win32":
                # Terminate the process on Windows
                subprocess.run(["taskkill", "/F", "/PID", str(service_process.pid)], check=True, capture_output=True)
            else:
                # Send SIGTERM on Unix-like systems
                os.kill(service_process.pid, signal.SIGTERM)
            service_process.wait(timeout=5) # Wait for process to terminate
            print("Service stopped successfully.")
        except subprocess.CalledProcessError as e:
            print(f"Error terminating process with taskkill: {e.stderr.decode()}")
        except ProcessLookupError:
            print(f"Process with PID {service_process.pid} already terminated.")
        except Exception as e:
            print(f"An unexpected error occurred during service termination: {e}")
    elif service_process:
        print(f"Service (PID: {service_process.pid}) already stopped or finished.")
    else:
        print("No service process to stop.")
    
    # Optional: Clean up log files after testing
    # if os.path.exists(OUTPUT_LOG):
    #     os.remove(OUTPUT_LOG)
    # if os.path.exists(ERROR_LOG):
    #     os.remove(ERROR_LOG)


def _canonicalize_json_bytes(json_bytes):
    # Load and dump to ensure canonical representation (sorted keys, no extra spaces)
    return json.dumps(json.loads(json_bytes), sort_keys=True, separators=(',', ':')).encode('utf-8')

def _do_put(key, value):
    expected_value_bytes = json.dumps(value, sort_keys=True, separators=(',', ':')).encode('utf-8')
    r = requests.put(f"{BASE_URL}/{key}", data=expected_value_bytes)
    assert r.status_code == 200, f"PUT failed for key '{key}' with status code {r.status_code}: {r.text}"
    return expected_value_bytes

def _do_get(key, expected_value_bytes=None):
    r = requests.get(f"{BASE_URL}/{key}")
    if expected_value_bytes is None:
        assert r.status_code == 404, f"GET for '{key}' unexpectedly found with status code {r.status_code}"
    else:
        assert r.status_code == 200, f"GET failed for key '{key}' with status code {r.status_code}: {r.text}"
        retrieved_value_bytes = _canonicalize_json_bytes(r.content)
        assert retrieved_value_bytes == expected_value_bytes, f"GET content mismatch for key '{key}'. Expected {expected_value_bytes}, got {retrieved_value_bytes}"
    return r.status_code, r.content

def _do_patch(key, op_type, field, value):
    r = requests.post(f"{BASE_URL}/{key}?op={op_type}&field={field}&val={value}")
    assert r.status_code == 200, f"PATCH failed for key '{key}' op '{op_type}' with status code {r.status_code}: {r.text}"

def _do_delete(key):
    r = requests.delete(f"{BASE_URL}/{key}")
    assert r.status_code == 200, f"DELETE failed for key '{key}' with status code {r.status_code}: {r.text}"

def test_put_get():
    key = "test_key_simple"
    value = {"a": 1, "b": "hello"}
    expected_value_bytes = _do_put(key, value)
    _do_get(key, expected_value_bytes)

def test_put_get_complex_types():
    key = "test_key_complex_types"
    value = {
        "int_val": 123,
        "float_val": 45.67,
        "bool_true": True,
        "bool_false": False,
        "null_val": None,
        "str_val": "complex string with spaces and symbols!@#$",
        "empty_str": ""
    }
    expected_value_bytes = _do_put(key, value)
    _do_get(key, expected_value_bytes)

def test_put_get_nested_json():
    key = "test_key_nested_json"
    value = {
        "level1": {
            "level2_arr": [
                {"item1": 1},
                {"item2": "two"}
            ],
            "level2_obj": {
                "nested_key": True,
                "another_val": 99.99
            }
        },
        "top_level_list": [10, 20, "thirty"]
    }
    expected_value_bytes = _do_put(key, value)
    _do_get(key, expected_value_bytes)

def test_patch_int():
    key = "patch_key_simple"
    value = {"a": 1, "b": "hello"}
    _do_put(key, value)
    _do_patch(key, "set_int", "a", 123)

    r_code, r_content = _do_get(key)
    retrieved_value = json.loads(r_content)
    assert retrieved_value["a"] == 123, f"PATCH int verification failed. Expected 123, got {retrieved_value['a']}"
    assert retrieved_value["b"] == "hello", f"PATCH int verification failed. Expected 'hello', got {retrieved_value['b']}"

def test_delete_key():
    key = "test_key_to_delete"
    value = {"a": 1}
    _do_put(key, value)
    _do_delete(key)
    _do_get(key) # Should return 404

def run_performance_tests():
    print("\n--- Running Performance Tests ---")
    put_get_times = []
    patch_int_times = []

    for i in range(NUM_ITERATIONS):
        put_get_times.append(_run_test_and_time(test_put_get))
        patch_int_times.append(_run_test_and_time(test_patch_int))
    
    print(f"Put/Get (n={NUM_ITERATIONS}): Avg={sum(put_get_times)/NUM_ITERATIONS:.4f}s, Min={min(put_get_times):.4f}s, Max={max(put_get_times):.4f}s")
    print(f"Patch Int (n={NUM_ITERATIONS}): Avg={sum(patch_int_times)/NUM_ITERATIONS:.4f}s, Min={min(patch_int_times):.4f}s, Max={max(patch_int_times):.4f}s")

def _stress_test_user(user_id):
    for i in range(NUM_ITERATIONS // NUM_CONCURRENT_USERS):
        key = f"stress_key_{user_id}_{i}"
        value = {"user": user_id, "iteration": i, "timestamp": time.time()}
        
        try:
            _do_put(key, value)
            _do_get(key, _canonicalize_json_bytes(json.dumps(value).encode('utf-8')))

            if i % 2 == 0:
                patch_val = i * 10
                _do_patch(key, "set_int", "iteration", patch_val)
                
                patched_value = value.copy()
                patched_value["iteration"] = patch_val
                _do_get(key, _canonicalize_json_bytes(json.dumps(patched_value).encode('utf-8')))

        except Exception as e:
            print(f"User {user_id} encountered an error: {e}")

def run_stress_tests():
    print("\n--- Running Stress Tests ---")
    threads = []
    for i in range(NUM_CONCURRENT_USERS):
        thread = threading.Thread(target=_stress_test_user, args=(i,))
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()
    print("Stress tests finished.")

if __name__ == "__main__":
    try:
        start_service()
        print("Starting client tests...")
        
        print("Running test_put_get...")
        test_put_get()
        print("test_put_get passed.")

        print("Running test_put_get_complex_types...")
        test_put_get_complex_types()
        print("test_put_get_complex_types passed.")

        print("Running test_put_get_nested_json...")
        test_put_get_nested_json()
        print("test_put_get_nested_json passed.")

        print("Running test_patch_int...")
        test_patch_int()
        print("test_patch_int passed.")

        print("Running test_delete_key...")
        test_delete_key()
        print("test_delete_key passed.")
        
        print("All basic functional tests passed!")

        # run_performance_tests()
        # run_stress_tests()
        
        print("\nAll client tests completed.")
    finally:
        stop_service()