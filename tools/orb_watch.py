import serial, time
s = serial.Serial('/dev/cu.usbmodem83201', 115200, timeout=1)
while True:
    line = s.readline().decode('utf-8', 'replace').rstrip()
    if line:
        print(f"[{time.strftime('%H:%M:%S')}] {line}", flush=True)
