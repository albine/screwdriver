# 部署检查清单

## 问题：配置文件没有部署

### 发现的问题

**Q1**: 股票代码需要写后缀吗？
**A1**: 可写可不写，系统会自动识别

```conf
# 两种写法都支持
600759.SH,PriceLevelVolumeStrategy   # 推荐：明确指定
600759,PriceLevelVolumeStrategy      # 也可以：自动识别为.SH

# 自动识别规则
# 6开头 → .SH（上海）
# 0/2/3开头 → .SZ（深圳）
```

**Q2**: 部署脚本上传配置文件了吗？
**A2**: ❌ 原来的 deploy.sh 没有上传策略配置文件

### 修复前的问题

```bash
# deploy.sh (修复前)
CONFIG_DIR="${SCRIPT_DIR}/fastfish/config/prod"  # ❌ 只上传了华泰网关配置

# 只复制了 htsc-insight-cpp-config.conf
# 没有复制 backtest.conf 和 live.conf
```

**结果：**
- ✅ 华泰网关配置被部署
- ❌ 回测配置文件（backtest.conf）缺失
- ❌ 实盘配置文件（live.conf）缺失
- ❌ 部署后无法运行回测和实盘

### 修复后的改进

```bash
# deploy.sh (修复后)
BUILD_DIR="${SCRIPT_DIR}/build"
LIBS_DIR="${SCRIPT_DIR}/fastfish/libs"
CONFIG_DIR="${SCRIPT_DIR}/fastfish/config/prod"       # 华泰网关配置
STRATEGY_CONFIG_DIR="${SCRIPT_DIR}/config"            # ✅ 新增：策略配置

# 部署时复制所有配置文件
- htsc-insight-cpp-config.conf  # 华泰网关配置
- backtest.conf                 # 回测策略配置
- live.conf                     # 实盘策略配置
```

## 部署检查清单

### 部署前检查

```bash
# 1. 检查所有必需文件是否存在
./deploy.sh --help

# 预检查会验证：
✅ build/engine - 可执行文件
✅ fastfish/libs/*.so - 共享库
✅ fastfish/config/prod/htsc-insight-cpp-config.conf - 网关配置
✅ config/backtest.conf - 回测策略配置
✅ config/live.conf - 实盘策略配置
✅ fastfish/cert/*.pem - 证书文件
```

### 部署流程

```bash
# 方法1：打包并部署（默认）
./deploy.sh

# 方法2：仅打包，不上传
./deploy.sh --pack-only

# 方法3：部署到其他服务器
./deploy.sh --host other-server --dest /path/to/deploy
```

### 部署后验证

```bash
# SSH 到服务器
ssh market-m

# 检查部署结构
cd /home/jiace/project/trading-engine
ls -la

# 应该看到：
trading-engine/
├── bin/
│   └── engine               # ✅ 可执行文件
├── lib/
│   └── *.so                 # ✅ 共享库
├── config/
│   ├── htsc-insight-cpp-config.conf  # ✅ 网关配置
│   ├── backtest.conf        # ✅ 回测配置
│   └── live.conf            # ✅ 实盘配置
├── cert/
│   ├── InsightClientCert.pem
│   └── InsightClientKeyPkcs8.pem
├── logs/                    # ✅ 日志目录
└── run.sh                   # ✅ 启动脚本

# 测试回测模式
./run.sh backtest

# 测试实盘模式（需要先设置环境变量）
export FF_USER="username"
export FF_PASSWORD="password"
export FF_IP="gateway_ip"
export FF_PORT="gateway_port"
export FF_CERT_DIR="local_interface_ip"
./run.sh live
```

## 配置文件管理

### 本地开发环境

```
config/
├── backtest.conf       # 回测配置（开发用）
└── live.conf           # 实盘配置（开发用）
```

### 生产环境建议

```
config/
├── backtest.conf           # 默认回测配置
├── live.conf               # 默认实盘配置
├── live_dev.conf           # 开发环境实盘配置
├── live_test.conf          # 测试环境实盘配置
└── live_prod.conf          # 生产环境实盘配置（谨慎配置）
```

**部署到不同环境：**
```bash
# 开发环境
./run.sh live config/live_dev.conf

# 测试环境
./run.sh live config/live_test.conf

# 生产环境
./run.sh live config/live_prod.conf
```

## 常见问题

### Q1: 部署后提示找不到配置文件？

**检查：**
```bash
# 在服务器上检查
ls -la config/

# 如果缺失，手动复制
scp config/backtest.conf market-m:/home/jiace/project/trading-engine/config/
scp config/live.conf market-m:/home/jiace/project/trading-engine/config/
```

### Q2: 修改配置后需要重新编译吗？

**不需要！** 配置文件是运行时读取的：

```bash
# 1. 修改配置文件
vim config/live.conf

# 2. 重新部署（只需上传配置文件）
scp config/live.conf market-m:/home/jiace/project/trading-engine/config/

# 3. 重启服务
ssh market-m
cd /home/jiace/project/trading-engine
./run.sh live
```

### Q3: 如何快速更新配置？

**方法1：使用 rsync（推荐）**
```bash
rsync -avz config/*.conf market-m:/home/jiace/project/trading-engine/config/
```

**方法2：重新部署**
```bash
./deploy.sh
```

**方法3：Git + 远程拉取**
```bash
# 在服务器上
cd /home/jiace/project/trading-engine
git pull
# 或手动复制新配置
```

## 自动化建议

### 配置文件版本控制

```bash
# 配置文件应该纳入 Git
git add config/*.conf
git commit -m "Update trading config"
git push

# 在服务器上拉取最新配置
ssh market-m "cd /home/jiace/project/trading-engine && git pull"
```

### 配置文件模板

创建配置模板，避免敏感信息泄露：

```bash
# config/live.conf.template
# 实盘配置模板
# 复制此文件为 live.conf 并填写实际股票代码

# 示例配置
# STOCK_CODE,STRATEGY_NAME
002603.SZ,PrintStrategy
```

## 总结

| 问题 | 修复前 | 修复后 |
|------|--------|--------|
| **策略配置** | ❌ 不会部署 | ✅ 自动部署 |
| **回测模式** | ❌ 无法运行 | ✅ 可以运行 |
| **实盘模式** | ❌ 无法运行 | ✅ 可以运行 |
| **部署验证** | ❌ 无提示 | ✅ 预检查 |

**核心改进：**
1. ✅ deploy.sh 现在会上传策略配置文件
2. ✅ 部署前检查配置文件是否存在
3. ✅ 配置文件注释更清晰（后缀可选）
4. ✅ 完整的部署验证流程

**建议：**
- 立即使用新的 deploy.sh 重新部署
- 检查服务器上的配置文件
- 配置文件纳入版本控制
