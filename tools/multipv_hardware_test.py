"""Deploy an optional firmware, configure an explicit inventory, and verify the real UI.

No authentication on current firmware. OPENSOLECU_ADMIN_PASSWORD is only needed
to upgrade an older authenticated firmware. Captures/backup are archived before OTA.
No pairing or IEEE 802.15.4 transmit command is issued.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request

p=argparse.ArgumentParser()
p.add_argument('--base',required=True)
p.add_argument('--firmware',type=Path)
p.add_argument('--inventory',type=Path)
p.add_argument('--verify-config-write',action='store_true')
p.add_argument('--out',type=Path,default=Path('dist/private/multipv-hardware'))
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}));token='';checks=[]

def request(path,data=None,status=200):
    headers={}
    if token:headers['Authorization']='Bearer '+token
    if data is not None:
        if isinstance(data,bytes):headers['Content-Type']='application/octet-stream'
        else:headers['Content-Type']='application/json';data=json.dumps(data).encode()
    req=urllib.request.Request(a.base+path,data=data,headers=headers)
    try:r=opener.open(req,timeout=60)
    except urllib.error.HTTPError as e:r=e
    body=r.read()
    assert r.status==status,(path,r.status,body[:160])
    if r.headers.get('Content-Encoding')=='gzip':body=gzip.decompress(body)
    checks.append(f'{path}: {status}')
    return body

def get(path):return json.loads(request(path))
def login():
    global token
    token=json.loads(request('/api/login',dict(password=os.environ['OPENSOLECU_ADMIN_PASSWORD'])))['token']

def await_boot():
    time.sleep(3)
    for _ in range(30):
        try:
            system=get('/api/system')
            # app_main validates the OTA image after ten seconds. Rebooting earlier
            # would correctly trigger ESP-IDF rollback to the previous firmware.
            if 12<=system['uptimeSeconds']<120:return system
        except (OSError,TimeoutError):pass
        time.sleep(1)
    raise RuntimeError('Device did not return after reboot')

before=get('/api/system');assert before['mode']=='SNIFFER'
(a.out/'before.json').write_text(json.dumps(before,indent=2))
if a.firmware:
    (a.out/'backup-before.json').write_bytes(request('/api/backup'))
    (a.out/'frames-before.jsonl').write_bytes(request('/api/sniffer/export?format=jsonl'))
    (a.out/'capture-before.pcapng').write_bytes(request('/api/sniffer/export?format=pcapng'))
    if get('/api/config').get('authenticationRequired',True):login()
    firmware=a.firmware.read_bytes()
    print('OTA upload:',len(firmware),'bytes, sha256',hashlib.sha256(firmware).hexdigest(),flush=True)
    print(request('/api/ota',firmware).decode(),flush=True)
    await_boot();token=''

if a.inventory:
    inventory=json.loads(a.inventory.read_text(encoding='utf-8-sig'))
    print(request('/api/config',dict(inverters=inventory,channel=16)).decode(),flush=True)
    await_boot();token=''

config=get('/api/config');live=get('/api/live')
assert config['authenticationRequired'] is False and not token
assert config.get('normalModeAvailable') is True
request('/api/login',{},status=404)
if a.verify_config_write:
    request('/api/config',dict(installation=config['installation']))
    await_boot()
    assert get('/api/config')==config
assert len(config['inverters'])>=3
assert len(live['inverters'])>=len(config['inverters'])
assert live['pvCount']==sum(v['channelCount'] for v in live['inverters'])
assert set(v['serial'] for v in config['inverters'])<=set(v['serial'] for v in live['inverters'])
for v in live['inverters']:
    assert v['model'] in ('DS3','YC600','QS1') and v['configuredModel'] in ('AUTO','DS3','YC600','QS1')
    assert len(v['channels'])==v['channelCount'] in (2,4)
    if v['status']=='no_data':assert all(channel['power'] is None for channel in v['channels'])
# Rejected updates must leave NVS inventory intact and must not reboot the device.
for data in [dict(inverters=[config['inverters'][0]]*2),dict(inverters=[{}]*17),
             dict(inverters=[dict(serial='bad')]),dict(inverters=[dict(serial='703000021300',address=-1)]),
             dict(inverters=[dict(serial='703000021300',address=1.5)]),
             dict(inverters=[dict(serial='703000021300',model='INVALID')]),
             dict(inverters=[dict(serial='703000021300',name='x'*33)])]:
    request('/api/config',data,status=400)
assert get('/api/config')['inverters']==config['inverters']
for v in config['inverters']:
    for range_ in ['today','7d','30d','12m']:
        history=get('/api/history?range='+range_+'&serial='+v['serial'])
        assert all(r['serial']==v['serial'] for r in history['records'])
    assert get('/api/stats?serial='+v['serial'])['serial']==v['serial']
request('/api/stats?serial=bad',status=400)
request('/api/history?serial=bad',status=400)
assert request('/api/export.csv').splitlines()[0].endswith(b';serial')
get('/api/backup')
request('/api/wifi',dict(channel=27),status=400)
request('/api/pair',{},status=409) # Unavailable while the configured mode remains passive.
request('/api/ota',b'not an ESP32-C6 image',status=400)
# Keep the card listening continuously on the confirmed APsystems channel.
request('/api/sniffer/control',dict(action='channel',channel=16))

from playwright.sync_api import sync_playwright
errors=[]
with sync_playwright() as pw:
    browser=pw.chromium.launch()
    page=browser.new_page(viewport=dict(width=1440,height=1000))
    page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(a.base+'/#dashboard',wait_until='domcontentloaded')
    page.locator('[data-pv]').nth(5).wait_for()
    assert page.locator('#login-dialog,#login-button,[name=adminPassword]').count()==0
    assert page.locator('[data-pv]').count()>=6
    page.screenshot(path=str(a.out/'desktop.png'),full_page=True)
    page.set_viewport_size(dict(width=390,height=844))
    assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
    page.screenshot(path=str(a.out/'mobile.png'),full_page=True)
    page.select_option('#history-inverter',config['inverters'][1]['serial'])
    page.wait_for_timeout(6000) # Receive a real SSE update and retain the chosen history.
    assert page.locator('#history-inverter').input_value()==config['inverters'][1]['serial']
    page.goto(a.base+'/#settings',wait_until='domcontentloaded')
    page.locator('.inventory-row').nth(2).wait_for()
    assert page.locator('.inventory-row').count()==len(config['inverters'])
    browser.close()
assert not errors,errors
after=get('/api/system')
assert after['mode']=='SNIFFER' and after['radio']['txSubmissions']==0
assert after['uptimeSeconds']>=before['uptimeSeconds'] if not (a.firmware or a.inventory) else after['uptimeSeconds']>=10
(a.out/'after.json').write_text(json.dumps(after,indent=2))
(a.out/'checks.json').write_text(json.dumps(checks,indent=2))
print('PASS:',len(checks),'HTTP checks; real desktop/mobile UI, inventory, per-inverter history, SSE, TX=0;',after['freeHeap'],'bytes free')
print(json.dumps([dict(serial=v['serial'],status=v['status'],messages=v['messages']) for v in after['live']['inverters']]))
