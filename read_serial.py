import os
import time

port = "/dev/ttyACM0"
baud = 115200

# Set baud rate using stty
os.system(f"stty -F {port} {baud} raw -echo")

try:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    print(f"Opened {port}, waiting for data...")
    
    start_time = time.time()
    while time.time() - start_time < 30:
        data = os.read(fd, 1024)
        if data:
            print(data.decode('utf-8', errors='ignore'), end='', flush=True)
        time.sleep(0.1)
    
    os.close(fd)
except Exception as e:
    print(f"Error: {e}")
