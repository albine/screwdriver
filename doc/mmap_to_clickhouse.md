# mmap_to_clickhouse - 高性能行情数据导出工具

## 概述

`mmap_to_clickhouse` 是一个 C++ 编写的高性能工具，用于将 mmap 格式的行情二进制文件导出为 TSV 格式，通过管道导入 ClickHouse。

**性能对比：**

| 工具 | 1M 记录耗时 | 吞吐量 | 加速比 |
|------|------------|--------|--------|
| Python | ~24 秒 | ~42K 条/秒 | 1x |
| C++ (1 线程) | 0.79 秒 | ~1.3M 条/秒 | 30x |
| C++ (16 线程) | 0.07 秒 | ~14M 条/秒 | **333x** |

实测 3000 万条 orders 记录：C++ 仅需 **2 秒**，Python 需要 **12 分钟**。

---

## 构建

### 本地开发构建
```bash
./build.sh mmap_to_clickhouse
```

### 生产环境构建（CentOS 7）
```bash
./docker-build.sh
```

构建产物：`build/mmap_to_clickhouse`

---

## 命令行用法

```
mmap_to_clickhouse [options] <bin_file>

选项：
  --type TYPE      记录类型：orders | transactions | ticks
                   （不指定时自动检测 magic）
  --threads N      线程数（默认 16）
  --limit N        限制记录数（调试用）
  -h, --help       显示帮助
```

---

## 使用示例

### 1. 导入 Orders 数据
```bash
./bin/mmap_to_clickhouse --type orders /path/to/orders.bin | \
    clickhouse-client --password "$CLICKHOUSE_PASSWORD" \
    --query "INSERT INTO MDOrderStruct FORMAT TabSeparated"
```

### 2. 导入 Transactions 数据
```bash
./bin/mmap_to_clickhouse --type transactions /path/to/transactions.bin | \
    clickhouse-client --password "$CLICKHOUSE_PASSWORD" \
    --query "INSERT INTO MDTransactionStruct FORMAT TabSeparated"
```

### 3. 导入 Ticks 数据
```bash
./bin/mmap_to_clickhouse --type ticks /path/to/ticks.bin | \
    clickhouse-client --password "$CLICKHOUSE_PASSWORD" \
    --query "INSERT INTO MDStockStruct FORMAT TabSeparated"
```

### 4. 测试导出（不导入）
```bash
# 导出前 100 条记录到文件
./bin/mmap_to_clickhouse --type orders --limit 100 /path/to/orders.bin > test.tsv

# 查看输出
head -5 test.tsv
```

### 5. 调整线程数
```bash
# 单线程（适合小文件或调试）
./bin/mmap_to_clickhouse --type orders --threads 1 /path/to/orders.bin

# 32 线程（适合高核心数服务器）
./bin/mmap_to_clickhouse --type orders --threads 32 /path/to/orders.bin
```

---

## 批量导入脚本

使用封装脚本 `script/import_mmap_fast.sh` 可以一键导入整个目录：

```bash
./script/import_mmap_fast.sh /path/to/data/raw/2026/01/26 \
    --password "$CLICKHOUSE_PASSWORD" \
    --threads 16
```

**脚本选项：**
- `--password PWD` - ClickHouse 密码
- `--host HOST` - ClickHouse 主机（默认 localhost）
- `--database DB` - 数据库名（默认 default）
- `--threads N` - 线程数（默认 16）
- `--only TYPE` - 只导入指定类型
- `--limit N` - 限制记录数
- `--dry-run` - 只解析不导入

---

## 支持的文件类型

| 文件名 | 类型参数 | 目标表 | 记录大小 | Magic |
|--------|----------|--------|----------|-------|
| orders.bin | `orders` | MDOrderStruct | 144 bytes | 0x4F524432 |
| transactions.bin | `transactions` | MDTransactionStruct | 136 bytes | 0x54584E32 |
| ticks.bin | `ticks` | MDStockStruct | 2216 bytes | 0x54494B32 |

---

## 输出格式

工具输出 TSV（Tab-Separated Values）格式到 stdout，日志信息输出到 stderr。

**Orders TSV 列顺序（17 列）：**
```
local_recv_timestamp, orderindex, orderprice, orderqty, orderno, tradedqty, applseqnum,
mddate, mdtime, securityidsource, securitytype, ordertype, orderbsflag, channelno,
datamultiplepowerof10, htscsecurityid, securitystatus
```

**Transactions TSV 列顺序（17 列）：**
```
local_recv_timestamp, tradeindex, tradebuyno, tradesellno, tradeprice, tradeqty, trademoney,
applseqnum, mddate, mdtime, securityidsource, securitytype, tradetype, tradebsflag,
channelno, datamultiplepowerof10, htscsecurityid
```

**Ticks TSV 列顺序（46 列）：**
```
local_recv_timestamp, datatimestamp, maxpx, minpx, preclosepx, numtrades, totalvolumetrade,
totalvaluetrade, lastpx, openpx, closepx, highpx, lowpx, totalbuyqty, totalsellqty,
weightedavgbuypx, weightedavgsellpx, withdrawbuynumber, withdrawbuyamount, withdrawbuymoney,
withdrawsellnumber, withdrawsellamount, withdrawsellmoney, totalbuynumber, totalsellnumber,
buypricequeue[10], buyorderqtyqueue[10], sellpricequeue[10], sellorderqtyqueue[10],
buyorderqueue[50], sellorderqueue[50], buynumordersqueue[50], sellnumordersqueue[50],
mddate, mdtime, securityidsource, securitytype, numbuyorders, numsellorders, channelno,
datamultiplepowerof10, buyorderqueue_count, sellorderqueue_count, buynumordersqueue_count,
sellnumordersqueue_count, htscsecurityid, tradingphasecode
```

---

## 验证数据正确性

### 与 ClickHouse 已有数据对比
```bash
# 导出 C++ 结果
./bin/mmap_to_clickhouse --type orders --limit 1000 orders.bin > /tmp/cpp.tsv

# 查询 ClickHouse（数值字段）
clickhouse-client --password "$CLICKHOUSE_PASSWORD" --query "
    SELECT local_recv_timestamp, orderindex, orderprice, orderqty, orderno,
           tradedqty, applseqnum, mddate, mdtime, securityidsource, securitytype,
           ordertype, orderbsflag, channelno, datamultiplepowerof10
    FROM MDOrderStruct
    WHERE mddate = 20260126
    ORDER BY local_recv_timestamp
    LIMIT 1000
    FORMAT TabSeparated
" > /tmp/ch.tsv

# 对比（排序后）
cut -f1-15 /tmp/cpp.tsv | sort | md5sum
sort /tmp/ch.tsv | md5sum
```

---

## 注意事项

1. **字符串处理**：C++ 工具会在 `\0` 处截断字符串，输出更干净
2. **内存使用**：每个线程约占用 100-200MB 内存（取决于记录数）
3. **磁盘 I/O**：使用 mmap 读取，操作系统会自动管理页面缓存
4. **错误处理**：Magic 不匹配时会报错退出

---

## 相关文件

- 源码：`src/mmap_to_clickhouse.cpp`
- 结构体定义：`include/market_data_structs_aligned.h`
- 封装脚本：`script/import_mmap_fast.sh`
- Python 版本（旧）：`script/import_mmap_to_clickhouse.py`
