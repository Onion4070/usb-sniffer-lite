import serial

try:
    ser = serial.Serial('COM24', 9600, timeout=1)
except serial.SerialException as e:
    print(f"Error opening serial port: {e}")
    ser = None
    exit()

print("Serial port initialized.")
ser.write(b's\r\n')

while True:
    buffer = ser.readline() if ser else b''
    if buffer:
        try:
            print(f"{buffer.decode().strip()}")
        except UnicodeDecodeError as e:
            print(f"Decoding error: {e}")