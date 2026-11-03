"""Analyze raw LIFE_FLUSH logs. Transaction evidence is not codec/pixel acceptance."""
import argparse
import json
import re
from pathlib import Path

EVENT = re.compile(r'LIFE_FLUSH (entry|commit|resume|fail)\b')
FIELD = re.compile(r'\b([a-z_]+)=([^\s,]+)')
PID = re.compile(r'\[umd pid=(\d+)')

def analyze(lines):
    transactions = {}
    issues = []
    events = 0
    for number, line in enumerate(lines, 1):
        match = EVENT.search(line)
        if not match:
            continue
        events += 1
        fields = dict(FIELD.findall(line[match.end():]))
        pid = PID.search(line)
        if not pid or not all(k in fields for k in ('runtime', 'ctx', 'seq')):
            issues.append({'line': number, 'error': 'missing process/context/sequence identity'})
            continue
        key = (pid[1], fields['runtime'], fields['ctx'], fields['seq'])
        tx = transactions.setdefault(key, {'pid': pid[1], 'runtime': fields['runtime'],
            'ctx': fields['ctx'], 'seq': fields['seq'], 'events': {}, 'errors': []})
        event = match[1]
        if event in tx['events']:
            tx['errors'].append('duplicate ' + event)
        else:
            tx['events'][event] = {'line': number, 'fields': fields}
    for tx in transactions.values():
        data = tx['events']
        if 'fail' in data:
            tx['errors'].append('driver reported transaction failure')
        if not all(k in data for k in ('entry', 'commit', 'resume')):
            tx['errors'].append('incomplete entry/commit/resume')
        else:
            entry, commit, resume = (data[k] for k in ('entry', 'commit', 'resume'))
            if not entry['line'] < commit['line'] < resume['line']:
                tx['errors'].append('events out of order')
            e, c, r = (x['fields'] for x in (entry, commit, resume))
            try:
                for old, before in [('session', 'old_session'), ('token', 'old_token'), ('gen', 'old_gen')]:
                    if int(e[old]) != int(c[before]):
                        tx['errors'].append('entry/commit mismatch: ' + old)
                if int(c['new_token']) == 0 or int(c['new_token']) == int(c['old_token']):
                    tx['errors'].append('token did not change to nonzero value')
                if int(c['new_gen']) != int(c['old_gen']) + 1:
                    tx['errors'].append('generation did not advance by one')
                if int(r['gen']) != int(c['new_gen']):
                    tx['errors'].append('resume generation mismatch')
            except (KeyError, ValueError):
                tx['errors'].append('missing or invalid numeric fields')
        tx['transactionEvidence'] = 'VALID' if not tx['errors'] else 'INVALID'
    verdict = ('NOT_COVERED' if not events else
        'INVALID' if issues or any(t['errors'] for t in transactions.values()) else 'TRANSACTIONS_VALID')
    return {'verdict': verdict, 'transactionCount': len(transactions), 'issues': issues,
        'transactions': list(transactions.values()), 'scope':
        'Log transaction ordering and scalar identity only. Codec, module identity, pixels, seek causality, and late-completion isolation require separate evidence.'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    with args.log.open(encoding='utf-8', errors='replace') as f:
        result = analyze(f)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    print(text)
    return 0 if result['verdict'] == 'TRANSACTIONS_VALID' else 2

if __name__ == '__main__':
    raise SystemExit(main())
