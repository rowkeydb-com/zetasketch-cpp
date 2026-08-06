#!/usr/bin/env python3
import subprocess
import sys

def run_cmd(cmd, input_data):
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = process.communicate(input_data + "\n")
    if process.returncode != 0:
        print(f"Error running {' '.join(cmd)}: {err}", file=sys.stderr)
        sys.exit(1)
    return out

def fuzz_create(cpp_bin, java_bin, np, sp, num):
    print(f"Testing Create NP={np}, SP={sp}, Elements={num}...", end="", flush=True)
    items = [f"item_{i}" for i in range(num)]
    input_data = "\n".join(items)
    
    java_out = run_cmd([java_bin, "CREATE", str(np), str(sp)], input_data).strip()
    cpp_out = run_cmd([cpp_bin, "CREATE", str(np), str(sp)], input_data).strip()
    
    if java_out != cpp_out:
        print(" FAILED!")
        print(f"Mismatch for Create NP={np}, SP={sp}, Elements={num}")
        print(f"Java: {java_out[:100]}...")
        print(f"C++ : {cpp_out[:100]}...")
        sys.exit(1)
    print(" OK")

def fuzz_merge(cpp_bin, java_bin, np, sp, num_sketches, items_per_sketch):
    print(f"Testing Merge NP={np}, SP={sp}, {num_sketches} sketches of {items_per_sketch} items...", end="", flush=True)

    java_hexes = []
    cpp_hexes = []
    for i in range(num_sketches):
        input_data = "\n".join([f"test_{i}_{j}" for j in range(items_per_sketch)])
        
        java_out = run_cmd([java_bin, "CREATE", str(np), str(sp)], input_data)
        cpp_out = run_cmd([cpp_bin, "CREATE", str(np), str(sp)], input_data)
        
        java_hexes.append(java_out.strip())
        cpp_hexes.append(cpp_out.strip())
        
    java_merge_in = "\n".join(java_hexes)
    cpp_merge_in = "\n".join(cpp_hexes)

    java_merged = run_cmd([java_bin, "MERGE", str(np), str(sp)], java_merge_in).strip()
    cpp_merged = run_cmd([cpp_bin, "MERGE", str(np), str(sp)], cpp_merge_in).strip()

    if java_merged != cpp_merged:
        print(" FAILED!")
        print(f"Mismatch for Merge NP={np}, SP={sp}")
        print(f"Java: {java_merged}")
        print(f"C++ : {cpp_merged}")
        sys.exit(1)
    print(" OK")

def main():
    if len(sys.argv) != 3:
        print("Usage: differential_fuzzer.py <cpp_bin> <java_bin>")
        sys.exit(1)

    cpp_bin = sys.argv[1]
    java_bin = sys.argv[2]

    print("Starting Differential Fuzzer...")

    configs = [
        (15, 20),
        (10, 15),
        (15, 0),
        (10, 0)
    ]

    elements = [10, 100, 1000, 5000]

    for np, sp in configs:
        for num in elements:
            fuzz_create(cpp_bin, java_bin, np, sp, num)

    print("Testing Merges...")
    for np, sp in configs:
        fuzz_merge(cpp_bin, java_bin, np, sp, 3, 100) # Merge 3 sparse sketches
        fuzz_merge(cpp_bin, java_bin, np, sp, 3, 2000) # Merge 3 normal sketches
        fuzz_merge(cpp_bin, java_bin, np, sp, 10, 200) # Mixed combinations

    print("All differential fuzzing tests passed.")

if __name__ == "__main__":
    main()
