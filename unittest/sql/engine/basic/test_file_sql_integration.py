"""Exercise file table functions through the native seekdb SQL executor.

Run only against an isolated local server: this test changes secure_file_priv.
No external SQL engine is used. The input fixtures are created under data-dir.
"""
import argparse,base64,datetime,decimal,json,os,shutil,tempfile
from pathlib import Path
import pymysql

parser=argparse.ArgumentParser()
parser.add_argument('--host',default='127.0.0.1')
parser.add_argument('--port',type=int,default=2881)
parser.add_argument('--user',default='root')
parser.add_argument('--unix-socket',help='Use the local admin socket to configure secure_file_priv')
parser.add_argument('--data-dir',type=Path)
parser.add_argument('--output',type=Path,default=Path('file-sql-results.json'))
args=parser.parse_args()
directory=args.data_dir or Path(tempfile.mkdtemp(prefix='seekdb-file-sql-'))
directory.mkdir(parents=True,exist_ok=True)
REPO=Path(__file__).resolve().parents[4]
official=REPO/'tools/deploy/mysql_test/test_suite/file_sql/data'
for name in ['basic.csv','basic.jsonl','invalid.txt']:
    shutil.copy2(official/name,directory/name)
(directory/'simple.parquet').write_bytes(base64.b64decode((official/'simple.parquet.b64').read_bytes()))
(directory/'join.csv').write_text('id,category\n1,A\n3,C\n4,D\n',encoding='utf-8')
(directory/'union.jsonl').write_text('{"id":1,"v":1}\n{"id":2,"extra":"late"}\n{"id":3,"v":2.5}\n',encoding='utf-8')
(directory/'bad.csv').write_text('a,b\n1\n',encoding='utf-8')
(directory/'nested.jsonl').write_text('{"x":{"y":1}}\n',encoding='utf-8')
(directory/'nulls.csv').write_text('id,n\n1,\n2,NULL\n',encoding='utf-8')
(directory/'empty.csv').write_text('id,v\n',encoding='utf-8')
(directory/'alternate.csv').write_text('id,value\n77,other\n',encoding='utf-8')
(directory/'custom.csv').write_text('1;""\n2;missing\n',encoding='utf-8')
with (directory/'large.csv').open('w',encoding='utf-8')as stream:
    stream.write('id,g,value\n')
    for i in range(10000):stream.write(f'{i},{i%4},{i*0.5}\n')
def connect():
    return pymysql.connect(host=args.host,port=args.port,user=args.user,
        password=os.environ.get('SEEKDB_TEST_PASSWORD',''),unix_socket=args.unix_socket,
        charset='utf8mb4',autocommit=True,read_timeout=90)
connection=connect()
records=[]
def query(sql,params=None):
    with connection.cursor()as cursor:
        cursor.execute(sql,params)
        return cursor.fetchall() if cursor.description else ()
def normalized(value):
    if isinstance(value,(list,tuple)):return [normalized(v)for v in value]
    if isinstance(value,(datetime.date,datetime.datetime)):return str(value)
    if isinstance(value,decimal.Decimal):return float(value)
    return value
def check(name,sql,expected=None,error=None,predicate=None):
    record={'name':name,'sql':sql}
    try:
        actual=normalized(query(sql));record['actual']=actual
        record['passed']=error is None and (predicate(actual) if predicate else expected is None or actual==normalized(expected))
    except pymysql.Error as failure:
        code=failure.args[0]
        # Client errors and broken connections never satisfy an expected SQL error.
        record.update(error_code=code,error=str(failure),passed=error is not None and code not in (0,2006,2013) and (error is True or code==error))
    records.append(record);print(('PASS 'if record['passed']else'FAIL ')+name,flush=True)
    if not record['passed']:print(json.dumps(record,ensure_ascii=False,default=str),flush=True)
def path(name):return connection.escape(str(directory/name))
def csv(name,options=''):return f'read_csv({path(name)}{options})'
CSV_XML=",'xml'"
CSV_CUSTOM=",header=false,delimiter=';',nullstr='missing'"
CSV_SEMICOLON=",delimiter=';'"
try:
    query('CREATE DATABASE IF NOT EXISTS file_sql_regression');query('USE file_sql_regression')
    query('SET SESSION enable_file_sql=OFF')
    check('disabled file SQL rejected',f'SELECT COUNT(*) FROM {csv("basic.csv")}',error=1235)
    check('ordinary SQL remains usable','SELECT 6*7',[[42]])
    check('ordinary DESCRIBE with file SQL disabled','DESCRIBE SELECT 42',predicate=lambda rows:'EXPRESSION' in str(rows))
    check('ordinary DESC with file SQL disabled','DESC SELECT 42',predicate=lambda rows:'EXPRESSION' in str(rows))
    query('SET GLOBAL secure_file_priv=%s',(str(directory),))
    # Global settings are inherited by a new connection, not the setup session.
    connection.close();connection=connect()
    query('USE file_sql_regression')
    query('SET SESSION enable_file_sql=ON')
    check('ordinary DESCRIBE with file SQL enabled','DESCRIBE SELECT 42',predicate=lambda rows:'EXPRESSION' in str(rows))
    check('CSV original fixture',f'SELECT id,name,score,active,day,created,REPLACE(note,CHAR(10),\'<NL>\'),id__2 FROM {csv("basic.csv")} f ORDER BY id',
          [[1,'Alice',98.5,1,'2026-08-01','2026-08-01 10:11:12','hello,<NL>world',101],
           [2,'Bob',87.0,0,None,None,'plain',102],
           [3,'Chen',90.25,1,'2026-08-03','2026-08-03 12:13:14','say "hi"',103]])
    check('CSV aggregate',f'SELECT COUNT(*),SUM(id),ROUND(AVG(score),2),SUM(active) FROM {csv("basic.csv",",header=true,sample_size=-1")}',[[3,6,91.92,2]])
    check('CSV syntax rejects positional format',f'SELECT * FROM {csv("basic.csv",CSV_XML)}',error=1064)
    check('sample size only full scan',f'SELECT * FROM {csv("basic.csv",",sample_size=10")}',error=1235)
    check('unknown named option rejected',f'SELECT * FROM {csv("basic.csv",",wrong=true")}',error=True)
    check('JSON union missing fields',f'SELECT id,v,extra FROM read_ndjson({path("union.jsonl")}) ORDER BY id',[[1,1.0,None],[2,None,'late'],[3,2.5,None]])
    check('JSON original fixture',f'SELECT id,name,score,active,day,note FROM read_json({path("basic.jsonl")},format=\'newline_delimited\') ORDER BY id',[[1,'Alice',98.5,1,None,None],[2,'Bob',None,0,'2026-08-02',None],[3,'Chen',90.0,1,None,'ok']])
    check('Parquet count and aggregate',f'SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) FROM read_parquet({path("simple.parquet")})',[[7,0,6,21]])
    check('dynamic schema describe',f'DESCRIBE SELECT * FROM {csv("basic.csv")}',predicate=lambda rows:[r[:3]for r in rows]==[['id','BIGINT','NO'],['name','VARCHAR','NO'],['score','DOUBLE','NO'],['active','BOOLEAN','NO'],['day','DATE','YES'],['created','DATETIME','YES'],['note','VARCHAR','NO'],['id__2','BIGINT','NO']])
    check('all NULL schema',f'DESCRIBE SELECT * FROM {csv("nulls.csv")}',predicate=lambda rows:rows[1][:3]==['n','NULL','YES'])
    check('WHERE ORDER BY LIMIT expression alias',f'SELECT id+10 AS shifted,name FROM {csv("basic.csv")} AS x WHERE id>=2 ORDER BY score DESC LIMIT 1',[[13,'Chen']])
    check('GROUP BY HAVING',f'SELECT g,COUNT(*),MIN(id),MAX(id),SUM(value),AVG(value) FROM {csv("large.csv")} GROUP BY g HAVING COUNT(*)>2000 ORDER BY g',
          [[g,2500,g,9996+g,6247500+1250*g,2499+0.5*g]for g in range(4)])
    check('INNER JOIN two files',f'SELECT a.id,b.category FROM {csv("basic.csv")} a INNER JOIN {csv("join.csv")} b ON a.id=b.id ORDER BY a.id',[[1,'A'],[3,'C']])
    check('LEFT JOIN NULL side',f'SELECT a.id,b.category FROM {csv("basic.csv")} a LEFT JOIN {csv("join.csv")} b ON a.id=b.id ORDER BY a.id',[[1,'A'],[2,None],[3,'C']])
    check('cross format JOIN',f'SELECT a.id,b.name FROM {csv("basic.csv")} a JOIN read_json({path("basic.jsonl")}) b ON a.id=b.id ORDER BY a.id',[[1,'Alice'],[2,'Bob'],[3,'Chen']])
    check('native JOIN plan',f'EXPLAIN SELECT a.id FROM {csv("basic.csv")} a JOIN {csv("join.csv")} b ON a.id=b.id',predicate=lambda rows:'JOIN'in str(rows).upper()and'FUNCTION'in str(rows).upper())
    check('empty CSV with header',f'SELECT COUNT(*) FROM {csv("empty.csv")}',[[0]])
    check('NULL count and expression',f'SELECT COUNT(n),COUNT(*),SUM(id) FROM {csv("nulls.csv")}',[[0,2,3]])
    check('custom delimiter header and null',f'SELECT column0,column1 FROM {csv("custom.csv",CSV_CUSTOM)} ORDER BY column0',[[1,''],[2,None]])
    for name,expected in [('basic.csv',[[3]]),('alternate.csv',[[1]]),('basic.csv',[[3]])]:
        check('plan parameter isolation '+name,f'SELECT COUNT(*) FROM {csv(name)}',expected)
    check('CSV malformed row rejected',f'SELECT * FROM {csv("bad.csv")}',error=True)
    check('JSON nested object rejected',f'SELECT * FROM read_json({path("nested.jsonl")})',error=True)
    check('path traversal denied',f'SELECT * FROM read_csv({connection.escape(str(directory/"../outside.csv"))})',error=True)
    outside=directory.parent/(directory.name+'-outside.csv');outside.write_text('id\n99\n',encoding='utf-8')
    link=directory/'escape.csv'
    if link.exists()or link.is_symlink():link.unlink()
    link.symlink_to(outside)
    check('symlink escape denied',f'SELECT * FROM {csv("escape.csv")}',error=True)
    link.unlink();outside.unlink()
    exported=directory/'exported.csv'
    if exported.exists():exported.unlink()
    check('COPY query native export',f'COPY (SELECT id,name,score FROM {csv("basic.csv")} ORDER BY id) TO {path("exported.csv")} (FORMAT CSV,HEADER,NULLSTR \'NULL\')')
    check('COPY read back',f'SELECT * FROM {csv("exported.csv")} ORDER BY id',[[1,'Alice',98.5],[2,'Bob',87.0],[3,'Chen',90.25]])
    check('COPY existing target fails',f'COPY (SELECT 1 AS value) TO {path("exported.csv")} (HEADER)',error=True)
    query('DROP TABLE IF EXISTS file_copy_input');query('CREATE TABLE file_copy_input(id BIGINT,name VARCHAR(64))');query("INSERT INTO file_copy_input VALUES(1,'quoted, value'),(2,NULL)")
    table_export=directory/'table-export.csv'
    if table_export.exists():table_export.unlink()
    check('COPY table with options',f'COPY file_copy_input TO {path("table-export.csv")} (HEADER,DELIMITER \';\',NULLSTR \'NULL\')')
    check('COPY table read back',f'SELECT * FROM {csv("table-export.csv",CSV_SEMICOLON)} ORDER BY id',[[1,'quoted, value'],[2,None]])
    failed=directory/'failed-export.csv'
    if failed.exists():failed.unlink()
    check('COPY query failure',f'COPY (SELECT CAST(\'invalid\' AS JSON) AS bad) TO {path("failed-export.csv")} (HEADER)',error=True)
    records.append({'name':'failed COPY leaves no target or temporary','passed':not failed.exists()and not list(directory.glob('.seekdb-copy-*'))})
    check('file discovery',f'SELECT COUNT(*) FROM glob({connection.escape(str(directory/"*.jsonl"))})',[[3]])
    query('SET SESSION enable_file_sql=OFF')
    check('disable affects subsequent calls',f'SELECT COUNT(*) FROM {csv("basic.csv")}',error=1235)
    check('ordinary SQL after failures','SELECT 9*9',[[81]])
finally:
    connection.close();args.output.parent.mkdir(parents=True,exist_ok=True)
    result={'passed':sum(r['passed']for r in records),'total':len(records),'data_dir':str(directory),'cases':records}
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2,default=str),encoding='utf-8')
    print('SQL checks:',result['passed'],'/',result['total'],flush=True)
raise SystemExit(0 if records and all(r['passed']for r in records)else 1)
