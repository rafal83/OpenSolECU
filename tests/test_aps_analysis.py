"""Offline fragmentation regressions using the public DS3 sample, not site identifiers."""
import copy
import pathlib
import sys
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from analyze_aps import analyze,packet,preview


class FragmentTests(unittest.TestCase):
    def setUp(self):
        payload=bytes.fromhex(pathlib.Path('tests/fixtures/ds3-0.hex').read_text())
        self.payload=payload
        # Short-address MAC, NWK source 5471, destination ECU 0000; APS counter 13.
        prefix=bytes.fromhex('618801d8a3000071540800000071540f01c0140601050f140d')
        self.rows=[dict(id=i+1,monotonicUs=1000000+i*10000,channel=16,srcPan=0xa3d8,
                        hex=(prefix+ext+data).hex()) for i,(ext,data) in enumerate([
                            (b'\x01\x02',payload[:97]),(b'\x02\x01',payload[97:])])]

    def test_complete_and_crc(self):
        g=analyze(self.rows)['fragmentGroups'][0]
        self.assertTrue(g['complete']);self.assertTrue(g['preview']['checksumValid'])
        self.assertEqual(g['preview']['serial'],'703000021300')
        self.assertAlmostEqual(g['preview']['acFrequency'],50.02)

    def test_missing_tail_is_never_validated(self):
        g=analyze(self.rows[:1])['fragmentGroups'][0]
        self.assertFalse(g['complete']);self.assertFalse(g['preview']['checksumValid'])
        self.assertEqual(g['preview']['receivedBytes'],97)

    def test_out_of_order_and_relay_duplicates(self):
        relay=copy.deepcopy(self.rows[0]);raw=bytearray.fromhex(relay['hex']);raw[5:9]=b'\x11\x22\x33\x44';relay['hex']=raw.hex()
        g=analyze([self.rows[1],self.rows[0],relay])['fragmentGroups'][0]
        self.assertTrue(g['complete']);self.assertTrue(g['preview']['checksumValid'])

    def test_conflict_rejected(self):
        bad=copy.deepcopy(self.rows[0]);raw=bytearray.fromhex(bad['hex']);raw[-1]^=1;bad['hex']=raw.hex()
        g=analyze([self.rows[0],bad,self.rows[1]])['fragmentGroups'][0]
        self.assertTrue(g['conflict']);self.assertFalse(g['complete'])

    def test_timeout_and_counter_isolation(self):
        delayed=copy.deepcopy(self.rows[1]);delayed['monotonicUs']+=11000000
        self.assertTrue(all(not g['complete'] for g in analyze([self.rows[0],delayed])['fragmentGroups']))
        other=copy.deepcopy(self.rows[1]);raw=bytearray.fromhex(other['hex']);raw[24]=14;other['hex']=raw.hex()
        self.assertTrue(all(not g['complete'] for g in analyze([self.rows[0],other])['fragmentGroups']))

    def test_all_truncations_and_aps_ack(self):
        for row in self.rows:
            for n in range(len(row['hex'])//2):
                short={**row,'hex':row['hex'][:n*2]};packet(short)
        ack={**self.rows[0],'hex':self.rows[0]['hex'][:34]+'82140601050f140d0100ff'}
        result=analyze([ack]);self.assertEqual(result['apsAcks'],1);self.assertEqual(result['fragmentGroups'],[])
        bad=bytearray(self.payload);bad[34]^=1
        self.assertFalse(preview(bad)['checksumValid'])


if __name__=='__main__':unittest.main()
