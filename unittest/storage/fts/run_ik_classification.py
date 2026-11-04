#!/usr/bin/env python3
"""Compare IK classification with the exact official implementation.

Run after building targets seekdb and oceanbase:
  python3 unittest/storage/fts/run_ik_classification.py --build-dir build_release
"""
import argparse
import json
import pathlib
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build-dir', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path)
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
repo = pathlib.Path(__file__).resolve().parents[3]
build = a.build_dir.resolve()
out = (a.output or build/'ik-classification-test').resolve()
out.mkdir(parents=True, exist_ok=True)
base = '5bce9e06d1281347ad61344e4b01fab9158c58a2'
header = 'src/storage/fts/ik/ob_ik_char_util.h'
reference = subprocess.check_output(['git', 'show', base+':'+header], cwd=repo).decode()
reference = reference.replace('ObFTCharUtil', 'ReferenceFTCharUtil').replace(
    '_OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_', '_REFERENCE_IK_CHAR_UTIL_H_')
(out/'reference_ik_char_util.h').write_text(reference)
def official(path):
    return subprocess.check_output(['git', 'show', base+':'+path], cwd=repo).decode()
stream_header = official('src/share/text_analysis/ob_token_stream.h')
stream_source = official('src/share/text_analysis/ob_token_stream.cpp')
decl = stream_header[stream_header.index('class ObTextWhitespaceTokenizer final'):stream_header.index('class ObTokenNormalizer :')]
impl = stream_source[stream_source.index('ObTextWhitespaceTokenizer::ObTextWhitespaceTokenizer()'):stream_source.index('ObTokenNormalizer::ObTokenNormalizer()')]
reference_stream = '#pragma once\n#include "share/text_analysis/ob_token_stream.h"\nnamespace oceanbase { namespace share {\n'+decl+impl+'\n}}\n'
(out/'reference_whitespace.h').write_text(reference_stream.replace('ObTextWhitespaceTokenizer', 'ReferenceWhitespaceTokenizer'))
commands = json.loads((build/'compile_commands.json').read_text())
entry = next(c for c in commands if '/storage/' in c['file'] and c['file'].endswith(('.cpp', '.cxx')))
args = entry.get('arguments') or shlex.split(entry['command'])
filtered = []
i = 0
while i < len(args):
    if args[i] in ('-o', '-MF', '-MT', '-MQ'):
        i += 2
    elif args[i] in ('-c', '-MD', '-MMD') or args[i] == entry['file']:
        i += 1
    else:
        filtered.append(args[i]); i += 1
test = repo/'unittest/storage/fts/test_ik_classification.cpp'
binary = out/'test_ik_classification'
cmd = filtered + ['-I'+str(out), str(test), '-L'+str(build/'src/observer'),
                  '-Wl,-rpath,'+str(build/'src/observer'), '-loceanbase',
                  '-lpthread', '-ldl', '-latomic', '-o', str(binary)]
if a.compile_only:
    cmd = filtered + ['-I'+str(out), '-c', str(test), '-o', str(out/'test.o')]
with (out/'compile.log').open('w') as log:
    subprocess.run(cmd, cwd=entry['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
if a.compile_only:
    print('Test source compilation passed')
    raise SystemExit(0)
with (out/'result.txt').open('w') as log:
    subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
print((out/'result.txt').read_text())
