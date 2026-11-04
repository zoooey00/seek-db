#!/usr/bin/env python3
"""Compare full FTS results on a dedicated benchmark instance.

Reads fts_large_bench and recreates the fts_hotpath_regression database to test
INSERT/UPDATE/DELETE index maintenance. Run only on a dedicated test instance.
"""
import argparse
import itertools
import json
import pathlib
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--mysql', required=True, help='MySQL client command with connection arguments')
p.add_argument('--output', type=pathlib.Path, required=True)
p.add_argument('--reference', type=pathlib.Path)
a = p.parse_args()
client = shlex.split(a.mysql)
def literal(text):
    return "CONVERT(X'"+text.encode('utf-8').hex()+"' USING utf8mb4)"

texts = ['', ' ', 'OceanBase 数据库 全文索引', '中华人民共和国 数据检索',
         '数据库数据库全文全文', 'ＡＢＣ１２３ abc123', 'abc-def a.b x@y.com',
         '一百二十三 123.45公斤 三十二个', '日本語 ひらがな カタカナ 한글',
         '𠀀𠮷 😀🙂🚀', 'The QUICK brown fox and the dogs', 'a\x00b\n数据库\t索引',
         'é É Å Straße 中文', '数据库 ' * 100]
configs = [None, [], [{'output':'all'}],
           [{'additional_args':[{'ik_mode':'smart'}]}, {'output':'all'}],
           [{'additional_args':[{'ik_mode':'max_word'}]}, {'output':'all'}],
           [{'additional_args':[{'min_token_size':1}]}],
           [{'additional_args':[{'max_token_size':16}]}]]
cases = []
for text, parser, config in itertools.product(texts, ['ik','beng','space','ngram','ngram2'], configs):
    args = [literal(text), literal(parser)]
    if config is not None: args.append(literal(json.dumps(config)))
    cases.append('SELECT TOKENIZE('+','.join(args)+')')
for parser in ['ik','beng']:
    cases.append('SELECT TOKENIZE(NULL,'+literal(parser)+')')
required_start = len(cases)

# fts_large_bench is created by the unchanged official full benchmark first.
for cols, terms in [('title,content', ['数据库', '全文 索引', 'OceanBase', '数据库 OceanBase', '不存在的词zzzz']),
                    ('content', ['数据库', '索引 查询', 'CPU']),
                    ('title_en,content_en', ['database','index search','the','missingzzzz'])]:
    for term, boolean in itertools.product(terms, [False,True]):
        score = 'MATCH('+cols+') AGAINST('+literal(term)+(' IN BOOLEAN MODE' if boolean else '')+')'
        for order in ['ORDER BY id','ORDER BY relevance DESC,id LIMIT 20','ORDER BY relevance DESC,id LIMIT 3 OFFSET 2']:
            cases.append('SELECT id,'+score+' AS relevance FROM fts_large_bench.docs WHERE '+score+' '+order)
# Exercise the same tokenizer through incremental index maintenance as well as
# ALTER TABLE index construction. Only this dedicated regression database is
# written; the official benchmark SQL and dataset remain unchanged.
db = 'fts_hotpath_regression'
documents={i:dict(cn='数据库 全文 索引 OceanBase '+('新词' if i % 2 else '检索'),
                  en='Database index fox '+('updated' if i % 2 else 'search')) for i in range(1,21)}
expected_ids={}
cases += ['DROP DATABASE IF EXISTS '+db, 'CREATE DATABASE '+db,
          'CREATE TABLE '+db+'.docs (id INT PRIMARY KEY,cn TEXT,en TEXT) DEFAULT CHARSET=utf8mb4',
          'ALTER TABLE '+db+'.docs ADD FULLTEXT INDEX f_cn(cn) WITH PARSER ik',
          'ALTER TABLE '+db+'.docs ADD FULLTEXT INDEX f_en(en) WITH PARSER beng',
          'INSERT INTO '+db+'.docs VALUES '+','.join(
              '('+str(i)+','+literal('数据库 全文 索引 OceanBase '+('新词' if i % 2 else '检索'))+','+
              literal('Database index fox '+('updated' if i % 2 else 'search'))+')' for i in range(1,21))]
for phase in range(3):
    if phase == 1:
        cases.append('UPDATE '+db+'.docs SET cn='+literal('新词 数据库')+',en='+literal('updated database')+' WHERE id <= 5')
        for i in range(1,6): documents[i]=dict(cn='新词 数据库',en='updated database')
    if phase == 2:
        cases.append('DELETE FROM '+db+'.docs WHERE id % 3 = 0')
        documents={i:doc for i,doc in documents.items() if i % 3 != 0}
    for col,terms in [('cn',['数据库','新词','OceanBase']),('en',['database','updated','fox'])]:
        for term in terms:
            score='MATCH('+col+') AGAINST('+literal(term)+')'
            # These fixtures contain the search terms as separate words. Check
            # IDs against the deliberately constructed documents as well as the
            # original executable, so two equally broken versions cannot pass.
            expected_ids[len(cases)]=[i for i,doc in documents.items() if term.casefold() in doc[col].casefold()]
            cases.append('SELECT id,'+score+' AS relevance FROM '+db+'.docs WHERE '+score+' ORDER BY id')
cases.append('DROP DATABASE '+db)
snapshots = []
for sql in cases:
    result = subprocess.run(client+['-e','SET NAMES utf8mb4; '+sql], capture_output=True, timeout=120)
    snapshots.append(dict(sql=sql, code=result.returncode,
                          stdout=result.stdout.decode('utf-8',errors='strict'),
                          stderr=result.stderr.decode('utf-8',errors='replace')))
a.output.write_text(json.dumps(snapshots,ensure_ascii=False,indent=2)+'\n')
assert sum(s['code']==0 for s in snapshots[:len(texts)*5*len(configs)]) > 200, 'Insufficient successful TOKENIZE cases'
assert all(s['code']==0 for s in snapshots[required_start:]), 'MATCH or index maintenance regression SQL failed'
for i, ids in expected_ids.items():
    actual_ids=[int(row.split('\t',1)[0]) for row in snapshots[i]['stdout'].splitlines()]
    assert actual_ids==ids, 'Incorrect incremental index result: '+snapshots[i]['sql']
if a.reference:
    reference = json.loads(a.reference.read_text())
    diffs = [i for i,(x,y) in enumerate(zip(reference,snapshots)) if x != y]
    assert len(reference)==len(snapshots) and not diffs, 'Result differences: '+repr(diffs[:20])
print('PASS',len(snapshots),'complete SQL outputs and error results')
