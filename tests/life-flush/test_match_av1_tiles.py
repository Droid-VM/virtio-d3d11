import unittest
from match_av1_tiles import match

def record(decoder='d',ctx='c',gen=0,pic=1,tile=0,size=3,digest='a'*64):
 return (f'[umd pid=1] AV1_INPUT_ID v=1 decoder={decoder} ctx={ctx} gen={gen} session=4 token=5 '
         f'pic={pic} qpc=99 type=1 show=1 oh=6 oh_bits=7 tile={tile} tiles=1 row=0 col=0 '
         f'bytes={size} sha256={digest} view=v resource=r mip=0 slice={pic}')

def index(entries):return {'entries':entries}
def src(i,size=3,digest='a'*64,packet=8):
 return {'coded_index':i,'bytes':size,'sha256':digest,'packet':packet,'packet_timestamp':packet*33,
         'packet_time_seconds':packet*.033,'order_hint':i,'show_frame':1}
class MatchTests(unittest.TestCase):
 def test_unique_match(self):
  r=match(index([src(2)]),record());self.assertEqual(r['unique'],1);self.assertEqual(r['records'][0]['source_matches'][0]['coded_index'],2)
 def test_repeated_identical_tile_is_ambiguous(self):
  r=match(index([src(2,packet=8),src(9,packet=30)]),record());self.assertEqual(r['ambiguous'],1);self.assertEqual(len(r['records'][0]['source_matches']),2)
 def test_no_match(self):
  r=match(index([src(2)]),record(size=4));self.assertEqual(r['unmatched'],1)
 def test_multiple_log_records_keep_decoder_generation_and_slice(self):
  logs='\n'.join([record(pic=1),record(pic=2,gen=1)])
  r=match(index([src(2)]),logs);self.assertEqual(r['input_tile_records'],2);self.assertEqual([x['generation'] for x in r['records']],[0,1]);self.assertEqual([x['slice'] for x in r['records']],[1,2])
 def test_ignores_non_identity_lines(self):
  self.assertEqual(match(index([src(2)]),'ordinary line')['input_tile_records'],0)
if __name__=='__main__':unittest.main()
