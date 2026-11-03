import unittest
from analyze_transactions import analyze

class TransactionTests(unittest.TestCase):
    def logs(self):
        p = '[umd pid=7 tid=9] LIFE_FLUSH '
        identity = ' runtime=a ctx=b seq=1 '
        return [p+'entry'+identity+'session=0 token=1 gen=0',
                p+'commit'+identity+'old_session=0 new_session=1 old_token=1 new_token=2 old_gen=0 new_gen=1',
                p+'resume'+identity+'gen=1']
    def test_zero_session_is_valid(self):
        self.assertEqual(analyze(self.logs())['verdict'], 'TRANSACTIONS_VALID')
    def test_no_transaction_is_uncovered(self):
        self.assertEqual(analyze(['ordinary key frame'])['verdict'], 'NOT_COVERED')
    def test_missing_resume_fails(self):
        self.assertEqual(analyze(self.logs()[:2])['verdict'], 'INVALID')
    def test_different_context_cannot_join(self):
        x=self.logs();x[2]=x[2].replace('ctx=b','ctx=c')
        self.assertEqual(analyze(x)['verdict'], 'INVALID')
    def test_different_process_cannot_join(self):
        x=self.logs();x[2]=x[2].replace('pid=7','pid=8')
        self.assertEqual(analyze(x)['verdict'], 'INVALID')
    def test_entry_token_must_match(self):
        x=self.logs();x[0]=x[0].replace('token=1','token=9')
        self.assertEqual(analyze(x)['verdict'], 'INVALID')
    def test_generation_must_advance_once(self):
        x=self.logs();x[1]=x[1].replace('new_gen=1','new_gen=2')
        self.assertEqual(analyze(x)['verdict'], 'INVALID')
    def test_zero_or_unchanged_token_fails(self):
        for token in ('0','1'):
            x=self.logs();x[1]=x[1].replace('new_token=2','new_token='+token)
            self.assertEqual(analyze(x)['verdict'], 'INVALID')
    def test_duplicate_commit_fails(self):
        x=self.logs();x.insert(2,x[1]);self.assertEqual(analyze(x)['verdict'],'INVALID')
    def test_wrong_order_fails(self):
        self.assertEqual(analyze(list(reversed(self.logs())))['verdict'],'INVALID')
    def test_fail_after_commit_fails(self):
        x=self.logs();x.append('[umd pid=7] LIFE_FLUSH fail runtime=a ctx=b seq=1 committed=1')
        self.assertEqual(analyze(x)['verdict'],'INVALID')

if __name__=='__main__':unittest.main()
