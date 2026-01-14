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

### CRITICAL: Always Use the Build Script

**DO NOT** use raw `cmake` or `make` commands directly. **ALWAYS** use:

```bash
./build.sh [target]
```

**Why?**
- Sets `LD_LIBRARY_PATH` correctly for fastfish libraries
- Uses consistent build configuration
- Handles ABI compatibility (`-D_GLIBCXX_USE_CXX11_ABI=0`)
- Ensures proper RPATH settings

### Build Targets

| Target | Purpose | When to Build |
|--------|---------|---------------|
| `engine` | Main trading engine | After modifying `src/main.cpp`, `src/strategy/*`, `include/*` |
| `test_logger` | Logger tests | After modifying logging setup |
| `test_fastorderbook` | OrderBook replay | After modifying `src/FastOrderBook.cpp` |
| `test_price_level_strategy` | Strategy tests | After modifying strategy code |
| (all) | Build everything | Use `./build.sh` without target |

### Build Examples

```bash
# Build main engine (most common)
./build.sh engine

# Build and run tests
./build.sh test_fastorderbook
./build/test_fastorderbook

# Clean rebuild (after CMakeLists.txt changes)
./build.sh --clean engine

# Debug build (for debugging with gdb)
./build.sh --debug engine
```

### When Claude Should Build

Claude should **automatically** build when:
1. ✅ Modifying any `.cpp` or `.h` files
2. ✅ Creating new strategy files in `src/strategy/`
3. ✅ Changing `CMakeLists.txt`
4. ✅ User asks to "test" or "run" the engine
5. ✅ After fixing compilation errors

Claude should **NOT** build when:
- ❌ Only reading/analyzing code
- ❌ Modifying configuration files (`.conf`, `.py`)
- ❌ Editing documentation

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

---

## Deployment (Production)

### Building for Production

Use Docker build for CentOS 7 compatibility:
```bash
./docker-build.sh
```

This ensures binary runs on remote CentOS 7 servers (GLIBC compatibility).

### Deploying to Remote Server

```bash
# Package and deploy
./deploy.sh

# Package only (no upload)
./deploy.sh --pack-only

# Deploy to specific server
./deploy.sh --host market-m --dest /path/to/dest
```

**DO NOT** manually deploy. The script:
- Packages binary + libraries + configs + certs
- Creates deployment structure
- Generates `run.sh` startup script
- Handles library symbolic links

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

```bash
# 1. Modify code
vim src/strategy/MyStrategy.h

# 2. Build
./build.sh engine

# 3. Test locally
python run_backtest.py 20260114

# 4. Check logs
tail -f logs/backtest_biz.log
```

### Claude's Automatic Actions

When Claude modifies code, it should:
1. ✅ Immediately build using `./build.sh [target]`
2. ✅ Fix any compilation errors
3. ✅ Suggest running backtest to verify
4. ✅ Check logs if user reports issues

---

## Additional Notes

- **Never commit** `build/` or `build_centos7/` directories
- **Always use** `./build.sh` for consistency
- **Check logs** in `logs/` after running backtest
- **Market data** is downloaded on-demand by `run_backtest.py`
- **ABI compatibility**: Code must be compatible with fastfish's `_GLIBCXX_USE_CXX11_ABI=0`

---

Last Updated: 2026-01-14
