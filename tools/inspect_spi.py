import re
import struct

with open('/home/root/pdj/EP147', 'rb') as f:
    data = f.read()

print("EP147 binary loaded, size:", len(data))

# Search for subucom_spi strings
spi_strings = [b"subucom_spi1.0", b"subucom_spi2.0", b"subucom_spi3.0", b"subucom_spi5.0", b"/dev/subucom_"]
for s in spi_strings:
    pos = 0
    while True:
        idx = data.find(s, pos)
        if idx == -1:
            break
        print("Found %s at 0x%08x" % (s.decode('ascii', errors='ignore'), idx))
        pos = idx + 1
