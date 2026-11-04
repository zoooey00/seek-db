# 全文索引热路径优化

本次从官方 `oceanbase/seekdb` 的 `2026-competition` 分支重新实现，[固定基准提交](https://github.com/oceanbase/seekdb/commit/5bce9e06d1281347ad61344e4b01fab9158c58a2) 为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。未沿用此前个人仓库中的优化源码、构建产物或测量结果。

## 瓶颈与修改

1. IK 的 `TokenizeContext::prepare_next_char → ObFTCharUtil::classify_first_char → do_classify` 调用链在依次检查英文字母、数字、中文和其他 CJK 时重复解码同一输入。现在仅解码一次，继续调用原有 Unicode 谓词，保留判断顺序、UTF16 代理项限制及解码失败行为。
2. 英文 whitespace tokenizer 的两个扫描循环原先逐字符调用 `ob_mbcharlen_ptr`，再分派到字符集的 `mbcharlen` 函数。现在只对 UTF8 的 ASCII 字节直接取长度 1；非 ASCII、异常多字节输入及其他字符集仍走原函数。分隔符判断和 token 输出流程保持原样。

修改没有引入输出缓存，没有改变词典、词频、停用词、排序、持久化格式或检索规则。定位依据为上述源码调用链，实际收益以完整基准为准；未将源码调用次数当作端到端性能结论。

## 正确性

`unittest/storage/fts/run_ik_classification.py` 从固定官方提交读取原头文件、whitespace 类及原实现，重命名后与候选直接对照，不另写简化算法充当 oracle。字符集在测试开始时显式初始化。

```text
PASS: 3945734 exact classification/result-code comparisons
PASS: 604296 complete whitespace token streams
```

覆盖 UTF8 全部 Unicode 码点、所有两字节组合、截断/随机异常输入、空指针及不支持的排序规则返回值。该官方提交仅保留 utf8mb4_general_ci、utf8mb4_bin、utf8mb4_unicode_ci、binary 四种排序规则，完整 token 流覆盖这四种；UTF16 输入的分类测试核对的是“不支持”错误行为，不能视为已验证 UTF16 分词。截断输入分配可读 padding，逻辑长度不变，用于安全对照原处理器的历史字宽返回行为。

每轮完整基准之后，另运行 591 项完整 SQL 输出及错误结果对照。其中包括基准库的 72 项 MATCH 完整 ID、未取整相关度、确定性排序及分页输出；专用回归库另验证建索引后 INSERT、UPDATE、DELETE 的增量维护，并核对手工构造数据的完整预期文档 ID。分词用例包括中英文、全角字符、补充平面字符、空文本和分词配置。八轮输出一致，SHA256 见 [验证清单](fts-hotpath-evidence/validation-manifest.json)。每轮含 394 项成功语句、197 项原版同样拒绝的错误用例；没有将错误执行当作成功查询。未以命中数替代完整结果验证。

## 测试条件与结果

全新 Linux 原生构建目录；Clang 17、Release / RelWithDebInfo、`-O2`、Unity batch size 10，构建并行 2。优化前后配置一致。原生 MariaDB 10.3.39 客户端；实例配置 memory_limit=3G、cpu_count=4、datafile_size=1G、datafile_maxsize=4G、log_disk_size=4G。每轮新实例和新报告，编译结束后才开始测量。

分两组独立测试，每组顺序均为原版、候选、候选、原版，共八轮。固定 20000 篇文档、500 行批次、3000 次分词、200 次查询、3 组采样、30 次预热、SKIP_LOAD=0。基准脚本、数据生成器、SQL、计时、评分脚本和基线文件均保持官方字节内容；[完整性哈希](fts-hotpath-evidence/benchmark-integrity.json) 可核对。

原版 ELF 在应用候选修改前单独保存。八轮使用同样两份已验证 ELF，执行版本由 LABEL 与二进制 SHA256 对应；报告中的 git_head/git_dirty 是基准脚本读取的源码工作区状态，不能用它替代实际执行 ELF 的身份。生产源码差异已核对为上面两处，源码/ELF 哈希均见验证清单。启动参数排错及回归用例调试产生的预检记录保留在本地，不混入这八轮。

提交源码统一为 LF 换行后重新编译，剥离调试信息的候选 ELF 与八轮实际执行文件逐字节相同，并再次通过原生对照；[一致性结果](fts-hotpath-evidence/normalization-result.json) 记录两次相同的 SHA256。验证清单中的源码哈希对应 Git 实际提交的字节内容。

下表为同机四轮原版和四轮候选各自的算术平均耗时；正提升率代表耗时下降。构建单位为秒，分词和查询单位为毫秒。

| 指标 | 官方原版 | 候选 | 耗时下降 |
|---|---:|---:|---:|
| build_ik_all_sec | 7.251250 | 7.241250 | +0.14% |
| build_ik_content_sec | 5.777000 | 5.647500 | +2.24% |
| build_beng_en_sec | 3.091250 | 3.017500 | +2.39% |
| tokenize_ik_avg_ms | 0.166650 | 0.155225 | +6.86% |
| tokenize_beng_avg_ms | 0.114400 | 0.111125 | +2.86% |
| query_cn_avg_ms | 1.512625 | 1.510150 | +0.16% |
| query_beng_avg_ms | 2.191400 | 2.243250 | -2.37% |
| query_mixed_avg_ms | 1.689125 | 1.741675 | -3.11% |
| query_limit_avg_ms | 1.981650 | 2.029850 | -2.43% |

类别平均：build +1.59%、tokenize +4.86%、query -1.94%。三类等权综合耗时下降 **+1.50%**。负值保留，没有将退化项截断。单次短测和微基准不能替代上述完整测量。

这些是本机相对官方原版的结果，不是统一评测平台的正式成绩，也不证明达到满分。正式成绩须在统一环境以固定基线评测。

两组各自综合耗时下降分别为 **+2.28%**、**+0.62%**。整体收益有限，查询类别两组变化方向不同，合并后均值存在退化；不能将表中全部波动归因于代码，也不能认为已获得稳定的大幅提速。独立分类微基准六组交替执行原版/候选，checksum 一致，显示局部解码热路径收益；该微基准未与整库测量并行，不参与评分，见 [原始微基准](fts-hotpath-evidence/microbench.txt)。

原始八轮完整报告：[block1/1-baseline](fts-hotpath-evidence/block1/1-baseline.txt)、[block1/2-candidate](fts-hotpath-evidence/block1/2-candidate.txt)、[block1/3-candidate](fts-hotpath-evidence/block1/3-candidate.txt)、[block1/4-baseline](fts-hotpath-evidence/block1/4-baseline.txt)、[block2/1-baseline](fts-hotpath-evidence/block2/1-baseline.txt)、[block2/2-candidate](fts-hotpath-evidence/block2/2-candidate.txt)、[block2/3-candidate](fts-hotpath-evidence/block2/3-candidate.txt)、[block2/4-baseline](fts-hotpath-evidence/block2/4-baseline.txt)；[九项指标及分类计算](fts-hotpath-evidence/summary.json)。

## 复现回归

在完成官方依赖初始化及 Release 配置后：

```bash
cmake --build build_release --target seekdb oceanbase -j2
python3 unittest/storage/fts/run_ik_classification.py --build-dir build_release
```

在专用实例运行未修改的 `tools/benchmark/fts_large_bench.sh` 完整基准后，可使用 `check_sql_results.py --mysql '<标准客户端连接命令>' --output original.json` 保存原版快照，在候选实例追加 `--reference original.json` 对照。此脚本读取基准库，另删除并重建 `fts_hotpath_regression` 专用回归库以测试增量维护，不修改评分 SQL 或基准库数据。
