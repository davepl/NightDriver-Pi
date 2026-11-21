#!/usr/bin/env python3
"""Decode native test-pattern frames from a pcap (or stdin) into timing CSV.

Usage: python3 tools/colorlight_pattern_timing.py CONFIG CAPTURE.pcap OUTPUT.csv
For continuous capture, pipe tcpdump -w - into this tool with CAPTURE.pcap '-'.
"""
import csv
import hashlib
import json
import pathlib
import sys
from colorlight_capture_audit import packets

DIGITS = [[14,17,19,21,25,17,14],[4,12,4,4,4,4,14],[14,17,1,2,4,8,31],
          [30,1,1,14,1,1,30],[2,6,10,18,31,2,2],[31,16,16,30,1,1,30],
          [14,16,16,30,17,17,14],[31,1,2,4,8,8,8],[14,17,17,14,17,17,14],
          [14,17,17,15,1,1,14]]


def run(config, capture, target):
    width,height=config['width'],config['height']
    rows={};count=0;previous=None;refresh=0;last_pixel=None
    with open(target,'w') as output:
        writer=csv.writer(output)
        writer.writerow(['refresh','sync_epoch_ns','sync_interval_ms','frame_id','missed_host_ticks',
                         'pixel_to_sync_ms','wire_pixel_sha256'])
        output.flush()
        for time_ns,p in packets(capture):
            if p[12]==85:
                import struct
                y,x,n=struct.unpack_from('>HHH',p,13)
                if y>=height or x+n>width:raise ValueError('Pixel geometry mismatch')
                row=rows.setdefault(y,bytearray(width*3));row[x*3:(x+n)*3]=p[21:21+n*3]
                count+=n;last_pixel=time_ns
            elif p[12]==1:
                if len(rows)==height and count==width*height:
                    def number(x,scale,length):
                        value=''
                        for j in range(length):
                            glyph=[sum((max(rows[y*scale][(x+j*6*scale+c*scale)*3:(x+j*6*scale+c*scale)*3+3])>80)<<(4-c) for c in range(5)) for y in range(7)]
                            value+=str(DIGITS.index(glyph))
                        return int(value)
                    frame=number(15,2,8)
                    missed=number(width-56,1,4)
                    bits=sum((rows[1][(184+b*8)*3+1]>80)<<(15-b) for b in range(16))
                    assert bits==frame%65536,'Frame counter and binary code disagree'
                    writer.writerow([refresh,time_ns,(time_ns-previous)/1e6 if previous else '',frame,missed,
                                     (time_ns-last_pixel)/1e6,hashlib.sha256(b''.join(rows[y] for y in range(height))).hexdigest()])
                    if refresh%60==0:output.flush()
                    previous=time_ns;refresh+=1
                rows={};count=0


if __name__=='__main__':
    try:
        run(json.loads(pathlib.Path(sys.argv[1]).read_text()),*sys.argv[2:])
    except KeyboardInterrupt:
        pass
