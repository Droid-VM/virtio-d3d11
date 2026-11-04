"""Match traced input tile identities to coded single-tile source index entries."""
import argparse
import json
import re
from pathlib import Path

def match(index, lines):
    lookup = {}
    for entry in index['entries']:
        lookup.setdefault((entry['bytes'], entry['sha256']), []).append(entry)
    outputs=[]
    if isinstance(lines,str): lines=lines.splitlines()
    pattern=re.compile(
        r'AV1_INPUT_ID v=1 decoder=(\S+) ctx=(\S+) gen=(\d+) session=(\d+) token=(\d+) '
        r'pic=(\d+) qpc=(-?\d+) type=(\d+) show=(\d+) oh=(\d+) oh_bits=(\d+) '
        r'tile=(\d+) tiles=(\d+) row=(\d+) col=(\d+) bytes=(\d+) sha256=([0-9a-f]{64}) '
        r'view=(\S+) resource=(\S+) mip=(\d+) slice=(\d+)')
    for line_no,line in enumerate(lines,1):
        if isinstance(line,bytes): line=line.decode('utf-8',errors='replace')
        m=pattern.search(line)
        if not m: continue
        g=m.groups(); size=int(g[15]); digest=g[16]
        candidates=lookup.get((size,digest),[])
        outputs.append({'log_line':line_no,'decoder':g[0],'ctx':g[1],
            'generation':int(g[2]),'session':int(g[3]),'token':int(g[4]),
            'picture':int(g[5]),'qpc':int(g[6]),'frame_type':int(g[7]),
            'show_frame':int(g[8]),'order_hint':int(g[9]),'order_hint_bits':int(g[10]),
            'tile':int(g[11]),'tile_count':int(g[12]),'row':int(g[13]),'col':int(g[14]),
            'bytes':size,'sha256':digest,'view':g[17],'resource':g[18],
            'mip':int(g[19]),'slice':int(g[20]),
            'source_matches':[{'coded_index':x['coded_index'],'packet':x['packet'],
                'packet_timestamp':x['packet_timestamp'],'packet_time_seconds':x['packet_time_seconds'],
                'order_hint':x['order_hint'],'show_frame':x['show_frame']} for x in candidates],
            'match_status':'NO_MATCH' if not candidates else ('UNIQUE_TILE' if len(candidates)==1 else 'AMBIGUOUS_DUPLICATE_TILE')})
    return {'schema':1,'input_tile_records':len(outputs),'unique':sum(x['match_status']=='UNIQUE_TILE' for x in outputs),
        'ambiguous':sum(x['match_status']=='AMBIGUOUS_DUPLICATE_TILE' for x in outputs),
        'unmatched':sum(x['match_status']=='NO_MATCH' for x in outputs),'records':outputs,
        'scope':'Exact tile bytes identify source coded-tile candidates only; no per-frame display PTS, seek causality, host acceptance, or target-pixel proof.'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('index',type=Path);p.add_argument('umd_log',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    with a.umd_log.open('r',encoding='utf-8',errors='replace') as f:
        result=match(json.loads(a.index.read_text()),f)
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(f"MATCHED records={result['input_tile_records']} unique={result['unique']} ambiguous={result['ambiguous']} unmatched={result['unmatched']}")
if __name__=='__main__':main()
