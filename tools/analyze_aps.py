"""Offline APsystems analysis of OpenSolECU JSONL, with bounded APS fragment assembly.
Only classic MAC v0/1 and unencrypted Zigbee APS unicast/broadcast are supported.
Incomplete messages produce explicitly unvalidated previews, never production records.
"""
import argparse, collections, json, pathlib


def packet(row):
    b=bytes.fromhex(row['hex'])
    if len(b)<3:return None
    fc=int.from_bytes(b[:2],'little');src=fc>>14;dst=(fc>>10)&3
    if fc&7!=1 or fc&8 or (fc>>12)&3>1 or src not in (0,2,3) or dst not in (0,2,3):return None
    k=3
    if dst:k+=2+(2 if dst==2 else 8)
    if src:k+=(0 if fc&0x40 else 2)+(2 if src==2 else 8)
    if k+8>len(b):return None
    nwk=int.from_bytes(b[k:k+2],'little')
    if nwk&3 or (nwk>>2)&15!=2 or nwk&0x200:return None
    target=int.from_bytes(b[k+2:k+4],'little');origin=int.from_bytes(b[k+4:k+6],'little');k+=8
    if nwk&0x800:k+=8
    if nwk&0x1000:k+=8
    if nwk&0x400:
        if k+2>len(b):return None
        k+=2+2*b[k]
    if k+8>len(b):return None
    aps=b[k];kind=aps&3
    if kind not in (0,2) or aps&0x20 or aps&0x10 or aps&0x0c not in (0,8):return None
    ep,cluster,profile,sep,counter=b[k+1],int.from_bytes(b[k+2:k+4],'little'),int.from_bytes(b[k+4:k+6],'little'),b[k+6],b[k+7]
    if (profile,ep,sep)!=(0x0f05,0x14,0x14):return None
    k+=8;frag=block=0;ack=None
    if aps&0x80:
        if k>=len(b):return None
        frag=b[k]&3;k+=1
        if frag==3:return None
        if frag:
            if k>=len(b):return None
            block=b[k];k+=1
            if kind==2:
                if k>=len(b):return None
                ack=b[k];k+=1
    return dict(id=row['id'],us=row['monotonicUs'],channel=row['channel'],pan=row.get('srcPan'),
                origin=origin,target=target,cluster=cluster,counter=counter,kind=kind,
                fragment=frag,block=block,ack=ack,payload=b[k:])


def preview(data):
    if len(data)<58 or data[6:11]!=bytes.fromhex('fbfb5cbbbb'):return None
    be=lambda offset,size=2:int.from_bytes(data[offset:offset+size],'big')
    valid=len(data)==105 and data[-2:]==b'\xfe\xfe' and sum(data[8:-4])==be(len(data)-4)
    return dict(serial=data[:6].hex().upper(),receivedBytes=len(data),checksumValid=valid,
                warning=None if valid else 'PARTIAL OR INVALID: reference-scaled fields, not validated measurements',
                pv1Voltage=be(26)/48,pv2Voltage=be(28)/48,pv1Current=be(30)*.0125,
                pv2Current=be(32)*.0125,acVoltage=be(34)/3.8,acFrequency=be(36)/100,
                inverterSeconds=be(38),temperature=be(48)*.0198-23.84,
                energyCounter1=be(50,4),energyCounter2=be(54,4))


def analyze(rows):
    packets=[v for row in rows if (v:=packet(row)) is not None]
    groups=[];active={};unfragmented=[];polls=[]
    for v in packets:
        if v['kind']!=0:continue
        data=v['payload']
        if v['cluster']==6 and len(data)==19 and data[6:10]==bytes.fromhex('fbfb06bb'):
            polls.append(dict(id=v['id'],origin=v['origin'],target=v['target'],ecu=data[:6][::-1].hex().upper(),us=v['us']))
        if not v['fragment']:
            if (decoded:=preview(data)):unfragmented.append(dict(id=v['id'],**decoded))
            continue
        key=(v['channel'],v['pan'],v['origin'],v['target'],v['cluster'],v['counter'])
        g=active.get(key)
        if not g or v['us']-g['lastUs']>10000000:
            g=dict(channel=v['channel'],pan=v['pan'],origin=v['origin'],target=v['target'],cluster=v['cluster'],
                   apsCounter=v['counter'],expectedBlocks=None,blocks={},frameIds=[],lastUs=v['us'],conflict=False)
            active[key]=g;groups.append(g)
        g['lastUs']=v['us'];g['frameIds'].append(v['id'])
        index=0 if v['fragment']==1 else v['block']
        if v['fragment']==1:
            if not 1<=v['block']<=8:g['conflict']=True;continue
            if g['expectedBlocks'] not in (None,v['block']):g['conflict']=True
            g['expectedBlocks']=v['block']
        if index>=8:g['conflict']=True;continue
        if index in g['blocks'] and g['blocks'][index]!=data:g['conflict']=True
        g['blocks'][index]=data
    for g in groups:
        n=g['expectedBlocks'];g['complete']=bool(n and not g['conflict'] and set(g['blocks'])==set(range(n)))
        data=b''.join(g['blocks'][i] for i in range(n)) if g['complete'] else g['blocks'].get(0,b'')
        g['preview']=preview(data);g['blocks']={str(i):b.hex().upper() for i,b in g['blocks'].items()}
    return dict(frames=len(rows),apsFrames=len(packets),apsAcks=sum(v['kind']==2 for v in packets),polls=polls,
                fragmentGroups=groups,unfragmentedMeasurements=unfragmented)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=pathlib.Path);p.add_argument('--out',type=pathlib.Path,required=True);a=p.parse_args()
    rows=[json.loads(line) for line in a.capture.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    result=analyze(rows);a.out.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('Frames:',result['frames'],'APS:',result['apsFrames'],'polls:',len(result['polls']),'fragment groups:',len(result['fragmentGroups']))
    for g in result['fragmentGroups']:
        print('Source',hex(g['origin']),'APS',g['apsCounter'],'blocks',','.join(g['blocks']),'/',g['expectedBlocks'],'complete',g['complete'],'serial',(g['preview'] or {}).get('serial'))
