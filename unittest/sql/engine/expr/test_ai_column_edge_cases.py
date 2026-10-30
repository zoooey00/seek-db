"""AI column boundary regressions using a local deterministic model service."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import sys
import threading
import time
import unittest
import uuid

import pymysql

test_dir = Path(os.environ.get('SEEKDB_AI_TEST_DIR', str(Path(__file__).parent)))
spec = importlib.util.spec_from_file_location('ai_column_existing', test_dir / 'test_ai_column_integration.py')
existing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(existing)


class AIColumnEdgeCases(existing.AIColumnIntegration):
    @classmethod
    def setUpClass(cls):
        cls.events = []
        cls.lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                text = next((m['content'] for m in request.get('messages', []) if m['role'] == 'user'), '')
                with cls.lock:
                    cls.events.append(text)
                    attempt = cls.events.count(text)
                lose_response = text.startswith('lost-response:') and attempt == 1
                if attempt == 1 and (text.startswith('retry-429:') or text.startswith('retry-5xx:')):
                    payload = b'{"error":{"message":"temporary fault"}}'
                    self.send_response(429 if text.startswith('retry-429:') else 503)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
                if text.startswith('slow-old:'):
                    time.sleep(0.8)
                system = next((m['content'] for m in request.get('messages', []) if m['role'] == 'system'), '')
                if 'valid JSON value' in system:
                    content = json.dumps({'input': text})
                else:
                    content = 'reply:' + text
                payload = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': content}}]}).encode()
                if lose_response:
                    self.close_connection = True
                    return
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
        cls.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'),
            password=os.environ.get('SEEKDB_PASSWORD', ''),
            charset='utf8mb4', autocommit=True, read_timeout=30)
        cls.model = 'aic_edge_' + uuid.uuid4().hex[:12]
        cls.endpoint = cls.model + '_ep'
        with cls.connection.cursor() as cursor:
            cursor.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                           (cls.model, json.dumps({'type': 'completion', 'model_name': 'edge-regression'})))
            cursor.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                           (cls.endpoint, json.dumps({
                               'ai_model_name': cls.model,
                               'url': 'http://127.0.0.1:%d/v1/chat/completions' % cls.server.server_port,
                               'access_key': 'local-test-only', 'provider': 'openai',
                               'request_model_name': 'edge-regression'})))

    def test_case_only_dependency_update_regenerates(self):
        self.q("CREATE TABLE docs(id INT PRIMARY KEY,content VARCHAR(100) COLLATE utf8mb4_general_ci,"
               "result VARCHAR(200) AI COLUMN(AI_COMPLETE('%s',content)))" % self.model)
        self.q("INSERT INTO docs(id,content) VALUES(1,'lower')")
        self.wait('SELECT result FROM docs', (('reply:lower',),))
        self.q("UPDATE docs SET content='LOWER' WHERE id=1")
        self.wait('SELECT result FROM docs', (('reply:LOWER',),), timeout=3)

    def test_trailing_space_dependency_update_regenerates(self):
        self.q("CREATE TABLE docs(id INT PRIMARY KEY,content VARCHAR(100) COLLATE utf8mb4_general_ci,"
               "result VARCHAR(200) AI COLUMN(AI_COMPLETE('%s',content)))" % self.model)
        self.q("INSERT INTO docs(id,content) VALUES(1,'text')")
        self.wait('SELECT result FROM docs', (('reply:text',),))
        self.q("UPDATE docs SET content='text ' WHERE id=1")
        self.wait('SELECT result FROM docs', (('reply:text ',),), timeout=3)

    def test_ordinary_alter_rename_succeeds(self):
        self.q('CREATE TABLE ordinary(id INT PRIMARY KEY)')
        self.q('INSERT INTO ordinary VALUES(1)')
        self.q('ALTER TABLE ordinary RENAME TO renamed')
        self.assertEqual(((1,),), self.q('SELECT id FROM renamed'))

    def test_view_does_not_allow_result_assignment(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'view-input')")
        self.wait('SELECT result FROM docs', (('reply:view-input',),))
        self.q('CREATE VIEW result_view AS SELECT * FROM docs')
        with self.assertRaises(pymysql.Error) as caught:
            self.q("UPDATE result_view SET result='forged' WHERE id=1")
        self.assertEqual(1235, caught.exception.args[0])

    def test_business_trigger_cannot_assign_result(self):
        self.create(extra=',note INT')
        self.q("INSERT INTO docs(id,content,note) VALUES(1,'trigger-input',0)")
        self.wait('SELECT result FROM docs', (('reply:trigger-input',),))
        self.q("CREATE TRIGGER forge_result BEFORE UPDATE ON docs FOR EACH ROW SET NEW.result='forged'")
        with self.assertRaises(pymysql.Error) as caught:
            self.q('UPDATE docs SET note=1 WHERE id=1')
        self.assertEqual(1235, caught.exception.args[0])
        self.assertEqual((('reply:trigger-input', 0),), self.q('SELECT result,note FROM docs'))

    def test_business_trigger_can_update_ordinary_column(self):
        self.create(columns='result JSON', extra=',note INT')
        self.q("INSERT INTO docs(id,content,note) VALUES(1,'json-trigger',0)")
        self.wait("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM docs", (('json-trigger',),))
        self.q('CREATE TRIGGER increment_note BEFORE UPDATE ON docs FOR EACH ROW SET NEW.note=NEW.note+1')
        self.q('UPDATE docs SET note=2 WHERE id=1')
        self.assertEqual(((3, 'json-trigger'),), self.q(
            "SELECT note,JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM docs"))

    def test_internal_trigger_namespace_cannot_be_forged(self):
        self.create()
        with self.assertRaises(pymysql.Error) as caught:
            self.q('CREATE TRIGGER __seekdb_ai_forged BEFORE UPDATE ON docs FOR EACH ROW SET NEW.content=NEW.content')
        self.assertEqual(1235, caught.exception.args[0])

    def test_nested_view_cannot_insert_result(self):
        self.create()
        self.q('CREATE VIEW result_view AS SELECT id,content,result AS generated_text FROM docs')
        self.q('CREATE VIEW outer_view AS SELECT * FROM result_view')
        with self.assertRaises(pymysql.Error) as caught:
            self.q("INSERT INTO outer_view(id,content,generated_text) VALUES(1,'x','forged')")
        self.assertEqual(1235, caught.exception.args[0])

    def test_strict_view_read_of_ordinary_column_remains_allowed(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'view-ready')")
        self.wait('SELECT result FROM docs', (('reply:view-ready',),))
        self.q('CREATE VIEW all_columns AS SELECT * FROM docs')
        self.q('SET SESSION ai_column_strict_read=ON')
        self.q('START TRANSACTION')
        self.q("UPDATE docs SET content='view-pending' WHERE id=1")
        self.assertEqual(((1,),), self.q('SELECT id FROM all_columns'))
        with self.assertRaises(pymysql.Error) as caught:
            self.q('SELECT result FROM all_columns')
        self.assertEqual(11120, caught.exception.args[0])

    def wait_for_call(self, marker, timeout=5):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            with self.lock:
                if marker in self.events:
                    return
            time.sleep(0.02)
        self.fail('No model request arrived for ' + marker)

    def retry_once(self, prefix):
        self.q("CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(200),"
               "result VARCHAR(200) AI COLUMN(AI_COMPLETE('%s',content)) WITH(max_retries=1))" % self.model)
        marker = prefix + uuid.uuid4().hex
        self.q('INSERT INTO docs(id,content) VALUES(1,%s)', (marker,))
        self.wait('SELECT result FROM docs', (('reply:' + marker,),), timeout=12)
        with self.lock:
            self.assertEqual(2, self.events.count(marker))

    def test_http_429_recovers_with_counted_requests(self):
        self.retry_once('retry-429:')

    def test_http_503_recovers_with_counted_requests(self):
        self.retry_once('retry-5xx:')

    def test_lost_response_recovers_with_counted_duplicate(self):
        self.retry_once('lost-response:')

    def test_late_response_cannot_overwrite_new_row_version(self):
        self.create()
        old = 'slow-old:' + uuid.uuid4().hex
        new = 'fresh:' + uuid.uuid4().hex
        self.q('INSERT INTO docs(id,content) VALUES(1,%s)', (old,))
        self.wait_for_call(old)
        self.q('UPDATE docs SET content=%s WHERE id=1', (new,))
        self.wait('SELECT result FROM docs', (('reply:' + new,),))
        time.sleep(0.9)
        self.assertEqual(((new, 'reply:' + new),), self.q('SELECT content,result FROM docs'))

    def test_delete_during_inference_cannot_restore_row(self):
        self.create()
        marker = 'slow-old:' + uuid.uuid4().hex
        self.q('INSERT INTO docs(id,content) VALUES(1,%s)', (marker,))
        self.wait_for_call(marker)
        self.q('DELETE FROM docs WHERE id=1')
        time.sleep(1)
        self.assertEqual(((0,),), self.q('SELECT COUNT(*) FROM docs'))
        self.assertEqual(((0,),), self.q(
            'SELECT COUNT(*) FROM oceanbase.__all_ai_column_task t '
            'JOIN oceanbase.__all_ai_column a ON a.table_id=t.table_id AND a.column_id=t.column_id '
            'WHERE a.database_name=%s', (self.db,)))


    def test_business_trigger_dependency_update_regenerates(self):
        self.create(extra=',note INT')
        self.q("INSERT INTO docs(id,content,note) VALUES(1,'before',0)")
        self.wait('SELECT result FROM docs', (('reply:before',),))
        self.q("CREATE TRIGGER rewrite_input BEFORE UPDATE ON docs FOR EACH ROW BEGIN IF NEW.note=1 THEN SET NEW.content='after'; END IF; END")
        self.q('UPDATE docs SET note=1 WHERE id=1')
        self.wait('SELECT content,result FROM docs', (('after', 'reply:after'),))

    def test_business_trigger_dependency_update_rolls_back(self):
        self.create(extra=',note INT')
        self.q("INSERT INTO docs(id,content,note) VALUES(1,'original',0)")
        self.wait('SELECT result FROM docs', (('reply:original',),))
        marker = 'rolled-back-trigger:' + uuid.uuid4().hex
        self.q('CREATE TRIGGER rewrite_input BEFORE UPDATE ON docs FOR EACH ROW SET NEW.content=%s', (marker,))
        self.q('START TRANSACTION')
        self.q('UPDATE docs SET note=1 WHERE id=1')
        self.assertEqual(((marker, None),), self.q('SELECT content,result FROM docs'))
        self.q('ROLLBACK')
        time.sleep(0.3)
        self.assertNotIn(marker, self.events)
        self.assertEqual((('original', 'reply:original'),), self.q('SELECT content,result FROM docs'))

    def test_business_trigger_reverting_dependency_avoids_regeneration(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'same')")
        self.wait('SELECT result FROM docs', (('reply:same',),))
        self.q('CREATE TRIGGER preserve_input BEFORE UPDATE ON docs FOR EACH ROW SET NEW.content=OLD.content')
        calls = len(self.events)
        self.q("UPDATE docs SET content='discarded' WHERE id=1")
        self.assertEqual((('same', 'reply:same'),), self.q('SELECT content,result FROM docs'))
        time.sleep(0.3)
        self.assertEqual(calls, len(self.events))

    def test_renamed_ai_table_accepts_updates_and_manual_retry(self):
        self.create()
        self.q("INSERT INTO docs(id,content) VALUES(1,'before-rename')")
        self.wait('SELECT result FROM docs', (('reply:before-rename',),))
        self.q('RENAME TABLE docs TO renamed')
        self.q("UPDATE renamed SET content='after-rename' WHERE id=1")
        self.wait('SELECT result FROM renamed', (('reply:after-rename',),))
        self.q('ALTER TABLE renamed RETRY AI COLUMN result WHERE id=1')
        self.wait('SELECT result FROM renamed', (('reply:after-rename',),))
        self.assertEqual((('renamed',),), self.q(
            'SELECT table_name FROM oceanbase.__all_ai_column WHERE database_name=%s', (self.db,)))

    def test_rename_during_model_call_publishes_to_same_table_id(self):
        self.create()
        marker = 'slow-old:' + uuid.uuid4().hex
        self.q('INSERT INTO docs(id,content) VALUES(1,%s)', (marker,))
        self.wait_for_call(marker)
        self.q('RENAME TABLE docs TO renamed')
        self.q('CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(200),result VARCHAR(200))')
        self.q("INSERT INTO docs VALUES(1,'replacement','untouched')")
        self.wait('SELECT result FROM renamed', (('reply:' + marker,),))
        self.assertEqual((('untouched',),), self.q('SELECT result FROM docs'))


if __name__ == '__main__':
    names = [name for name in AIColumnEdgeCases.__dict__ if name.startswith('test_')]
    suite = unittest.TestSuite(AIColumnEdgeCases(name) for name in sorted(names))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
