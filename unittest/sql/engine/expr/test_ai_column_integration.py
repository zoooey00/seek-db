#!/usr/bin/env python3
"""Native AI COLUMN regression coverage. Requires pymysql and a running seekdb.

Run with SEEKDB_PORT=2881 python3 test_ai_column_integration.py. The fixtures use
unique databases/models and an ephemeral local HTTP server; no model key needed.
"""
import json
import os
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class AIColumnIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.events = []
        cls.lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                messages = body.get('messages', [])
                text = next((m['content'] for m in messages if m['role'] == 'user'), '')
                system = next((m['content'] for m in messages if m['role'] == 'system'), '')
                with cls.lock:
                    cls.events.append(text)
                if 'slow-old' in text:
                    time.sleep(0.8)
                if 'BAD_JSON' in text:
                    value = 'this is not JSON'
                elif 'base-10 integer' in system:
                    value = '999' if 'BIG_INT' in text else '7'
                elif 'valid JSON value' in system:
                    value = json.dumps({'input': text})
                else:
                    value = 'reply:' + text
                payload = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': value}}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.connection = pymysql.connect(host=os.environ.get('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.environ.get('SEEKDB_PORT', '2881')), user=os.environ.get('SEEKDB_USER', 'root'),
            password=os.environ.get('SEEKDB_PASSWORD', ''), charset='utf8mb4', autocommit=True, read_timeout=30)
        cls.model = 'aic_' + uuid.uuid4().hex[:12]
        cls.endpoint = cls.model + '_ep'
        with cls.connection.cursor() as cur:
            cur.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)', (cls.model, json.dumps({'type': 'completion', 'model_name': 'regression'})))
            cur.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', (cls.endpoint, json.dumps({
                'ai_model_name': cls.model, 'url': 'http://127.0.0.1:%d/v1/chat/completions' % cls.server.server_port,
                'access_key': 'local-test-only', 'provider': 'openai', 'request_model_name': 'regression'})))

    @classmethod
    def tearDownClass(cls):
        with cls.connection.cursor() as cur:
            cur.execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (cls.endpoint,))
            cur.execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (cls.model,))
        cls.connection.close()
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.db = 'aic_reg_' + uuid.uuid4().hex[:12]
        self.q('CREATE DATABASE ' + self.db)
        self.q('USE ' + self.db)

    def tearDown(self):
        self.connection.rollback()
        self.q('SET SESSION ai_column_strict_read=OFF')
        self.q('DROP DATABASE ' + self.db)

    def q(self, sql, args=None):
        with self.connection.cursor() as cur:
            cur.execute(sql, args)
            return cur.fetchall() if cur.description else cur.rowcount

    def wait(self, sql, expected, timeout=20):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            result = self.q(sql)
            if result == expected:
                return
            time.sleep(0.05)
        self.fail('%s yielded %r; expected %r' % (sql, result, expected))

    def create(self, columns='result VARCHAR(200)', expression='content', extra=''):
        self.q("CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(200),%s AI COLUMN (AI_COMPLETE('%s',%s)) WITH(max_retries=0)%s)" %
               (columns, self.model, expression, extra))

    def test_show_and_definition_change(self):
        self.create()
        self.assertEqual('result', self.q('SHOW AI COLUMNS FROM docs')[0][0])
        self.q("INSERT INTO docs(id,content) VALUES(1,'slow-old')")
        time.sleep(0.15)
        self.q("ALTER TABLE docs MODIFY COLUMN result VARCHAR(200) AI COLUMN (AI_COMPLETE('%s',CONCAT('v2:',content))) WITH(max_retries=0)" % self.model)
        self.wait('SELECT result FROM docs', (('reply:v2:slow-old',),))
        time.sleep(0.9)
        self.assertEqual((('reply:v2:slow-old',),), self.q('SELECT result FROM docs'))

    def test_composite_key_and_rollback(self):
        self.q("CREATE TABLE docs(tenant INT,k VARCHAR(100),content VARCHAR(100),result VARCHAR(200) AI COLUMN(AI_COMPLETE('%s',content)),PRIMARY KEY(tenant,k))" % self.model)
        key = "quote' and slash\\ 中文"
        marker = 'rollback-' + uuid.uuid4().hex
        self.q('START TRANSACTION')
        self.q('INSERT INTO docs(tenant,k,content) VALUES(1,%s,%s)', (key, marker))
        time.sleep(0.3)
        self.assertNotIn(marker, self.events)
        self.q('ROLLBACK')
        self.assertEqual(((0,),), self.q('SELECT COUNT(*) FROM docs'))
        self.q('INSERT INTO docs(tenant,k,content) VALUES(1,%s,%s)', (key, 'committed'))
        self.wait('SELECT result FROM docs', (('reply:committed',),))
        self.q('UPDATE docs SET k=%s WHERE tenant=1', (key + ' changed',))
        self.wait('SELECT result FROM docs', (('reply:committed',),))

    def test_expired_claim_recovery(self):
        self.create()
        self.q('START TRANSACTION')
        self.q("INSERT INTO docs(id,content) VALUES(1,'recover-me')")
        table_id = self.q("SELECT table_id FROM oceanbase.__all_ai_column WHERE database_name=%s AND table_name='docs'", (self.db,))[0][0]
        self.q("UPDATE oceanbase.__all_ai_column_task SET state=2,claim_token='interrupted-owner',lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE table_id=%s", (table_id,))
        self.q('COMMIT')
        self.wait('SELECT result FROM docs', (('reply:recover-me',),))

    def test_implicit_primary_key(self):
        self.q("CREATE TABLE docs(content VARCHAR(100),result VARCHAR(200) AI COLUMN(AI_COMPLETE('%s',content)))" % self.model)
        self.q("INSERT INTO docs(content) VALUES('no-key')")
        self.wait('SELECT result FROM docs', (('reply:no-key',),))
        self.q("UPDATE docs SET content='updated'")
        self.wait('SELECT result FROM docs', (('reply:updated',),))
        self.q('DELETE FROM docs')
        self.assertEqual(((0,),), self.q('SELECT COUNT(*) FROM docs'))

    def test_direct_writes_and_retry_errors(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'ready')")
        self.wait('SELECT result FROM docs', (('reply:ready',),))
        for sql in ["UPDATE docs SET result='forged'", "INSERT INTO docs(id,content,result) VALUES(2,'x','forged')",
                    "ALTER TABLE docs RETRY AI COLUMN content", "ALTER TABLE docs RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT"]:
            with self.assertRaises(pymysql.Error) as caught:
                self.q(sql)
            self.assertEqual(1235, caught.exception.args[0], sql)

    def test_strict_read_references_and_transaction(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'ready')")
        self.wait('SELECT result FROM docs', (('reply:ready',),))
        self.q('SET SESSION ai_column_strict_read=ON')
        self.q('START TRANSACTION')
        self.q("UPDATE docs SET content='new' WHERE id=1")
        self.assertEqual(((1,),), self.q('SELECT id FROM docs'))
        for sql in ['SELECT result FROM docs WHERE id=999', 'SELECT x.result FROM (SELECT result FROM docs) x',
                    'DELETE FROM docs WHERE result IS NULL', "UPDATE docs SET content='x' WHERE result IS NULL"]:
            with self.assertRaises(pymysql.Error) as caught:
                self.q(sql)
            self.assertEqual(11120, caught.exception.args[0], sql)
        self.q('ROLLBACK')
        self.assertEqual((('reply:ready',),), self.q('SELECT result FROM docs'))

    def test_partial_failure_publishes_group(self):
        self.create(extra=",bad JSON AI COLUMN(AI_COMPLETE('%s',CONCAT('BAD_JSON:',content))) WITH(max_retries=0)" % self.model)
        self.q("INSERT INTO docs(id,content) VALUES(1,'group')")
        self.wait('SELECT result,bad FROM docs', (('reply:group', None),))
        self.wait("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task t JOIN oceanbase.__all_ai_column a ON a.table_id=t.table_id AND a.column_id=t.column_id WHERE a.database_name='%s'" % self.db, ((0,),))
        self.q('SET SESSION ai_column_strict_read=ON')
        self.assertEqual((('reply:group',),), self.q('SELECT result FROM docs'))
        with self.assertRaises(pymysql.Error) as caught:
            self.q('SELECT bad FROM docs')
        self.assertEqual(11120, caught.exception.args[0])

    def test_drop_recreate_does_not_block_publishing(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'slow-old')")
        time.sleep(0.15)
        self.q('DROP TABLE docs')
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'fresh-table')")
        self.wait('SELECT result FROM docs', (('reply:fresh-table',),))
        time.sleep(0.9)
        self.assertEqual((('reply:fresh-table',),), self.q('SELECT result FROM docs'))

    def test_storage_conversion_failure_reaches_terminal_state(self):
        self.create(columns='result TINYINT')
        self.q("INSERT INTO docs(id,content) VALUES(1,'BIG_INT')")
        self.wait("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task t JOIN oceanbase.__all_ai_column a ON a.table_id=t.table_id AND a.column_id=t.column_id WHERE a.database_name='%s'" % self.db, ((0,),))
        self.assertEqual(((None,),), self.q('SELECT result FROM docs'))
        self.q('SET SESSION ai_column_strict_read=ON')
        with self.assertRaises(pymysql.Error) as caught:
            self.q('SELECT result FROM docs')
        self.assertEqual(11120, caught.exception.args[0])

    def test_vector_trigger_regeneration_and_repeated_updates(self):
        self.q("CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(20),emb VECTOR(4))")
        self.q("INSERT INTO docs VALUES(1,'x',[0,0,0,0]),(2,'x',[10,0,0,0]),(3,'x',[20,0,0,0])")
        self.q("CREATE VECTOR INDEX vi ON docs(emb) WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)")
        self.q("CREATE TRIGGER bu BEFORE UPDATE ON docs FOR EACH ROW BEGIN IF NOT(NEW.content <=> OLD.content) THEN SET NEW.emb=NULL; END IF; END")
        for iteration in range(3):
            self.q('START TRANSACTION')
            self.q("UPDATE docs SET content=%s WHERE id=2", ('changed-%d' % iteration,))
            self.assertEqual(((None,),), self.q('SELECT emb FROM docs WHERE id=2'))
            rows = self.q('SELECT id FROM docs ORDER BY l2_distance(emb,[0,0,0,0]) APPROXIMATE LIMIT 3')
            self.assertEqual({1, 3}, {r[0] for r in rows})
            self.q('COMMIT')
            self.q('UPDATE docs SET emb=[0,0,0,0] WHERE id=2')
            self.q('UPDATE docs SET emb=IF(id=2,[1,0,0,0],emb) WHERE id=2')
            self.assertEqual((('[1,0,0,0]',),), self.q('SELECT emb FROM docs WHERE id=2'))
            rows = self.q('SELECT id FROM docs ORDER BY l2_distance(emb,[0,0,0,0]) APPROXIMATE LIMIT 2')
            self.assertEqual({1, 2}, {r[0] for r in rows})

    def test_null_input_is_not_replaced_with_empty_string(self):
        self.create()
        calls = len(self.events)
        self.q('INSERT INTO docs(id,content) VALUES(1,NULL)')
        self.wait("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task t JOIN oceanbase.__all_ai_column a ON a.table_id=t.table_id AND a.column_id=t.column_id WHERE a.database_name='%s'" % self.db, ((0,),))
        self.assertEqual(((None,),), self.q('SELECT result FROM docs'))
        self.assertEqual(calls, len(self.events))


if __name__ == '__main__':
    unittest.main(verbosity=2)
