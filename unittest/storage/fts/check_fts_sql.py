#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compare complete TOKENIZE/MATCH results with an independently built base.

Run --write baseline.json against the base, then --compare baseline.json against
the optimized server. Only fts_hot_path_regression is dropped/recreated. Pass
--benchmark-results after the unchanged large benchmark to record full ID sets.
Requires Python 3 and a MySQL-compatible command-line client; no pip packages.
"""
import argparse
import hashlib
import json
import pathlib
import random
import subprocess


def literal(text):
    return "CONVERT(0x" + text.encode("utf8").hex() + " USING utf8mb4)" if text else "''"


def canonical(value):
    if isinstance(value, list):
        return sorted((canonical(x) for x in value), key=lambda x: json.dumps(x, sort_keys=True))
    if isinstance(value, dict):
        return {k: canonical(v) for k, v in sorted(value.items())}
    return value


def make_sql(benchmark_results):
    rng = random.Random(20261029)
    edges = ["", " ", "!!!", "alpha ALPHA Alpha beta", "a an the AND or", "12 123 12.5 1,000",
             "数据库全文索引分词器", "中华人民共和国人民大会堂", "三百五十个苹果 110个星球",
             "ＡＢＣ１２３ ａｂｃ", "ｶﾀｶﾅ カタカナ ひらがな 한글", "😀🚀中文𠀀abc", "café Straße İSTANBUL",
             "abc-def foo@bar.com C++ abc_def x.y", "a\tb\nc\rd", "abc\x00def", "e\u0301 É é",
             "X" * 84, "Y" * 85, "Z" * 4096,
             "数据库 索引 alpha beta 123.5千米。" * 180]
    pieces = ["数据库", "全文索引", "人民", "分词器", "缓存", "alpha", "BETA", "OceanBase",
              "database", "performance", "audit", "the", "１２３", "3.14", "三十个", "😀", "。", ",", "!", " ", "-"]
    texts = edges + ["".join(rng.choice(pieces) for _ in range(rng.randrange(1, 32))) for _ in range(96)]
    sql = ["SET NAMES utf8mb4; SET ob_query_timeout=100000000000; SET ob_trx_timeout=100000000000;",
           "DROP DATABASE IF EXISTS fts_hot_path_regression; CREATE DATABASE fts_hot_path_regression; USE fts_hot_path_regression;"]
    expected_keys = []

    def token_case(key, expression):
        sql.append("SELECT '%s', %s;" % (key, expression))
        expected_keys.append(key)

    for parser in ["ik", "beng", "space", "ngram", "ngram2"]:
        for i, text in enumerate(texts):
            if parser not in ("ik", "beng") and i >= len(edges):
                continue
            key = "T:%s:%d" % (parser, i)
            token_case(key, "TOKENIZE(%s,'%s','[{\"output\":\"all\"}]')" % (literal(text), parser))
        token_case("T:%s:null" % parser, "TOKENIZE(CAST(NULL AS CHAR CHARACTER SET utf8mb4),'%s')" % parser)
    for mode in ("smart", "max_word"):
        for i, text in enumerate(texts):
            key = "T:ik:%s:%d" % (mode, i)
            opts = json.dumps([{"output": "all"}, {"additional_args": [{"ik_mode": mode}]}])
            token_case(key, "TOKENIZE(%s,'ik','%s')" % (literal(text), opts))

    queries = ["全文索引 分词器", "OceanBase 稳定 database", "数据库", "database performance audit",
               "ALPHA beta", "人民", "１２３", "nonexistentwordzz", "", "alpha alpha", "三十个"]
    for table, parser, props in [("ik_smart", "ik", "PARSER_PROPERTIES=(ik_mode='smart')"),
                                  ("ik_max", "ik", "PARSER_PROPERTIES=(ik_mode='max_word')"),
                                  ("beng", "beng", "")]:
        sql.append("CREATE TABLE %s(id BIGINT PRIMARY KEY,title VARCHAR(256),body LONGTEXT) DEFAULT CHARSET=utf8mb4;" % table)
        for i, text in enumerate(texts):
            sql.append("INSERT INTO %s VALUES(%d,%s,%s);" % (table, i, literal(text[:60]), literal(text)))
        sql.append("ALTER TABLE %s ADD FULLTEXT INDEX fti(title,body) WITH PARSER %s %s;" % (table, parser, props))
        for phase in ("built", "updated", "rollback"):
            if phase == "updated":
                sql += ["UPDATE %s SET body='database database 全文索引' WHERE MOD(id,7)=0;" % table,
                        "DELETE FROM %s WHERE MOD(id,11)=0;" % table,
                        "INSERT INTO %s VALUES(10001,'new alpha','alpha 全文索引 performance');" % table]
            if phase == "rollback":
                sql += ["START TRANSACTION;", "UPDATE %s SET body='temporaryword' WHERE id=2;" % table,
                        "DELETE FROM %s WHERE id=3;" % table, "ROLLBACK;"]
            for qi, query in enumerate(queries):
                for mode in ("", " IN BOOLEAN MODE"):
                    match = "MATCH(title,body) AGAINST(%s%s)" % (literal(query), mode)
                    key = "Q:%s:%s:%d:%s" % (table, phase, qi, "boolean" if mode else "natural")
                    expected_keys.append(key)
                    sql.append("SELECT '%s',id,ROUND(%s,10) FROM %s WHERE %s ORDER BY id;" % (key, match, table, match))
                    # Deterministic top-k with an explicit tie breaker validates
                    # ranking and LIMIT independently of a mere hit count.
                    key += ":top"
                    expected_keys.append(key)
                    sql.append("SELECT '%s',id,ROUND(%s,10) FROM %s WHERE %s ORDER BY %s DESC,id LIMIT 7;" % (key, match, table, match, match))
    if benchmark_results:
        for name, cols, query in [("cn", "title,content", "全文索引 分词器"),
                                  ("beng", "title_en,content_en", "database performance audit"),
                                  ("mixed", "title,content", "OceanBase 稳定 database"),
                                  ("limit_full", "content", "倒排索引 tokenizer")]:
            key = "B:" + name
            expected_keys.append(key)
            sql.append("SELECT '%s',id FROM fts_large_bench.docs WHERE MATCH(%s) AGAINST(%s) ORDER BY id;" % (key, cols, literal(query)))
    return "\n".join(sql), expected_keys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mysql", default="mysql", help="client executable path")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=2881)
    p.add_argument("--user", default="root")
    p.add_argument("--benchmark-results", action="store_true")
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", type=pathlib.Path)
    mode.add_argument("--compare", type=pathlib.Path)
    p.add_argument("--output", required=True, type=pathlib.Path, help="result/evidence prefix")
    args = p.parse_args()
    sql, keys = make_sql(args.benchmark_results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".sql").write_text(sql, encoding="utf8")
    command = [args.mysql, "--no-defaults", "--protocol=tcp", "-h"+args.host, "-P"+str(args.port),
               "-u"+args.user, "-N", "-s", "--batch", "--raw", "--default-character-set=utf8mb4"]
    result = subprocess.run(command, input=sql, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf8")
    args.output.with_suffix(".stderr").write_text(result.stderr, encoding="utf8")
    if result.returncode:
        raise SystemExit("SQL regression failed: " + result.stderr[-3000:])
    snapshot = {key: [] for key in keys}
    for line in result.stdout.splitlines():
        key, *columns = line.split("\t")
        if key not in snapshot:
            raise SystemExit("Unexpected output: " + line[:200])
        if key.startswith("T:"):
            snapshot[key].append(canonical(json.loads(columns[0])) if columns[0] != "NULL" else None)
        elif key.startswith("Q:"):
            snapshot[key].append([int(columns[0]), float(columns[1])])
        else:
            snapshot[key].append(int(columns[0]))
    for key in keys:
        if key.startswith("T:") and len(snapshot[key]) != 1:
            raise SystemExit("Missing or repeated TOKENIZE output: " + key)
    # This assertion is independent of the baseline comparison: verify word
    # frequencies, including case folding and deduplication of repeated words.
    repeated = snapshot["T:beng:3"][0]
    expected = {"doc_len": 4, "tokens": [{"alpha": 3}, {"beta": 1}]}
    if repeated != canonical(expected):
        raise SystemExit("Incorrect repeated-word result: " + repr(repeated))
    snapshot_path = args.output.with_suffix(".snapshot.json")
    encoded = json.dumps(snapshot, ensure_ascii=False, sort_keys=True, indent=2)
    snapshot_path.write_text(encoded, encoding="utf8")
    if args.write:
        args.write.write_text(encoded, encoding="utf8")
    differences = []
    if args.compare:
        baseline = json.loads(args.compare.read_text(encoding="utf8"))
        for key in sorted(set(baseline) | set(snapshot)):
            if baseline.get(key) != snapshot.get(key):
                differences.append(key)
    summary = {"passed": not differences, "case_count": len(snapshot),
               "tokenize_cases": sum(k.startswith("T:") for k in snapshot),
               "match_cases": sum(k.startswith("Q:") for k in snapshot),
               "benchmark_result_counts": {k: len(v) for k,v in snapshot.items() if k.startswith("B:")},
               "compared_full_results": bool(args.compare), "different_cases": differences,
               "match_relevance_decimal_places": 10,
               "snapshot_sha256": hashlib.sha256(encoded.encode()).hexdigest(),
               "sql_sha256": hashlib.sha256(sql.encode()).hexdigest()}
    args.output.with_suffix(".result.json").write_text(json.dumps(summary, indent=2), encoding="utf8")
    print(json.dumps(summary, indent=2))
    raise SystemExit(1 if differences else 0)


if __name__ == "__main__":
    main()
