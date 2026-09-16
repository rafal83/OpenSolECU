"""Deterministically gzip local UI assets into an ESP-IDF C++ header."""
import gzip,pathlib,sys
root=pathlib.Path(sys.argv[1]);output=pathlib.Path(sys.argv[2])
lines=['#pragma once','#include <cstdint>']
for name,symbol in [('index.html','html'),('app.js','js'),('app.css','css')]:
    data=gzip.compress((root/name).read_bytes(),compresslevel=9,mtime=0)
    lines.append(f'static const uint8_t asset_{symbol}[] = {{'+','.join(str(v) for v in data)+'};')
output.write_text('\n'.join(lines)+'\n',encoding='utf-8')
