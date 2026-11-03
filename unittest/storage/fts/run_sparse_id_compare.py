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

"""Compile/run the sparse-ID comparator oracle using a completed seekdb build."""
import argparse,pathlib,json,shlex,subprocess
p=argparse.ArgumentParser();p.add_argument('--build-dir',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
repo=pathlib.Path(__file__).resolve().parents[3];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);build=a.build_dir.resolve()
commands=json.loads((build/'compile_commands.json').read_text(encoding='utf-8'));t=next(c for c in commands if 'ob_storage_fts' in c['file'] and 'Unity' in c['file']);cmd=shlex.split(t['command'])
cmd[cmd.index('-c')+1]=str(repo/'unittest/storage/fts/test_sparse_id_compare.cpp');cmd[cmd.index('-o')+1]=str(out/'oracle.o')
cmd.insert(1,'-I'+str(repo/'src/oblib/lib/charset'));compiler=pathlib.Path(cmd[0]);toolchain=next(x for x in cmd if x.startswith('--gcc-toolchain='));lib=build/'src/observer/liboceanbase.so'
link=[str(compiler),toolchain,'-fuse-ld='+str(compiler.parent/'ld.lld'),str(out/'oracle.o'),str(lib),'-pthread','-ldl','-lm','-Wl,--threads=2','-Wl,-rpath,'+str(lib.parent),'-Wl,-rpath,'+str(compiler.parent.parent/'lib64'),'-o',str(out/'oracle')]
for label,args in [('compile',cmd),('link',link),('run',[str(out/'oracle')])]:
 with (out/(label+'.log')).open('w') as f:r=subprocess.run(args,cwd=t['directory'],stdout=f,stderr=f)
 print(label,r.returncode,flush=True);print((out/(label+'.log')).read_text(encoding='utf-8')[-3000:],flush=True)
 if r.returncode:raise SystemExit(r.returncode)
