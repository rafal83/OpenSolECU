"""Exercise OpenSolECU's local USB console. No IEEE 802.15.4 transmit commands."""
import argparse,pathlib,time,re,json
import serial
from esptool.reset import HardReset
p=argparse.ArgumentParser();p.add_argument('--port',default='COM3');p.add_argument('--reset',action='store_true');p.add_argument('--seconds',type=float,default=15);p.add_argument('--send',action='append',default=[]);p.add_argument('--commands-file');p.add_argument('--log',default='dist/private/serial.log');a=p.parse_args()
commands=json.loads(pathlib.Path(a.commands_file).read_text(encoding='utf-8-sig')) if a.commands_file else [{'at':(3 if a.reset else .5)+i*2,'command':c} for i,c in enumerate(a.send)]
path=pathlib.Path(a.log);path.parent.mkdir(parents=True,exist_ok=True)
s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.15
# Match ESP-IDF Monitor: assert both before open, release RTS then DTR.
# Opening with both deasserted can accidentally select USB download mode on Windows.
s.dtr=True;s.rts=True;s.open();s.rts=False;s.dtr=s.dtr;s.dtr=False
pending=b''
try:
    if a.reset: HardReset(s,uses_usb=True)()
    started=time.monotonic();sent=0
    with path.open('ab') as log:
        while time.monotonic()-started<a.seconds:
            elapsed=time.monotonic()-started
            if sent<len(commands) and elapsed>commands[sent]['at']:
                s.write((commands[sent]['command']+'\n').encode());sent+=1
            data=s.read(s.in_waiting or 1)
            if not data:continue
            log.write(data);log.flush();pending+=data
            while b'\n' in pending:
                line,pending=pending.split(b'\n',1)
                text=line.decode('utf-8','replace').rstrip('\r')
                text=re.sub(r'((?:AP|Admin) password:).*',r'\1 [recorded in private serial log]',text)
                print(text,flush=True)
finally:
    s.close()
