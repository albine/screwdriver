# Trading Engine 编译与部署指南

## 概述

本文档介绍如何编译 Trading Engine 并部署到生产服务器（CentOS 7）。

## 环境要求

### 本地开发环境
- Docker（用于交叉编译）
- SSH 配置（~/.ssh/config 中配置 market-m）

### 生产服务器
- CentOS 7.x
- glibc 2.17+

## 编译步骤

### 1. Docker 编译（推荐）

由于生产服务器是 CentOS 7（glibc 2.17），需要使用 Docker 容器编译以确保兼容性：

```bash
# 使用 CentOS 7 容器编译 Release 版本
./docker-build.sh
```

编译完成后，二进制文件位于：`build/engine`

### 2. 本地编译（仅限本地测试）

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make engine -j$(nproc)
```

> **注意**：本地编译的二进制文件可能无法在 CentOS 7 上运行（glibc 版本不兼容）

## 部署步骤

### 1. 打包并部署

```bash
# 直接部署到 market-m（默认）
./deploy.sh

# 或仅打包不上传
./deploy.sh --pack-only
```

### 2. 部署选项

```
./deploy.sh [options]

Options:
    -h, --host HOST    远程服务器别名 (默认: market-m)
    -d, --dest PATH    远程部署路径 (默认: /home/jiace/project/trading-engine)
    -p, --pack-only    仅打包不上传
    --help             显示帮助
```

## 部署目录结构

```
/home/jiace/project/trading-engine/
├── bin/
│   └── engine                      # 主程序 (546KB)
├── lib/                            # 共享库
│   ├── libprotobuf.so.11.0.0       # Protocol Buffers (22MB)
│   ├── libprotobuf.so.11 -> libprotobuf.so.11.0.0
│   ├── libprotobuf.so -> libprotobuf.so.11.0.0
│   ├── libmdc_gateway_client.so    # 行情网关客户端 (5.1MB)
│   ├── libssl.so.1.0.2k            # OpenSSL (460KB)
│   ├── libssl.so.10 -> libssl.so.1.0.2k
│   ├── libssl.so -> libssl.so.1.0.2k
│   ├── libcrypto.so.1.0.2k         # OpenSSL Crypto (2.5MB)
│   ├── libcrypto.so.10 -> libcrypto.so.1.0.2k
│   ├── libcrypto.so -> libcrypto.so.1.0.2k
│   ├── libaeron_client_shared.so   # Aeron 客户端 (390KB)
│   ├── libaeron_driver.so          # Aeron 驱动 (771KB)
│   ├── libACE.so.6.4.3             # ACE 框架 (14MB)
│   ├── libACE.so -> libACE.so.6.4.3
│   ├── libACE_SSL.so.6.4.3         # ACE SSL (928KB)
│   └── libACE_SSL.so -> libACE_SSL.so.6.4.3
├── config/
│   └── htsc-insight-cpp-config.conf  # SDK 配置文件
├── cert/
│   ├── InsightClientCert.pem       # SSL 客户端证书
│   └── InsightClientKeyPkcs8.pem   # SSL 客户端密钥
├── logs/                           # 日志目录（运行时创建）
└── run.sh                          # 启动脚本
```

## 最小部署文件清单

| 文件 | 来源 | 大小 | 说明 |
|------|------|------|------|
| engine | build/engine | 546KB | 主程序 |
| libprotobuf.so.11.0.0 | fastfish/libs/ | 22MB | Protocol Buffers 序列化 |
| libmdc_gateway_client.so | fastfish/libs/ | 5.1MB | 行情网关客户端 SDK |
| libssl.so.1.0.2k | fastfish/libs/ | 460KB | OpenSSL |
| libcrypto.so.1.0.2k | fastfish/libs/ | 2.5MB | OpenSSL 加密库 |
| libaeron_client_shared.so | fastfish/libs/ | 390KB | Aeron 消息客户端 |
| libaeron_driver.so | fastfish/libs/ | 771KB | Aeron 驱动 |
| libACE.so.6.4.3 | fastfish/libs/ | 14MB | ACE 框架 |
| libACE_SSL.so.6.4.3 | fastfish/libs/ | 928KB | ACE SSL 扩展 |
| htsc-insight-cpp-config.conf | fastfish/config/prod/ | <1KB | SDK 配置 |
| InsightClientCert.pem | fastfish/cert/ | 1.3KB | SSL 证书 |
| InsightClientKeyPkcs8.pem | fastfish/cert/ | 1.7KB | SSL 密钥 |

**总计：约 47MB**

## 运行方式

### 1. 设置环境变量（实盘模式必需）

在 `~/.bashrc` 中添加：

```bash
export FF_USER="your_username"
export FF_PASSWORD="your_password"
export FF_IP="gateway_ip"
export FF_PORT="gateway_port"
export FF_CERT_DIR="local_interface_ip"
```

然后执行 `source ~/.bashrc`

### 2. 启动程序

```bash
cd /home/jiace/project/trading-engine

# 回测模式
./run.sh backtest

# 实盘模式
./run.sh live
```

### 3. 查看帮助

```bash
./run.sh --help
```

## 常见问题

### Q: 出现 GLIBC 版本不兼容错误

```
/lib64/libc.so.6: version `GLIBC_2.32' not found
```

**原因**：使用本地环境编译的二进制文件，glibc 版本过高。

**解决**：使用 `./docker-build.sh` 在 CentOS 7 容器中编译。

### Q: 找不到共享库

```
error while loading shared libraries: libxxx.so
```

**解决**：确保 `lib/` 目录下的库文件完整，且 `run.sh` 正确设置了 `LD_LIBRARY_PATH`。

### Q: 实盘模式启动失败

```
Missing required environment variables: FF_USER FF_PASSWORD ...
```

**解决**：设置所有必需的环境变量，参见上文"设置环境变量"部分。

## 脚本说明

| 脚本 | 用途 |
|------|------|
| docker-build.sh | 使用 Docker 容器编译（CentOS 7 兼容） |
| deploy.sh | 打包并部署到远程服务器 |
| run.sh | 本地启动脚本 |

## 相关文件

- `fastfish/docker/Dockerfile` - Docker 编译环境配置
- `fastfish/sync.sh` - fastfish 库同步脚本（SSH 配置参考）
- `CMakeLists.txt` - CMake 构建配置
