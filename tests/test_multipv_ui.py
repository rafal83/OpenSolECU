"""Browser regression for the multi-inverter dashboard. No device/network mutation."""
import json
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
serials = ['703000021300', '408000158215', '801000085070']
models = ['DS3', 'YC600', 'QS1']
inventory = [dict(serial=s, name=f'Onduleur {i+1}', address=100+i, model=models[i]) for i, s in enumerate(serials)]
config = dict(installation='Test', ssid='Test', apSsid='OpenSolECU-Test', apEnabled=True,
              timezone='Europe/Paris', ecu='00124B000001', pan=4660, channel=16,
              pollSeconds=5, logLevel=2, mode='SNIFFER', normalModeAvailable=True,
              maxInverters=16, inverters=inventory)
rows = [dict(**v, configured=True, status='measured' if i == 0 else 'no_data',
             online=i == 0, simulated=False, last_seen=1704067200 if i == 0 else 0,
             totalPower=300 if i == 0 else None, todayWh=50 if i == 0 else None,
             channelCount=4 if v['model']=='QS1' else 2,
             channels=[dict(power=(100*(j+1)) if i == 0 else None,
                            voltage=32+j if i == 0 else None, current=None)
                       for j in range(4 if v['model']=='QS1' else 2)],
             pv1=dict(power=100 if i == 0 else None, voltage=32 if i == 0 else None, current=None),
             pv2=dict(power=200 if i == 0 else None, voltage=33 if i == 0 else None, current=None))
        for i, v in enumerate(inventory)]
live = dict(inverters=rows, totalPower=None, measuredPower=300, todayWh=None, knownPowerCount=1, pvCount=8)
requests, errors, saved, uploads = [], [], [], []

with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page(viewport=dict(width=1440, height=1000))
    page.on('pageerror', lambda e: errors.append(str(e)))

    def route(r):
        url = urlsplit(r.request.url); requests.append(url.path+'?'+url.query)
        assert 'authorization' not in r.request.headers
        if url.path.startswith('/api/'):
            data = {}
            if url.path == '/api/config':
                if r.request.method == 'POST':
                    saved.append(r.request.post_data_json); data = dict(message='Saved')
                else: data = config
            elif url.path == '/api/live': data = live
            elif url.path == '/api/history': data = dict(records=[])
            elif url.path == '/api/ota':
                uploads.append(r.request.post_data_buffer); data = dict(message='OTA received')
            elif url.path == '/api/stats': data = dict.fromkeys(['todayWh','yesterdayWh','weekWh','monthWh','yearWh','totalWh','bestDayWh','dailyAverageWh','peakToday','peakTime'])
            elif url.path == '/api/events':
                r.fulfill(status=200, content_type='text/event-stream', body=': test\n\n'); return
            r.fulfill(status=200, json=data); return
        file = ROOT/'webui'/({'/':'index.html'}.get(url.path, url.path.lstrip('/')))
        mime = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8'}.get(file.suffix,'text/plain')
        r.fulfill(status=200 if file.exists() else 404, content_type=mime, body=file.read_bytes() if file.exists() else b'')

    page.route('**/*', route)
    page.goto('http://opensolecu.test/')
    page.locator('.inverter').nth(2).wait_for()
    assert page.locator('#login-dialog,#login-button,[name=adminPassword]').count()==0
    assert page.locator('[data-pv]').count() == 8
    assert page.locator('[data-pv] strong').all_text_contents() == ['100 W','200 W','— W','— W','— W','— W','— W','— W']
    assert 'PARTIELLE' in page.locator('#power-label').inner_text()
    assert page.locator('#power').inner_text() == '300'
    assert page.locator('#today-energy').inner_text() == '—'
    page.select_option('#history-inverter', serials[1])
    page.wait_for_function("document.querySelector('#history-inverter').value === '408000158215'")
    page.wait_for_timeout(100)
    assert any('serial='+serials[1] in r and '/api/history' in r for r in requests)
    page.evaluate('(data)=>live(data)',live)
    assert page.locator('#history-inverter').input_value() == serials[1]
    # Names are rendered as text, never interpreted as HTML.
    xss = json.loads(json.dumps(live));xss['inverters'][0]['name']='<img src=x onerror=alert(1)>'
    page.evaluate('(data)=>live(data)',xss)
    assert page.locator('.inverter img').count() == 0
    page.evaluate('(data)=>live(data)',live)
    output = ROOT/'dist/private/multipv';output.mkdir(parents=True,exist_ok=True)
    page.screenshot(path=str(output/'desktop.png'),full_page=True)
    page.set_viewport_size(dict(width=390,height=844))
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.screenshot(path=str(output/'mobile.png'),full_page=True)
    page.goto('http://opensolecu.test/#settings')
    page.locator('.inventory-row').nth(2).wait_for()
    assert page.locator('[name=mode] option[value=NORMAL]').is_enabled()
    page.click('#add-inverter')
    assert page.locator('.inventory-row').count() == 4
    assert '10 PV' in page.locator('#inventory-count').inner_text()
    last=page.locator('.inventory-row').nth(3)
    last.locator('[data-field=serial]').fill(serials[0])
    page.click('.save')
    assert 'unique' in page.locator('#notice').inner_text()
    assert not saved
    last.locator('[data-field=serial]').fill('703000021303')
    page.click('.save')
    page.wait_for_timeout(100)
    assert len(saved)==1 and len(saved[0]['inverters'])==4
    assert 'serial' not in saved[0] and 'inverterId' not in saved[0]
    last.locator('button').click()
    assert page.locator('.inventory-row').count()==3
    for _ in range(13): page.click('#add-inverter')
    assert page.locator('#add-inverter').is_disabled()
    assert '34 PV' in page.locator('#inventory-count').inner_text()
    page.set_input_files('#ota-form input[type=file]',dict(name='opensolecu.bin',mimeType='application/octet-stream',buffer=b'test firmware'))
    page.locator('#ota-form button').click()
    page.wait_for_timeout(100)
    assert uploads==[b'test firmware']
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.evaluate('live({inverters:[],knownPowerCount:0})')
    assert page.locator('[data-pv]').count()==0
    assert not page.locator('#inverter-empty').get_attribute('hidden')
    assert not errors, errors
    assert not any('/api/login' in r for r in requests)
    browser.close()
print('PASS: DS3/YC600/QS1 dynamic PV, unknown values, partial total, history selection, UTF-8, escaped names, mobile, inventory save/duplicates/limit/empty')
