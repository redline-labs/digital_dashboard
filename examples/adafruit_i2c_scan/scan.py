import board
import busio

print("Scanning I2C bus...")

i2c = busio.I2C(board.SCL, board.SDA)

while not i2c.try_lock():
    pass

try:
    devices = i2c.scan()
    if not devices:
        print("No I2C devices found.")
    else:
        print("Found {} devices:".format(len(devices)))
        for device in devices:
            print(" - 0x{:02x}".format(device))

finally:
    i2c.unlock()

print("Scan complete.") 