import usb.core
import usb.util
import time

# find our device
dev = usb.core.find(idVendor=0x04D8, idProduct=0xFF8C)

# was it found?
if dev is None:
    raise ValueError('Device not found')

# set the active configuration. With no arguments, the first
# configuration will be the active one
dev.set_configuration()

# write the data
msg = [0x40];
#while(True):
dev.write(0x01,msg,0,100);
res = dev.read(0x81,16,0,100);
for i in range(16):
    print hex(res[i])
#print res;
    #time.sleep(1);

