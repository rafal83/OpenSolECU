import pathlib,re,json
src=pathlib.Path('reference/ESP32-read-APS-inverters/AAA_DECODE.ino').read_text()
samples=re.findall(r'703000021300[0-9a-fA-F]+fefe',src,re.I)
out=pathlib.Path('tests/fixtures');out.mkdir(parents=True,exist_ok=True)
for i,s in enumerate(samples):
    b=bytes.fromhex(s); print(i,len(b),b[-4:].hex(),hex(sum(b[8:-4])&65535),hex(sum(b[9:-4])&65535))
    (out/f'ds3-{i}.hex').write_text(s+'\n')
