"""Read-only capture archival. Polls JSONL export without changing sniffer settings."""
import argparse,json,pathlib,time,urllib.request
p=argparse.ArgumentParser();p.add_argument('--base',default='http://opensolecu.local');p.add_argument('--seconds',type=float,default=170);p.add_argument('--out',type=pathlib.Path,required=True);a=p.parse_args()
op=urllib.request.build_opener(urllib.request.ProxyHandler({}));a.out.parent.mkdir(parents=True,exist_ok=True)
seen=set();start=time.monotonic();attempt=0
with a.out.open('w',encoding='utf-8') as out:
    while time.monotonic()-start<a.seconds:
        try:
            with op.open(a.base+'/api/sniffer/export?format=jsonl',timeout=8) as r:
                for line in r:
                    f=json.loads(line);key=(f['id'],f['monotonicUs'],f['hex'])
                    if key not in seen:seen.add(key);out.write(json.dumps(f)+'\n')
            out.flush()
        except (OSError,ValueError) as error:print(type(error).__name__,str(error),flush=True)
        attempt+=1
        if attempt%5==0:print('Archived',len(seen),'unique frames;',round(time.monotonic()-start),'s',flush=True)
        time.sleep(3)
print('Saved',len(seen),'frames:',a.out,flush=True)
