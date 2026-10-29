# IK / BENG 分词优化与验证报告

日期：2026-10-29（Asia/Shanghai）。目标仓库：`zoooey00/seek-db`，分支：`perf/fts-ik-beng-20261029`。

## 结论与限制

本次完成 12 个生产文件的分词改动、新增回归用例，并对最终组合版本重新构建验证。936 项 SQL 比较全部一致，五组 C++ 检查通过，官方 `tokenize_function`、`parser_properties`、`simple_query` 及新增 `parser_hot_path` 通过。

**本机结果未证实稳定的综合性能提升。** 全部四轮固定 CPU 对照中，IK/BENG TOKENIZE 均值耗时分别下降 1.84% / 6.28%，但三项建索引耗时均值上升，混合和 LIMIT 查询也变慢。按官方类别权重计算的本机均值改善为 -2.21%。这些只是有明显环境波动的描述性结果，不代表统计显著的收益或退化。

原版与优化版的八份固定 CPU 对照报告使用原始固定基线评分均为 **100/100**；最终组合版本的单轮得分为 **14.87/100**，TOKENIZE 明显变慢，因此没有证明最终版本可以稳定达到此前成绩。原版已经满分，固定基线与本机环境不同，且分数有上限，不能把此前的满分归因于本次改动，也不能据此宣称较原版提升 50%。全部结果保留，最终版本的低分记录没有剔除。

## 源码与提交范围

- 官方比较基点：`5bce9e06d1281347ad61344e4b01fab9158c58a2`。
- 上传分支父提交：`02fb66c1d41e637a2f4aba7fe050e6028b6f23ef`，即远端已有 `2026-competition`。该分支相较官方基点已有 18 个 AI 增强相关文件；本次全部保留，其内容未作修改。
- 已逐个核对 12 个 FTS 文件：远端父提交中的内容与官方基点相同，整合没有覆盖冲突。
- IK 复用已取得的字符与分类信息；单次 Unicode 解码替代重复解码，保持分类判断顺序；词典小包装使用调用方分配器与内联数组；仲裁链改为有序小数组，保留重复偏移检查。
- BENG 将规范化结果直接写入调用方的词元分配器，省去一次分配和复制，保留词元跨迭代、重置及解析器析构后的有效期约定。
- 修正 IK 从已释放列表单元取值，以及数量词合并后继续访问空列表的问题；补足词典包装初始化失败与析构清理。
- 原始数据生成器、建表 SQL、基准脚本、计时逻辑、评分脚本及固定基线均未修改。Git 对象及 SHA-256 核对见 [submission-integrity.json](evidence/submission-integrity.json)。

## 构建与运行设置

Ubuntu 20.04 / WSL2，Intel i5-12500H，Clang 17.0.6。两版使用同一个 CMake 构建目录及相同配置：`RelWithDebInfo`，`-O2 -g -DNDEBUG`；运行前只去除调试段。服务端 `cpu_count=2`、`memory_limit=2G`，专用端口 2901；固定 CPU 轮次的服务端绑定 `0,2`，客户端绑定 `4`。其他实例未调整。

参数严格采用官方固定基线：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。每轮重新导入全部文档并重建索引，没有修改语料、查询或预期命中数。CPU 采样在单独阶段执行，未混入计分基准。

完整原版与优化版分别生成并留存二进制。第一次增量切换曾因复制文件保留旧时间戳而混用接口，该异常构建已弃用；刷新修改文件时间戳后重新构建，后续回归与全部有效性能记录均采用重编后的程序。最终还将远端已有 AI 文件与本题源码一同放入原配置构建目录，重新编译 `seekdb` 与 `oceanbase` 两个目标，构建成功。最终组合版本的源码与二进制 SHA-256 见 [integrated-source-manifest.json](evidence/integrated-source-manifest.json) / [integrated-binary.json](evidence/integrated-binary.json)。

## 正确性证据

| 检查 | 结果及覆盖 |
|---|---|
| SQL 全量对照 | 936 / 936 一致：536 个 TOKENIZE、396 个 MATCH、4 个正式基准完整命中文档集合 |
| TOKENIZE | 两种 IK 模式、BENG 词频与文档长度；包含其他内置解析器对照、空值、长文本、全角字符、组合字符与补充平面字符 |
| MATCH | 布尔/自然语言、完整文档 ID 列表、相关性值四舍五入至十位小数、带确定性次序的 top-k、增删改与回滚 |
| 正式基准命中数 | 中文 8001、英文 11000、混合 7332、LIMIT 20；八轮对照及最终单轮均一致 |
| 完整谓词集合 | LIMIT 谓词返回的全部 9333 个文档也已比较；并非仅核对返回 20 行 |
| 旧索引兼容性 | 原版索引切换到优化版后，8001 个中文命中文档全部一致；最终组合版本再次比较旧索引完整集合 |
| Unicode 分类 | 3,336,192 个 Unicode 标量/编码组合（UTF-8、UTF-16BE、UTF-16LE）与原实现一致，另检查异常输入 |
| 内存和边界 | 释放型分配器下列表词元生命周期、数量词合并、仲裁数组 0/1/15/16/17/64/513 边界、词典数组 0/1/8/9/64 边界与分配失败、BENG 长词元保留 |

最终结果见 [sql-integrated.result.json](evidence/sql-integrated.result.json)、[cpp-integrated/run.log](evidence/cpp-integrated/run.log)、[mysqltest-integrated/result.json](evidence/mysqltest-integrated/result.json)。

较大的官方用例未全部通过，不能将本次检查描述为“官方全套通过”：

- `simple_query` 在完整原版和最终组合版本均通过，早期预检查失败不再作为最终结论。
- `basic_dml` 在两版均于第 1598 行失败：脚本查不到内部表 ID，将 `No such row` 带入 SQL 后出现 1064 错误。两版具有同样的失败位置及报错。
- `create_fts_index_afterward` 在两版均有结果长度差异，差异仅为预期结果末尾多输出一个空行。原始预期文件未修改。

这些记录保存在 [mysqltest-before-full-extra](evidence/mysqltest-before-full-extra/) 与 [mysqltest-integrated-extra](evidence/mysqltest-integrated-extra/)，作为已存在的测试适配限制保留。

## 性能结果：保留全部轮次

先完成两轮未绑定 CPU 的前后测量，发现连 SELECT 1 都有明显漂移，因此将其作为探索性记录保留在 `before-1/2`、`after-1/2`，未与下表混算。随后执行两组 ABBA 顺序：原版1 → 优化版1 → 优化版2 → 原版2，以及原版3 → 优化版3 → 优化版4 → 原版4。下表使用全部四轮的算术均值，每轮内部仍是原脚本的三次采样。

改善百分比 =（原版均值 − 优化版均值）/ 原版均值；正数表示更快。

| 指标 | 原版均值 | 优化版均值 | 耗时改善 |
|---|---:|---:|---:|
| IK 标题＋正文建索引（秒） | 12.8822 | 14.0615 | -9.15% |
| IK 正文建索引（秒） | 8.5935 | 9.4685 | -10.18% |
| BENG 建索引（秒） | 4.2018 | 4.8093 | -14.46% |
| IK TOKENIZE（毫秒） | 0.3095 | 0.3038 | +1.84% |
| BENG TOKENIZE（毫秒） | 0.2376 | 0.2227 | +6.28% |
| 中文查询（毫秒） | 2.5796 | 2.5484 | +1.21% |
| 英文查询（毫秒） | 3.9831 | 3.3945 | +14.78% |
| 混合查询（毫秒） | 2.8005 | 2.9116 | -3.97% |
| LIMIT 查询（毫秒） | 3.5785 | 3.9254 | -9.69% |

即使绑定 CPU，SELECT 1 均值仍在 0.1292–1.3509 毫秒间变化，优化版第二轮的建索引与 LIMIT 查询也有较大波动。未剔除这些轮次，未作基线扣除，未使用最好一轮替代总体结果，也未作显著性推断。原始报告、标准差与每次采样日志均在 `evidence/pinned-*`；可复核汇总见 [benchmark-summary.json](evidence/benchmark-summary.json)。

最终组合版本另跑一轮正式参数基准作为整合检查，不与此前隔离改动的四轮结果混算。结果：总建索引 44.026 秒，IK/BENG TOKENIZE 1.0870/0.9801 毫秒，原始评分 **14.87/100**，四项命中数正确。见 [integrated-final.txt](evidence/integrated-final.txt) 与 [integrated-final-fixed-score.log](evidence/integrated-final-fixed-score.log)。本轮明显慢于此前对照；WSL 内另有独立实例同时存在，尚不能区分环境干扰与整合差异的影响，不能用此前单独分词版本的满分替代最终版本结果。

独立 CPU 采样中，原版 IK 的字典 DAT 匹配、数组哈希查找和锁开销占据明显位置；字符解码、分配及仲裁也有采样命中。采样量有限，且反映整个服务进程，不能凭函数样本比例宣称某项改动的耗时收益。前后 `*-perf-report.txt` 原始报告均保留。

## 复现

完整构建与回归方法见 [unittest/storage/fts/README.md](../../../unittest/storage/fts/README.md)。C++ 对照使用从官方基点逐字复制的原始分类器夹具，并校验 SHA-256，因此目标仓库的独立导入历史不影响运行。

```bash
python3 unittest/storage/fts/run_fts_cpp.py --build-dir /path/to/build --output /tmp/fts-cpp
# 原版实例上记录；然后换优化版实例比较。请使用专用测试实例。
python3 unittest/storage/fts/check_fts_sql.py --mysql /path/to/client --port 2901 --benchmark-results --write /tmp/reference.json --output /tmp/before
python3 unittest/storage/fts/check_fts_sql.py --mysql /path/to/client --port 2901 --benchmark-results --compare /tmp/reference.json --output /tmp/after
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/bench.txt bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/bench.txt --json
```

基准命令需要按原脚本说明设置 `MYSQL` / `MYSQL_VERBOSE` 客户端连接参数。独立 SQL 测试仅重建 `fts_hot_path_regression`；正式基准重建自己的 `fts_large_bench` 数据库。执行带 `--benchmark-results` 的 SQL 对照前，需先在对应版本上运行正式基准以准备其数据。

`evidence/` 保留可直接阅读的构建命令、配置、程序摘要、全部有效基准与已知失败日志；完整 SQL 快照、生成的 SQL 与早期预检查记录存于 [evidence.zip](evidence.zip)。测试中间对象、数据库文件和二进制不加入 Git。
