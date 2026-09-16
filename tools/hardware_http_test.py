"""Bounded AP + HTTP/browser checks. Restores the PC's prior WLAN profile in finally.
No production radio command is sent: pairing is checked only on a locked passive build.
Run with Python having Playwright installed. Passwords stay in dist/private.
"""
import argparse, gzip, json, pathlib, re, struct, subprocess, sys, time
import urllib.request, urllib.error
from xml.sax.saxutils import escape

ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'dist/private'; OUT.mkdir(parents=True,exist_ok=True)
p=argparse.ArgumentParser();p.add_argument('--connected',action='store_true');p.add_argument('--restore',required=True);p.add_argument('--base',default='http://192.168.4.1');a=p.parse_args()
credentials=(OUT/'credentials.log').read_text(errors='replace')
ssid=re.findall(r'^SSID: (.+)',credentials,re.M)[-1].strip()
ap_password=re.findall(r'^AP password: (.+)',credentials,re.M)[-1].strip()

def wlan(*args):
    result=subprocess.run(['netsh','wlan',*args],capture_output=True,timeout=15)
    with (OUT/'wlan-test.log').open('ab') as f:f.write(result.stdout+result.stderr)
    return result.returncode

if not a.connected:
    profile=OUT/'opensolecu-test.xml'
    profile.write_text(f'''<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1"><name>{escape(ssid)}</name><SSIDConfig><SSID><name>{escape(ssid)}</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{escape(ap_password)}</keyMaterial></sharedKey></security></MSM></WLANProfile>''',encoding='utf-8')
    try:
        wlan('show','interfaces')
        assert wlan('add','profile',f'filename={profile}','user=current')==0
        assert wlan('connect',f'name={ssid}')==0
        result=subprocess.run([sys.executable,__file__,'--connected','--restore',a.restore,'--base',a.base],timeout=160)
    finally:
        wlan('connect',f'name={a.restore}')
        time.sleep(3)
        wlan('show','interfaces')
        wlan('delete','profile',f'name={ssid}')
        profile.unlink(missing_ok=True)
    sys.exit(result.returncode)

opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
checks=[]
def request(path,body=None,expected=200,raw=False):
    data=None if body is None else body if isinstance(body,bytes) else json.dumps(body).encode()
    headers={'Content-Type':'application/octet-stream' if raw else 'application/json'}
    req=urllib.request.Request(a.base+path,data=data,headers=headers)
    try:r=opener.open(req,timeout=8)
    except urllib.error.HTTPError as error:r=error
    content=r.read();status=r.status
    assert status==expected,(path,status,content[:120])
    checks.append(f'{path}: {status}')
    if r.headers.get('Content-Encoding')=='gzip':content=gzip.decompress(content)
    return content
def get(path):return json.loads(request(path))

for attempt in range(25):
    try:
        system=get('/api/system');break
    except (OSError,TimeoutError):time.sleep(1)
else:raise RuntimeError('AP unavailable after 25 attempts')
assert system['mode']=='SNIFFER' and system['radio']['txSubmissions']==0
assert system['wifi']['ap']['clients']>=1
config=get('/api/config')
assert not any(k in config for k in ['password','apPassword','adminHash','adminPassword'])
assert get('/api/live')['totalPower'] is None
for endpoint in ['/api/stats','/api/wifi','/api/debug','/api/status','/api/history?range=today','/api/history?range=7d','/api/history?range=30d','/api/history?range=12m']:
    get(endpoint)
request('/api/history?range=bad',expected=400)
request('/api/export.csv?from=5&to=1',expected=400)
assert request('/api/export.csv').startswith(b'\xef\xbb\xbf')
json.loads(request('/api/backup'))
assert config['authenticationRequired'] is False
request('/api/login',{},expected=404)
request('/api/config',{'mode':'NORMAL'},expected=400)
request('/api/pair',{},expected=409)
request('/api/sniffer/control',{'action':'channel','channel':27},expected=409)
request('/api/sniffer/control',{'action':'pause'})
time.sleep(.1)
capture=get('/api/sniffer');assert capture['paused']
(OUT/'http-sniffer.json').write_text(json.dumps(capture,indent=2))
pcap=request('/api/sniffer/export?format=pcapng');(OUT/'physical.pcapng').write_bytes(pcap)
rows=request('/api/sniffer/export?format=jsonl');(OUT/'physical.jsonl').write_bytes(rows)
records=[json.loads(line) for line in rows.splitlines() if line]
offset=0;frames=0;interfaces=0
while offset<len(pcap):
    kind,length=struct.unpack_from('<II',pcap,offset)
    assert length>=12 and length%4==0 and struct.unpack_from('<I',pcap,offset+length-4)[0]==length
    if kind==1:interfaces+=1;assert struct.unpack_from('<H',pcap,offset+8)[0]==230
    if kind==6:
        interface,hi,lo,cap,original=struct.unpack_from('<IIIII',pcap,offset+8)
        row=records[frames];assert cap==original==row['length']
        assert pcap[offset+28:offset+28+cap].hex().upper()==row['hex']
        assert interface+11==row['channel'];frames+=1
    offset+=length
assert interfaces==16 and frames==len(records) and frames>0
checks.append(f'PCAPNG independent verification: {frames} frames, bytes/channel identical to JSONL')
request('/api/ota',b'not an ESP image',expected=400,raw=True)
request('/api/sniffer/control',{'action':'resume'})
get('/api/wifi/scan');time.sleep(3);scan=get('/api/wifi/scan')
(OUT/'wifi-scan.json').write_text(json.dumps(scan,indent=2))
checks.append(f'WiFi scan: {len(scan["networks"])} networks')

from playwright.sync_api import sync_playwright
errors=[]
with sync_playwright() as pw:
    browser=pw.chromium.launch(headless=True)
    page=browser.new_page(viewport={'width':1440,'height':1000})
    page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(a.base+'/#dashboard',wait_until='domcontentloaded')
    page.wait_for_function("document.querySelector('#power').textContent==='—'")
    page.screenshot(path=str(OUT/'dashboard-desktop.png'),full_page=True)
    for section in ['statistics','settings','system','debug','sniffer']:
        page.locator(f'nav a[href="#{section}"]').click()
        page.wait_for_timeout(700)
        assert page.locator('#'+section).is_visible()
    page.wait_for_function("document.querySelectorAll('#frame-rows tr').length>0")
    page.locator('#frame-rows tr').first.click()
    assert 'HEX' in page.locator('#frame-detail').inner_text()
    page.locator('#close-frame').click()
    page.locator('#frame-filter').select_option('aps')
    assert all('APS ' in text for text in page.locator('#frame-rows tr').all_inner_texts())
    page.locator('#frame-filter').select_option('all')
    page.screenshot(path=str(OUT/'sniffer-desktop.png'),full_page=True)
    page.set_viewport_size({'width':390,'height':844})
    page.screenshot(path=str(OUT/'sniffer-mobile.png'),full_page=True)
    assert page.evaluate('document.documentElement.scrollWidth<=window.innerWidth'), 'Mobile horizontal overflow'
    assert page.locator('#login-button,#login-dialog').count()==0
    page.locator('#pause-capture').click();page.wait_for_timeout(1400)
    assert get('/api/sniffer')['paused']
    page.locator('#resume-capture').click();page.wait_for_timeout(1400)
    assert not get('/api/sniffer')['paused']
    browser.close()
assert not errors,errors
checks.append('Browser: all pages, live SSE frames, hex detail, filters, password-free controls, pause/resume, desktop/mobile')
final=get('/api/system');assert final['radio']['txSubmissions']==0
(OUT/'http-system-final.json').write_text(json.dumps(final,indent=2))
(OUT/'http-test-results.json').write_text(json.dumps(checks,indent=2))
print('PASS:',len(checks),'physical HTTP/browser checks; TX submissions = 0',flush=True)
