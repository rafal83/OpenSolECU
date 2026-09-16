"""Time-limited passive capture, serial control, sparse HTTP archival, offline report.

Requires pyserial (available in the ESP-IDF Python environment). The job pauses
the sniffer in finally and reports errors/gaps instead of claiming a full capture.
No association, transmit command, reboot or firmware write is performed.
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
import pathlib
import subprocess
import sys
import time
import urllib.request
import serial
from analyze_aps import analyze
from jsonl_to_pcapng import convert


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='COM3')
    p.add_argument('--base',default='http://opensolecu.local')
    p.add_argument('--minutes',type=float,default=30)
    p.add_argument('--interval',type=float,default=30)
    p.add_argument('--out',type=pathlib.Path,required=True)
    a=p.parse_args()
    if not 0<a.minutes<=120 or not 5<=a.interval<=60:p.error('Duration 0..120 min; export interval 5..60 s')
    a.out.mkdir(parents=True,exist_ok=True)
    if (a.out/'frames.jsonl').exists():p.error('Output already contains a capture; choose a new directory')
    op=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.1
    s.dtr=True;s.rts=True;s.open();s.rts=False;s.dtr=s.dtr;s.dtr=False
    pending=b'';started=False;seen=set();gaps=[];http_errors=[];last_id=None
    status={'state':'initializing','port':a.port,'channel':16,'frames':0,'httpErrors':http_errors,'archiveGaps':gaps}

    def progress():
        status['frames']=len(seen);status['updatedUtc']=datetime.now(timezone.utc).isoformat()
        temporary=a.out/'status.tmp';temporary.write_text(json.dumps(status,indent=2),encoding='utf-8')
        temporary.replace(a.out/'status.json')

    with (a.out/'serial.log').open('wb') as log, (a.out/'frames.jsonl').open('w',encoding='utf-8') as output:
        def read_lines():
            nonlocal pending
            data=s.read(s.in_waiting or 1)
            if data:log.write(data);log.flush();pending+=data
            result=[]
            while b'\n' in pending:
                line,pending=pending.split(b'\n',1)
                result.append(line.decode('utf-8','replace').rstrip('\r'))
            return result

        def command(text,kind='json',timeout=8):
            s.write((text+'\n').encode())
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                for line in read_lines():
                    if kind=='json' and line.startswith('JSON:'):return json.loads(line[5:])
                    if kind=='control' and line.startswith('RESULT:sniffer '):
                        if line.endswith('accepted'):return True
                        raise RuntimeError('Sniffer rejected the capture command')
            raise TimeoutError('No serial reply for '+text.split()[0])

        def archive():
            nonlocal last_id
            try:
                with op.open(a.base+'/api/sniffer/export?format=jsonl',timeout=10) as response:
                    for line in response:
                        row=json.loads(line);key=(row['id'],row['monotonicUs'],row['hex'])
                        if key in seen:continue
                        if last_id is not None and row['id']>last_id+1:gaps.append([last_id+1,row['id']-1])
                        seen.add(key);last_id=row['id'] if last_id is None else max(last_id,row['id'])
                        output.write(json.dumps(row)+'\n')
                output.flush()
            except (OSError,ValueError) as exc:
                http_errors.append({'utc':datetime.now(timezone.utc).isoformat(),'error':str(exc)})
                print('Archive warning:',type(exc).__name__,flush=True)

        try:
            status['before']=command('status')
            state=command('sniffer')
            if not state.get('active') or not state.get('rxOnly'):raise RuntimeError('Passive sniffer required')
            # Preserve the old RAM buffer before starting an independent capture.
            old=op.open(a.base+'/api/sniffer/export?format=jsonl',timeout=10).read()
            (a.out/'previous-buffer.jsonl').write_bytes(old)
            command('sniffer {"action":"capture","channel":16}',kind='control');started=True
            begin=time.monotonic();end=begin+a.minutes*60;next_export=begin+a.interval
            now=datetime.now(timezone.utc)
            status.update(state='capturing',startedUtc=now.isoformat(),expectedEndUtc=(now+timedelta(minutes=a.minutes)).isoformat())
            progress();print('Capturing passively for',a.minutes,'minutes; expected end',status['expectedEndUtc'],flush=True)
            while time.monotonic()<end:
                read_lines()
                if time.monotonic()>=next_export:
                    archive();next_export=time.monotonic()+a.interval
                    status['elapsedSeconds']=round(time.monotonic()-begin);progress()
                    print('Captured',len(seen),'frames;',status['elapsedSeconds'],'s; HTTP errors',len(http_errors),flush=True)
            status['state']='stopping'
        except BaseException as exc:
            status['state']='failed';status['error']=repr(exc)
            raise
        finally:
            if started:
                try:
                    command('sniffer {"action":"pause"}',kind='control')
                    status['finalSniffer']=command('sniffer')
                    status['after']=command('status')
                    archive()
                    if status['state']!='failed':status['state']='captured'
                except Exception as exc:
                    status['state']='failed';status['cleanupError']=repr(exc)
            s.close();progress()
    if status['state']=='captured':
        rows=[json.loads(line) for line in (a.out/'frames.jsonl').read_text().splitlines()]
        result=analyze(rows)
        (a.out/'aps.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        convert(a.out/'frames.jsonl',a.out/'capture.pcapng')
        report=pathlib.Path(__file__).with_name('analyze_capture.py')
        done=subprocess.run([sys.executable,str(report),str(a.out/'capture.pcapng'),'--out',str(a.out/'analysis')],timeout=120)
        status['state']='complete' if done.returncode==0 else 'captured_report_failed'
        status['apsFrames']=result['apsFrames'];status['fragmentGroups']=len(result['fragmentGroups'])
        status['completeMessages']=sum(g['complete'] for g in result['fragmentGroups'])
        status['checksumValidMessages']=sum(g['complete'] and bool((g['preview'] or {}).get('checksumValid')) for g in result['fragmentGroups'])
        progress();print(json.dumps({k:v for k,v in status.items() if k not in ('before','after','finalSniffer')}),flush=True)


if __name__=='__main__':main()
