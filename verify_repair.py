import subprocess,serial,time,select
from pathlib import Path
root=Path('/home/a/DirtFlatTop'); cli='/home/a/.local/bin/arduino-cli'
ports={};buf={}
log=(root/'repair_serial.log').open('w')
def upload(port,name):
 with (root/name).open('w') as f:
  subprocess.run([cli,'upload','-p',port,'--fqbn','esp32:esp32:heltec_wifi_lora_32_V3','--input-dir',str(root/'bmp585_monitor/build_repair'),str(root/'bmp585_monitor')],stdout=f,stderr=subprocess.STDOUT,check=True)
def attach(port):
 s=serial.Serial(port=None,baudrate=115200,timeout=0);s.dtr=False;s.rts=False;s.port=port;s.open();ports[s]=port;buf[s]=''
def capture(seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  for s in select.select(list(ports),[],[],0.1)[0]:
   buf[s]+=s.read(4096).decode(errors='replace')
   while '\n' in buf[s]:
    l,buf[s]=buf[s].split('\n',1);out=ports[s]+': '+l.rstrip()+'\n'; print(out,end='',flush=True);log.write(out);log.flush()
try:
 upload('/dev/ttyUSB0','repair_upload_db1.log');attach('/dev/ttyUSB0');capture(9)
 upload('/dev/ttyUSB1','repair_upload_db2.log');attach('/dev/ttyUSB1');capture(22)
finally:
 for s in ports:s.close()
 log.close()
