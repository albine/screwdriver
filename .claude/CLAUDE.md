# Trading Engine Project - Claude Code Guidelines

> **IMPORTANT**: Claude should automatically follow these guidelines when working on this project. This ensures consistent build, test, and deployment processes.

## Project Overview

This is a high-frequency trading engine for China stock market (Shanghai/Shenzhen), supporting:
- **Backtest mode**: Historical data replay with strategy testing
- **Live mode**: Real-time trading via HTSC FastFish gateway
- **Multiple strategies**: Opening Range Breakout, Price Level Volume monitoring, etc.

**Language**: C++17
**Build System**: CMake
**Logging**: Quill (high-performance async logging)
**Market Data**: HTSC FastFish gateway protocol

---

## Build Standards

### CRITICAL: Understand Two Build Scenarios

This project has **TWO different build scenarios**:

| Scenario | Script | Purpose | Output |
|----------|--------|---------|--------|
| **Local Testing** | `./build.sh` | 本地开发和回测测试 | 本地可执行文件（仅限本地运行） |
| **Production Deploy** | `./docker-build.sh` | 生产环境部署 | CentOS 7 兼容的二进制文件 |

**Key Differences**:
- **Local build** (`build.sh`): 快速编译，用于本地回测，但可能因 glibc 版本不兼容无法在生产服务器运行
- **Docker build** (`docker-build.sh`): 在 CentOS 7 容器中编译，确保生产环境兼容性，但编译较慢

### When to Use Which Build

**Use `./build.sh` when:**
- ✅ Developing locally (修改代码后快速验证)
- ✅ Running backtest on local machine (本地回测)
- ✅ Testing strategies locally (测试策略)
- ✅ Quick iteration during development (快速迭代开发)

**Use `./docker-build.sh` when:**
- ✅ Preparing for production deployment (准备生产部署)
- ✅ Building for CentOS 7 server (目标是 CentOS 7 服务器)
- ✅ Final build before deploy (部署前的最终构建)

**DO NOT use raw `cmake` or `make` commands** - always use the appropriate build script.

### Build Targets

| Target | Purpose | When to Build |
|--------|---------|---------------|
| `engine` | Main trading engine | After modifying `src/main.cpp`, `src/strategy/*`, `include/*` |
| `test_logger` | Logger tests | After modifying logging setup |
| `test_fastorderbook` | OrderBook replay | After modifying `src/FastOrderBook.cpp` |
| `test_price_level_strategy` | Strategy tests | After modifying strategy code |
| (all) | Build everything | Use `./build.sh` without target |

### Build Examples

**Local Development (本地开发):**
```bash
# Build main engine for local testing
./build.sh engine

# Build and run tests locally
./build.sh test_fastorderbook
./build/test_fastorderbook

# Clean rebuild (after CMakeLists.txt changes)
./build.sh --clean engine

# Debug build (for debugging with gdb)
./build.sh --debug engine
```

**Production Deployment (生产部署):**
```bash
# Build for CentOS 7 production server
./docker-build.sh

# Then deploy
./deploy.sh
```

### When Claude Should Build (Automatically)

Claude should **automatically build locally** (`./build.sh`) when:
1. ✅ Modifying any `.cpp` or `.h` files (to verify compilation)
2. ✅ Creating new strategy files in `src/strategy/`
3. ✅ Changing `CMakeLists.txt` (use `--clean`)
4. ✅ User asks to "test" or "run backtest"
5. ✅ After fixing compilation errors

Claude should **NOT** build when:
- ❌ Only reading/analyzing code
- ❌ Modifying configuration files (`.conf`, `.py`)
- ❌ Editing documentation
- ❌ User only asks general questions

**Production Build Decision**:
- Claude should **suggest** `./docker-build.sh` only when:
  - User explicitly mentions "deploy" or "production"
  - User asks to prepare for server deployment
- For normal development, **always use** `./build.sh`

---

## Testing Standards

### Running Backtest

**Method 1: Using Python automation script (RECOMMENDED)**
```bash
python run_backtest.py YYYYMMDD
# Example: python run_backtest.py 20260114
```

This script:
- Automatically downloads market data for configured stocks
- Runs the engine in backtest mode
- Provides structured logging

**Method 2: Direct execution**
```bash
# Must set library path first
export LD_LIBRARY_PATH=$PWD/fastfish/libs:$LD_LIBRARY_PATH
./build/engine backtest
```

### Backtest Configuration

- **Config file**: `config/backtest.conf`
- **Format**: `股票代码,策略名称` (one per line)
- **Stock codes**: Auto-detect exchange (6* → SH, others → SZ)
- **Market data**: Downloaded to `market_data/` directory

**Available Strategies**:
- `OpeningRangeBreakoutStrategy`: Opening range breakout
- `PriceLevelVolumeStrategy`: Price level volume monitoring
- `TestStrategy`: OrderBook validation
- `PrintStrategy`: Simple debug strategy

### Log Files

After running backtest, check logs:
- `logs/backtest.log` - System/debug logs
- `logs/backtest_biz.log` - Business logic logs (strategy signals, trades)

---

## Code Modification Guidelines

### Adding a New Strategy

1. Create strategy header: `src/strategy/YourStrategy.h`
2. Implement required methods: `OnOrderBook()`, `OnTransaction()`, etc.
3. Register in `include/strategy_factory.h`:
   ```cpp
   if (strategy_name == "YourStrategy") {
       return std::make_unique<YourStrategy>();
   }
   ```
4. **Build**: `./build.sh engine`
5. **Test**: Add to `config/backtest.conf` and run backtest

### Modifying Existing Code

**Workflow**:
1. Make code changes
2. **Build immediately**: `./build.sh [target]`
3. Fix any compilation errors
4. Run appropriate tests
5. Check logs for correctness

### Key Files and Purposes

| File/Directory | Purpose | Modify When |
|----------------|---------|-------------|
| `src/main.cpp` | Entry point, mode selection | Changing startup logic |
| `src/FastOrderBook.cpp` | OrderBook implementation | Fixing book logic |
| `src/strategy/*.h` | Strategy implementations | Adding/modifying strategies |
| `include/strategy_engine.h` | Strategy lifecycle management | Changing strategy interface |
| `include/history_data_replayer.h` | Data replay engine | Modifying backtest logic |
| `config/backtest.conf` | Backtest stock list | Testing different stocks |
| `config/live.conf` | Live trading config | Production deployment |

### Price Unit Conversion

本项目内部使用整数表示价格（乘以 10000），避免浮点精度问题。

**必须使用 `symbol_utils.h` 中的转换方法**：

```cpp
#include "symbol_utils.h"

// double（元）转 uint32_t（内部格式）
uint32_t price_int = symbol_utils::price_to_int(12.50);  // -> 125000

// uint32_t（内部格式）转 double（元）
double price = symbol_utils::int_to_price(125000);       // -> 12.50
```

**禁止直接使用 `* 10000` 或 `/ 10000.0`**，统一使用上述方法以保持代码一致性。

---

## Logging Standards

### File-Level Module Logging Macros

本项目使用 Quill 高性能异步日志库。**必须使用文件级模块日志宏**，不要直接调用 `LOG_ERROR(logger, ...)` 等底层宏。

**使用方法**：

```cpp
#include "logger.h"

// 1. 在文件开头定义模块名（头文件守卫之后）
#define LOG_MODULE "MyModule"

// 2. 使用 LOG_M_* 宏记录日志
LOG_M_DEBUG("debug message, value={}", value);
LOG_M_INFO("info message");
LOG_M_WARNING("warning message");
LOG_M_ERROR("error message: {}", error_msg);

// 3. 在文件末尾取消定义（#endif 之前）
#undef LOG_MODULE
```

**输出格式**：
```
2026-01-15 09:30:00.123456789 [INFO] [12345] [MyModule] info message
```

**预定义模块名**（在 `logger.h` 中）：
- `MOD_ENGINE` - "Engine"
- `MOD_ORDERBOOK` - "OrderBook"
- `MOD_STRATEGY` - "Strategy"
- `MOD_GATEWAY` - "Gateway"
- `MOD_MARKET` - "Market"

**业务日志**（交易相关）：
```cpp
// 使用 LOG_BIZ 宏，自动写入业务日志文件
LOG_BIZ(BIZ_ORDR, "下单 symbol={} price={}", symbol, price);
LOG_BIZ(BIZ_FILL, "成交 order_id={} qty={}", order_id, qty);
```

**业务日志类型**：
- `BIZ_SESS` - 会话（登录/登出）
- `BIZ_ORDR` - 下单
- `BIZ_FILL` - 成交回报
- `BIZ_CNCL` - 撤单
- `BIZ_RJCT` - 拒绝
- `BIZ_POSN` - 持仓变化
- `BIZ_ACCT` - 账户/资金

---

## Deployment (Production)

### Building for Production

**IMPORTANT**: Production server is CentOS 7 (glibc 2.17). **MUST** use Docker build:

```bash
./docker-build.sh
```

**Why Docker?**
- Ensures GLIBC 2.17 compatibility (CentOS 7 requirement)
- Local build may use GLIBC 2.32+ which will fail on production server
- Cross-compiles in CentOS 7 container environment

**Build Process**:
1. Builds Docker image from `fastfish/docker/Dockerfile`
2. Compiles in CentOS 7 container (with correct GCC/GLIBC)
3. Copies result to `build_centos7/engine`
4. Also copies to `build/engine` for deployment

### Deploying to Remote Server

```bash
# Package and deploy (full workflow)
./deploy.sh

# Package only (no upload)
./deploy.sh --pack-only

# Deploy to specific server
./deploy.sh --host market-m --dest /path/to/dest
```

**DO NOT** manually deploy. The `deploy.sh` script:
- Packages `build/engine` binary
- Includes all shared libraries from `fastfish/libs/`
- Includes configs from `config/` and `fastfish/config/prod/`
- Includes SSL certificates from `fastfish/cert/`
- Creates deployment structure with `run.sh`
- Handles library symbolic links
- Uploads to remote server via rsync

**Deployment Checklist**:
1. ✅ Run `./docker-build.sh` (not `./build.sh`!)
2. ✅ Verify binary exists: `ls -lh build/engine`
3. ✅ Run `./deploy.sh`
4. ✅ SSH to server and test: `./run.sh backtest`

---

## Environment Variables (Live Mode Only)

Live trading requires these environment variables:

```bash
export FF_USER="gateway_username"
export FF_PASSWORD="gateway_password"
export FF_IP="gateway_ip"
export FF_PORT="gateway_port"
export FF_CERT_DIR="local_interface_ip"
```

**Backtest mode does NOT need these.**

---

## Common Issues and Solutions

### Issue: `error while loading shared libraries: libXXX.so`

**Solution**: Set library path before running:
```bash
export LD_LIBRARY_PATH=$PWD/fastfish/libs:$LD_LIBRARY_PATH
```

Or use the build script which sets this automatically.

### Issue: CMake can't find dependencies

**Solution**: Clean rebuild:
```bash
./build.sh --clean
```

### Issue: Compilation fails with ABI errors

**Cause**: GCC version mismatch with fastfish libraries.

**Solution**: Use Docker build:
```bash
./docker-build.sh
```

### Issue: Strategy not found at runtime

**Check**:
1. Strategy is registered in `include/strategy_factory.h`
2. Strategy name in config matches exactly (case-sensitive)
3. Engine was rebuilt after adding strategy

---

## File Structure Summary

```
/home/pc/screwdriver/
├── build/                    # CMake build output (DO NOT commit)
│   └── engine               # Main executable
├── build_centos7/           # Docker build output
├── config/
│   ├── backtest.conf        # Backtest configuration
│   └── live.conf            # Live trading configuration
├── fastfish/                # HTSC gateway SDK (submodule)
│   ├── libs/                # Shared libraries (.so files)
│   ├── include/             # Gateway headers
│   └── cert/                # SSL certificates
├── include/                 # Project headers
├── src/
│   ├── main.cpp             # Entry point
│   ├── FastOrderBook.cpp    # OrderBook implementation
│   └── strategy/            # Strategy implementations
├── test/                    # Test programs
├── logs/                    # Runtime logs
├── market_data/             # Downloaded market data
├── script/                  # Utility scripts
├── CMakeLists.txt           # Build configuration
├── build.sh                 # STANDARD BUILD SCRIPT (use this!)
├── docker-build.sh          # Production build
├── deploy.sh                # Deployment script
└── run_backtest.py          # Backtest automation
```

---

## Quick Reference

### Daily Development Workflow

**Local Development & Testing:**
```bash
# 1. Modify code
vim src/strategy/MyStrategy.h

# 2. Build locally
./build.sh engine

# 3. Test with backtest
python run_backtest.py 20260114

# 4. Check logs
tail -f logs/backtest_biz.log
```

**Production Deployment:**
```bash
# 1. Final code review and commit
git commit -am "Add new strategy"

# 2. Build for production (CentOS 7)
./docker-build.sh

# 3. Deploy to server
./deploy.sh

# 4. SSH to server and verify
ssh market-m
cd /home/jiace/project/trading-engine
./run.sh backtest
```

### Claude's Automatic Actions

**During Local Development:**
When Claude modifies code, it should:
1. ✅ Immediately build using `./build.sh [target]` (for verification)
2. ✅ Fix any compilation errors
3. ✅ Suggest running backtest to verify functionality
4. ✅ Check logs if user reports issues

**For Production Deployment:**
Claude should:
1. ✅ Remind user to use `./docker-build.sh` (not `./build.sh`)
2. ✅ Suggest testing locally first with `./build.sh` + backtest
3. ✅ Guide through deployment checklist
4. ✅ Never assume local build works on production server

---

## Additional Notes

- **Never commit** `build/` or `build_centos7/` directories
- **Always use** `./build.sh` for consistency
- **Check logs** in `logs/` after running backtest
- **Market data** is downloaded on-demand by `run_backtest.py`
- **ABI compatibility**: Code must be compatible with fastfish's `_GLIBCXX_USE_CXX11_ABI=0`

---

Last Updated: 2026-01-15
