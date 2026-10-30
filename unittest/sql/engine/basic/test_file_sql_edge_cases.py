"""Lossless file SQL boundary tests against a running local seekdb."""
import os
from pathlib import Path
import tempfile
import unittest
import pymysql


class FileSqlEdges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'),
            password=os.environ.get('SEEKDB_PASSWORD', ''),
            database=os.environ.get('SEEKDB_DATABASE', 'test'),
            charset='utf8mb4', autocommit=True, read_timeout=30)
        with cls.connection.cursor() as cursor:
            cursor.execute('SELECT @@secure_file_priv')
            cls.root = Path(cursor.fetchone()[0])
            cursor.execute('SET SESSION enable_file_sql=ON')

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='file-sql-edge-', dir=self.root)
        self.path = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def q(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall() if cursor.description else cursor.rowcount

    def test_copy_preserves_embedded_cr_lf_crlf(self):
        for i, text in enumerate(('a\rb', 'a\nb', 'a\r\nb', 'a\r\r\nb')):
            with self.subTest(text=repr(text)):
                target = str(self.path / ('copy-%d.csv' % i))
                self.q('COPY (SELECT %s AS text) TO %s (HEADER)', (text, target))
                self.assertEqual(((text.encode().hex().upper(),),),
                                 self.q('SELECT HEX(text) FROM read_csv(%s)', (target,)))

    def test_csv_crlf_record_and_quoted_field_are_distinct(self):
        target = self.path / 'crlf.csv'
        target.write_bytes(b'id,text\r\n1,"a\r\nb"\r\n2,"c\rd"\r\n')
        self.assertEqual(((1, '610D0A62'), (2, '630D64')),
                         self.q('SELECT id,HEX(text) FROM read_csv(%s) ORDER BY id', (str(target),)))

    def test_json_overflowing_integers_remain_exact_strings(self):
        values = ('9223372036854775808', '-9223372036854775809',
                  '18446744073709551616', '9' * 400)
        target = self.path / 'integers.jsonl'
        target.write_text(''.join('{"v":' + v + '}\n' for v in values) + '{"v":"text"}\n')
        self.assertEqual(tuple((v,) for v in values) + (('text',),),
                         self.q('SELECT v FROM read_json(%s)', (str(target),)))
        self.assertEqual('VARCHAR', self.q('DESCRIBE SELECT * FROM read_json(%s)', (str(target),))[0][1])

    def test_json_bigint_boundaries_are_numeric(self):
        target = self.path / 'bigint.jsonl'
        target.write_text('{"v":-9223372036854775808}\n{"v":9223372036854775807}\n')
        self.assertEqual(((-9223372036854775808,), (9223372036854775807,)),
                         self.q('SELECT v FROM read_json(%s)', (str(target),)))

    def test_json_float_widening_and_numeric_strings(self):
        target = self.path / 'floats.jsonl'
        target.write_text('{"v":1}\n{"v":1.25e2}\n{"v":null}\n')
        self.assertEqual(((1.0,), (125.0,), (None,)),
                         self.q('SELECT v FROM read_json(%s)', (str(target),)))
        target = self.path / 'strings.jsonl'
        target.write_text('{"v":1.2500e2}\n{"v":"001"}\n')
        self.assertEqual((('1.2500e2',), ('001',)),
                         self.q('SELECT v FROM read_json(%s)', (str(target),)))

    def test_invalid_json_structures_still_rejected(self):
        cases = ('[]', '[1]', '{"v":[]}', '{"v":{}}', '{"v":1,"V":2}',
                 '{"v":1,}', '1', 'null', '{"v":NaN}', '{"v":01}')
        for i, content in enumerate(cases):
            with self.subTest(content=content):
                target = self.path / ('bad-%d.jsonl' % i)
                target.write_text(content + '\n')
                with self.assertRaises(pymysql.Error):
                    self.q('SELECT * FROM read_json(%s)', (str(target),))


if __name__ == '__main__':
    unittest.main(verbosity=2)
