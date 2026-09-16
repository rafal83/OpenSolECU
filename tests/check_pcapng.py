"""Independent reader validates the C++ writer's Wireshark-compatible output."""
import pathlib,struct
data=pathlib.Path('tests/capture-test.pcapng').read_bytes()
pos=0;interfaces=0;packets=0
while pos<len(data):
    kind,length=struct.unpack_from('<II',data,pos)
    assert length>=12 and length%4==0 and pos+length<=len(data)
    assert struct.unpack_from('<I',data,pos+length-4)[0]==length
    if kind==0x0a0d0d0a:
        assert struct.unpack_from('<IHH',data,pos+8)==(0x1a2b3c4d,1,0)
    elif kind==1:
        assert struct.unpack_from('<HHI',data,pos+8)==(230,0,125)
        interfaces+=1
    elif kind==6:
        interface,hi,lo,cap,original=struct.unpack_from('<IIIII',data,pos+8)
        assert interface==5 and ((hi<<32)|lo)==1704067200123456
        assert cap==original==44
        assert data[pos+28:pos+30]==bytes.fromhex('6188')
        assert b'RSSI=-48 dBm LQI=230' in data[pos:pos+length]
        packets+=1
    else:
        raise AssertionError(kind)
    pos+=length
assert interfaces==16 and packets==1
print('PASS: independent PCAPNG reader (16 channels, timestamps, NOFCS linktype, raw bytes, RSSI/LQI)')
