"""SQL integration checks using the real engine and a local deterministic HTTP double."""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import pymysql
from mock_ai_service import start, REQUESTS, LOCK, embedding

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=2881)
parser.add_argument('--host', default='127.0.0.1')
parser.add_argument('--mock-port', type=int, default=18080)
parser.add_argument('--output', type=Path, default=Path('ai-sql-results.json'))
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
service = start(port=args.mock_port)
conn = pymysql.connect(host=args.host, port=args.port, user='root',
                       password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                       connect_timeout=5, read_timeout=45, autocommit=True, charset='utf8mb4')
records = []

def sql(query, params=None):
    with conn.cursor() as cur:
        cur.execute(query, params)
        return cur.fetchall()

def decode(value):
    try:
        return json.loads(value)
    except (TypeError, ValueError):
        return value

def check(label, query, expected=None, predicate=None, error=False):
    rec = {'name': label, 'sql': query}
    try:
        rows = sql(query)
        rec['actual'] = rows
        rec['passed'] = not error and (predicate(rows) if predicate else rows == expected)
    except pymysql.Error as exc:
        rec.update(error=str(exc), error_code=exc.args[0], passed=error)
    except Exception as exc:
        rec.update(error=repr(exc), passed=False)
    records.append(rec)
    print(('PASS ' if rec['passed'] else 'FAIL ') + label + ('' if rec['passed'] else ' '+str(rec)), flush=True)
    args.output.write_text(json.dumps(records, ensure_ascii=False, indent=2))

def model(name, kind, provider, route, key='test-only'):
    for procedure, value in [('DROP_AI_MODEL_ENDPOINT', name+'_ep'), ('DROP_AI_MODEL', name)]:
        try:
            sql(f'CALL DBMS_AI_SERVICE.{procedure}(%s)', (value,))
        except pymysql.Error:
            pass
    sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
        (name, json.dumps({'type':kind,'model_name':'arbitrary-model'})))
    sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
        (name+'_ep', json.dumps({'ai_model_name':name,'url':'http://127.0.0.1:'+str(args.mock_port)+route,
                                'provider':provider,'access_key':key,'request_model_name':'request-override'})))

try:
    print('version', sql('SELECT VERSION()'), flush=True)
    sql('CREATE DATABASE IF NOT EXISTS ai_enhancement_validation')
    sql('USE ai_enhancement_validation')
    sql('SET ob_query_timeout=30000000')
    model('ai_test_complete','completion','openai','/v1/chat/completions')
    model('ai_test_embed','dense_embedding','openai','/v1/embeddings')
    model('ai_test_rerank','rerank','siliconflow','/v1/rerank')
    model('ai_test_ollama','completion','oLlAmA','/api/generate','')
    model('ai_test_ollama_embed','dense_embedding','OLLAMA','/api/embed','')
    model('ai_test_empty','completion','openai','/empty/v1/chat/completions')
    model('ai_test_bad_dim','dense_embedding','openai','/bad-dim/v1/embeddings')
    model('ai_test_bad_vector','dense_embedding','openai','/bad-vector/v1/embeddings')
    model('ai_test_bad_index','rerank','siliconflow','/bad-index/v1/rerank')
    model('ai_test_embed_index','dense_embedding','openai','/bad-index/v1/embeddings')
    model('ai_test_root','completion','openai','/wrong-root/v1/chat/completions')
    model('ai_test_http','completion','openai','/http-error/v1/chat/completions')
    model('ai_test_offline','completion','openai','/v1/chat/completions')
    sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
        ('ai_test_offline_ep',json.dumps({'url':'http://127.0.0.1:1/v1/chat/completions'})))
    time.sleep(3)
    check('ordinary SQL unchanged','SELECT 1+2, IFNULL(NULL,7)', ((3,7),))
    check('prompt JSON array', "SELECT JSON_TYPE(JSON_EXTRACT(AI_PROMPT('{0}',JSON_ARRAY(1,2)),'$.args[0]'))", (('ARRAY',),))
    check('prompt JSON object', "SELECT JSON_TYPE(JSON_EXTRACT(AI_PROMPT('{0}',JSON_OBJECT('a','中')),'$.args[0]'))", (('OBJECT',),))
    sql('DROP TABLE IF EXISTS ai_long_input')
    sql('CREATE TABLE ai_long_input(t LONGTEXT,a TEXT)')
    sql("INSERT INTO ai_long_input VALUES ('{0}/{0}/{0}',REPEAT('中',1000))")
    check('TEXT and LONGTEXT prompt', 'SELECT JSON_LENGTH(JSON_EXTRACT(AI_PROMPT(t,a),\'$.args\')) FROM ai_long_input', ((1,),))
    check('completion basic', "SELECT AI_COMPLETE('ai_test_complete','hello')", predicate=lambda r: decode(r[0][0])=='hello')
    check('completion NULL config', "SELECT AI_COMPLETE('ai_test_complete','hello',NULL)", predicate=lambda r: decode(r[0][0])=='hello')
    check('completion native JSON config', "SELECT AI_COMPLETE('ai_test_complete','hello',JSON_OBJECT('temperature',0.25,'max_tokens',7))", predicate=lambda r: decode(r[0][0])=='hello')
    check('completion string JSON config', "SELECT AI_COMPLETE('ai_test_complete','hello','{\"temperature\":0.25,\"max_tokens\":7}')", predicate=lambda r: decode(r[0][0])=='hello')
    check('completion repeated placeholders', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT('{0}/{0}/{0}','你好世界'))", predicate=lambda r: decode(r[0][0])=='你好世界/你好世界/你好世界')
    check('completion nested prompt', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT('outer {0}',AI_PROMPT('inner {0}','ok')))", predicate=lambda r: decode(r[0][0])=='outer inner ok')
    check('completion JSON args', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT('{0}',JSON_ARRAY(1,2)))", predicate=lambda r: decode(r[0][0])==[1,2])
    check('completion long prompt', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT(t,a)) FROM ai_long_input", predicate=lambda r: decode(r[0][0])=='/'.join(['中'*1000]*3))
    check('completion empty response rejected', "SELECT AI_COMPLETE('ai_test_empty','hello')", error=True)
    check('completion config array rejected', "SELECT AI_COMPLETE('ai_test_complete','hello',JSON_ARRAY(1))", error=True)
    check('nonobject HTTP JSON rejected', "SELECT AI_COMPLETE('ai_test_root','hello')", error=True)
    check('HTTP error rejected', "SELECT AI_COMPLETE('ai_test_http','hello')", error=True)
    check('network connection failure rejected', "SELECT AI_COMPLETE('ai_test_offline','hello')", error=True)
    check('SQL remains usable after network errors', 'SELECT 9*9', ((81,),))
    check('prompt out of range', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT('{2}','x'))", error=True)
    check('prompt huge index', "SELECT AI_COMPLETE('ai_test_complete',AI_PROMPT('{999999999999999999999999999999}','x'))", error=True)
    check('embedding default dimension', "SELECT AI_EMBED('ai_test_embed','hello')", predicate=lambda r: decode(r[0][0])==embedding('hello',3))
    check('embedding explicit dimension', "SELECT AI_EMBED('ai_test_embed','hello',5)", predicate=lambda r: decode(r[0][0])==embedding('hello',5))
    check('embedding NULL input', "SELECT AI_EMBED('ai_test_embed',NULL)", ((None,),))
    for dimension in ['0','-1','1.5',"'3'",'NULL']:
        check('invalid dimension '+dimension, f"SELECT AI_EMBED('ai_test_embed','hello',{dimension})", error=True)
    check('embedding wrong dimension rejected', "SELECT AI_EMBED('ai_test_bad_dim','hello',3)", error=True)
    check('embedding nonnumeric vector rejected', "SELECT AI_EMBED('ai_test_bad_vector','hello')", error=True)
    check('ollama completion native protocol', "SELECT AI_COMPLETE('ai_test_ollama','hello',JSON_OBJECT('temperature',0.5,'max_tokens',12))", predicate=lambda r: decode(r[0][0])=='hello')
    check('ollama embedding native protocol', "SELECT AI_EMBED('ai_test_ollama_embed','hello',4)", predicate=lambda r: decode(r[0][0])==embedding('hello',4))
    check('endpoint provider persisted', "SELECT PROVIDER FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE ENDPOINT_NAME='ai_test_ollama_ep'", predicate=lambda r: str(r[0][0]).lower()=='ollama')
    sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
        ('ai_test_ollama_ep', json.dumps({'provider':'Ollama'})))
    time.sleep(3)
    check('Ollama without key survives provider-only ALTER', "SELECT AI_COMPLETE('ai_test_ollama','hello')", predicate=lambda r: decode(r[0][0])=='hello')
    sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
        ('ai_test_ollama_ep', json.dumps({'provider':'oLlAmA'})))
    time.sleep(3)
    check('Ollama without key survives repeated ALTER', "SELECT AI_COMPLETE('ai_test_ollama','again',NULL)", predicate=lambda r: decode(r[0][0])=='again')
    sql('DROP TABLE IF EXISTS ai_batch_input')
    sql('CREATE TABLE ai_batch_input(id INT PRIMARY KEY, content VARCHAR(100))')
    with conn.cursor() as cur:
        cur.executemany('INSERT INTO ai_batch_input VALUES(%s,%s)', [(i,None if i%11==0 else 'row-'+str(i)) for i in range(150)])
    with LOCK:
        REQUESTS.clear()
    check('embedding across rows and multiple batches', "SELECT id,AI_EMBED('ai_test_embed',content,4) FROM ai_batch_input ORDER BY id",
          predicate=lambda r: len(r)==150 and all((row[1] is None if row[0]%11==0 else decode(row[1])==embedding('row-'+str(row[0]),4)) for row in r))
    with LOCK:
        emb_requests = [r for r in REQUESTS if r['path'].endswith('/v1/embeddings')]
    rec={'name':'real multi-input HTTP requests','passed':bool(emb_requests) and any(len(r['body']['input'])>1 for r in emb_requests),
         'request_sizes':[len(r['body']['input']) for r in emb_requests]}
    records.append(rec)
    print(rec,flush=True)
    check('embedding filter and LIMIT', "SELECT id,AI_EMBED('ai_test_embed',content,3) FROM ai_batch_input WHERE id>=70 AND id%2=0 ORDER BY id LIMIT 9",
          predicate=lambda r: len(r)==9 and all((row[1] is None if row[0]%11==0 else decode(row[1])==embedding('row-'+str(row[0]),3)) for row in r))
    check('embedding per-row dimensions', "SELECT id,AI_EMBED('ai_test_embed',content,id%3+1) FROM ai_batch_input WHERE id<8 ORDER BY id",
          predicate=lambda r: len(r)==8 and all((row[1] is None if row[0]%11==0 else decode(row[1])==embedding('row-'+str(row[0]),row[0]%3+1)) for row in r))
    check('embedding duplicate response indices rejected', "SELECT AI_EMBED('ai_test_embed_index',content) FROM ai_batch_input WHERE id BETWEEN 1 AND 8", error=True)
    check('embedding usable after failed batch', "SELECT AI_EMBED('ai_test_embed','recovered')", predicate=lambda r: decode(r[0][0])==embedding('recovered',3))
    sql('DROP TABLE IF EXISTS ai_null_input')
    sql('CREATE TABLE ai_null_input(id INT,content VARCHAR(10))')
    sql('INSERT INTO ai_null_input VALUES(1,NULL),(2,NULL)')
    with LOCK:
        REQUESTS.clear()
    check('all-NULL batch', "SELECT AI_EMBED('ai_test_embed',content) FROM ai_null_input", ((None,),(None,)))
    records.append({'name':'all-NULL batch makes no HTTP request','passed':not REQUESTS})
    documents = ['doc-'+str(i) for i in range(45)]
    order = sorted(range(45), key=lambda i: (-(sum(documents[i].encode())%7),i))
    quoted = conn.escape(json.dumps(documents))
    check('rerank 45 strings global order', f"SELECT AI_RERANK('ai_test_rerank','query',{quoted})",
          predicate=lambda r: [v['index'] for v in decode(r[0][0])]==order)
    objects=[{'id':'origin-'+str(i),'text':x} for i,x in enumerate(documents)]
    quoted=conn.escape(json.dumps(objects))
    check('rerank objects preserve original IDs', f"SELECT AI_RERANK('ai_test_rerank','query',{quoted},'text')",
          predicate=lambda r: [v['id'] for v in decode(r[0][0])]==['origin-'+str(i) for i in order])
    check('rerank invalid index rejected', "SELECT AI_RERANK('ai_test_bad_index','query','[\"one\",\"two\"]')", error=True)
    check('rerank missing text key rejected', "SELECT AI_RERANK('ai_test_rerank','query','[{\"id\":1}]','text')", error=True)
    sql('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
        ('ai_test_complete_ep',json.dumps({'provider':'oLlAmA','access_key':'','url':'http://127.0.0.1:'+str(args.mock_port)+'/api/generate'})))
    time.sleep(3)
    check('endpoint ALTER from OpenAI to Ollama with empty key', "SELECT AI_COMPLETE('ai_test_complete','altered')", predicate=lambda r: decode(r[0][0])=='altered')
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_test_complete_ep')")
    check('endpoint DROP persisted', "SELECT COUNT(*) FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE ENDPOINT_NAME='ai_test_complete_ep'", ((0,),))
finally:
    conn.close()
    service.shutdown()
    report={'passed':sum(r['passed'] for r in records),'total':len(records),'cases':records,
            'mock':'local deterministic test double; official competition Mock unavailable'}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"{report['passed']}/{report['total']} SQL checks passed",flush=True)

raise SystemExit(0 if report['passed'] == report['total'] else 1)
