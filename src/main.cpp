#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>

// 引入策略引擎和适配器
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "live_market_adapter.h"

// 引入策略
#include "strategy/PriceLevelVolumeStrategy.h"
#include "strategy/TestStrategy.h"
#include "strategy/PrintStrategy.h"
#include "strategy/OpeningRangeBreakoutStrategy.h"
#include "strategy/GapUpVolumeBreakoutStrategy.h"

// 引入配置和策略工厂
#include "backtest_config.h"
#include "strategy_factory.h"

// 引入日志模块
#include "logger.h"

// 引入 ZMQ 通信模块
#include "zmq_client.h"

// 引入 fastfish SDK 所需头文件
#include "parameter_define.h"
#include "udp_client_interface.h"
#include "base_define.h"  // init_env() / fini_env()

// ==========================================
// 策略注册
// ==========================================
void register_all_strategies() {
    auto& factory = StrategyFactory::instance();

    factory.register_strategy("PriceLevelVolumeStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<PriceLevelVolumeStrategy>(symbol + "_PriceLevel");
        });

    factory.register_strategy("TestStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<TestStrategy>(symbol + "_Test");
        });

    factory.register_strategy("PerformanceStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<PerformanceStrategy>(symbol + "_Perf");
        });

    factory.register_strategy("PrintStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<PrintStrategy>(symbol + "_Print");
        });

    factory.register_strategy("OpeningRangeBreakoutStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<OpeningRangeBreakoutStrategy>(symbol + "_ORB");
        });

    factory.register_strategy("GapUpVolumeBreakoutStrategy",
        [](const std::string& symbol) -> std::unique_ptr<Strategy> {
            return std::make_unique<GapUpVolumeBreakoutStrategy>(symbol + "_GUVB");
        });
}

// ==========================================
// 回测模式
// ==========================================
void run_backtest_mode(quill::Logger* logger, const std::string& config_file = "config/backtest.conf") {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Backtest Mode ===");

    // 注册所有策略
    register_all_strategies();

    // 解析配置文件
    auto configs = parse_backtest_config(config_file);
    if (configs.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid configurations found in {}", config_file);
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Config format: stock_code,strategy_name (e.g., 600759,PriceLevelVolumeStrategy)");
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loaded {} backtest configurations from {}", configs.size(), config_file);

    // 创建策略引擎
    StrategyEngine engine;

    // 有效股票列表（用于后续加载数据）
    std::vector<std::string> valid_symbols;

    auto& factory = StrategyFactory::instance();

    // 为每个配置创建策略并检查数据
    for (const auto& cfg : configs) {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Processing: {} with strategy {}", cfg.symbol, cfg.strategy_name);

        // 检查策略是否存在
        if (!factory.has_strategy(cfg.strategy_name)) {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Unknown strategy: {}, skipping {}", cfg.strategy_name, cfg.symbol);
            auto available = factory.get_registered_strategies();
            std::string avail_str;
            for (const auto& s : available) avail_str += s + " ";
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Available strategies: {}", avail_str);
            continue;
        }

        // 检查/下载数据
        if (!check_data_exists(cfg.symbol)) {
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Data not found for {}, attempting download...", cfg.symbol);
            if (!download_market_data(cfg.symbol)) {
                LOG_MODULE_WARNING(logger, MOD_ENGINE, "Failed to download data for {}, skipping", cfg.symbol);
                continue;
            }
            if (!check_data_exists(cfg.symbol)) {
                LOG_MODULE_WARNING(logger, MOD_ENGINE, "Data still not available for {}, skipping", cfg.symbol);
                continue;
            }
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Data downloaded successfully for {}", cfg.symbol);
        }

        // 创建策略实例并转移所有权给 engine
        try {
            auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
            engine.register_strategy(cfg.symbol, std::move(strategy));  // 转移所有权
            valid_symbols.push_back(cfg.symbol);
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Registered strategy {} for {}", cfg.strategy_name, cfg.symbol);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create strategy: {}", e.what());
        }
    }

    if (valid_symbols.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid symbols to backtest");
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting strategy engine with {} symbols...", valid_symbols.size());
    engine.start();

    // 创建回测适配器
    BacktestAdapter adapter(&engine, StrategyEngine::SHARD_COUNT);

    // 为每个股票加载数据
    for (const auto& symbol : valid_symbols) {
        std::string tick_file = "test_data/MD_TICK_StockType_" + symbol + ".csv";
        std::string order_file = "test_data/MD_ORDER_StockType_" + symbol + ".csv";
        std::string txn_file = "test_data/MD_TRANSACTION_StockType_" + symbol + ".csv";

        LOG_MODULE_INFO(logger, MOD_ENGINE, "Loading data for {}...", symbol);

        if (!adapter.load_tick_file(tick_file)) {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Failed to load TICK file for {}", symbol);
        }
        adapter.load_order_file(order_file);
        adapter.load_transaction_file(txn_file);

        LOG_MODULE_INFO(logger, MOD_ENGINE, "Total events after loading {}: {}", symbol, adapter.event_count());
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Replaying {} events...", adapter.event_count());
    adapter.replay();
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Replay completed");

    // 等待队列排空
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping strategy engine...");
    engine.stop();
}

// ==========================================
// 全局 ZMQ 客户端（用于发送信号）
// ==========================================
static std::unique_ptr<ZmqClient> g_zmq_client;

// 发送交易信号到外部系统
void send_signal(const std::string& signal_type, const json& data) {
    if (g_zmq_client && g_zmq_client->is_running()) {
        json payload;
        payload["type"] = signal_type;
        payload["data"] = data;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string req_id = signal_type + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        g_zmq_client->send(req_id, payload);
    }
}

// ==========================================
// 实盘模式
// ==========================================
void run_live_mode(quill::Logger* logger, const std::string& config_file = "config/live.conf") {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Live Trading Mode ===");

    // 注册所有策略
    register_all_strategies();

    // 解析配置文件
    auto configs = parse_backtest_config(config_file);
    if (configs.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid configurations found in {}", config_file);
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Config format: stock_code,strategy_name (e.g., 600759,PriceLevelVolumeStrategy)");
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loaded {} live trading configurations from {}", configs.size(), config_file);

    // 创建策略引擎
    StrategyEngine engine;

    auto& factory = StrategyFactory::instance();

    // 为每个配置创建策略
    std::vector<std::string> valid_symbols;
    for (const auto& cfg : configs) {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Processing: {} with strategy {}", cfg.symbol, cfg.strategy_name);

        // 检查策略是否存在
        if (!factory.has_strategy(cfg.strategy_name)) {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Unknown strategy: {}, skipping {}", cfg.strategy_name, cfg.symbol);
            auto available = factory.get_registered_strategies();
            std::string avail_str;
            for (const auto& s : available) avail_str += s + " ";
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Available strategies: {}", avail_str);
            continue;
        }

        // 创建策略实例并转移所有权给 engine
        try {
            auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
            engine.register_strategy(cfg.symbol, std::move(strategy));
            valid_symbols.push_back(cfg.symbol);
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Registered strategy {} for {}", cfg.strategy_name, cfg.symbol);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create strategy: {}", e.what());
        }
    }

    if (valid_symbols.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid symbols to monitor");
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting strategy engine with {} symbols...", valid_symbols.size());
    engine.start();

    // 初始化 ZMQ 客户端（可通过 DISABLE_ZMQ=1 禁用）
    // 注意：某些系统上 ZMQ 可能因 signaler 问题导致 abort
    const char* disable_zmq = std::getenv("DISABLE_ZMQ");
    if (disable_zmq && std::string(disable_zmq) == "1") {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "ZMQ client disabled via DISABLE_ZMQ=1");
    } else {
        const char* zmq_endpoint = std::getenv("ZMQ_ENDPOINT");
        std::string endpoint = zmq_endpoint ? zmq_endpoint : "tcp://localhost:13380";

        g_zmq_client = std::make_unique<ZmqClient>(endpoint);

        if (g_zmq_client->start()) {
            LOG_MODULE_INFO(logger, MOD_ENGINE, "ZMQ client started, endpoint: {}", endpoint);
        } else {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Failed to start ZMQ client, continuing without it");
        }
    }

    // 创建实盘数据适配器
    LiveMarketAdapter adapter("market_data", &engine);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Connecting to market data gateway...");

    // 创建 UDP 客户端 (模仿 fastfish/src/main.cc 的 test_udp_client 实现)
    using namespace com::htsc::mdc::gateway;
    using namespace com::htsc::mdc::udp;

    从环境变量读取配置参数
    const char* env_user = std::getenv("FF_USER");
    const char* env_password = std::getenv("FF_PASSWORD");
    const char* env_ip = std::getenv("FF_IP");
    const char* env_port = std::getenv("FF_PORT");
    const char* env_interface_ip = std::getenv("FF_CERT_DIR");

    if (!env_user || !env_password || !env_ip || !env_port || !env_interface_ip) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "Missing required environment variables. Please set FF_USER, FF_PASSWORD, FF_IP, FF_PORT, FF_CERT_DIR");
        engine.stop();
        hft::logger::shutdown();
        return;
    }

    std::string user = env_user;
    std::string password = env_password;
    std::string ip = env_ip;
    int port = std::stoi(env_port);
    std::string interface_ip = env_interface_ip;  // UDP客户端本地接口IP

    // 创建UDP客户端
    UdpClientInterface* udp_client = ClientFactory::Instance()->CreateUdpClient();

    if (!udp_client) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create UDP client");
        engine.stop();
        hft::logger::shutdown();
        return;
    }

    // 设置工作线程池大小
    udp_client->SetWorkPoolThreadCount(50);

    // 注册消息处理器
    udp_client->RegistHandle(&adapter);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Logging in to gateway at {}:{}...", ip, port);

    // 登录（使用备份服务器列表）
    std::map<std::string, int> backup_list;
    backup_list.insert(std::pair<std::string, int>("168.9.65.25", 18088));
    // backup_list.insert(std::pair<std::string, int>("backup_ip_2", 18088));

    int ret = udp_client->LoginById(ip, port, user, password, backup_list);
    if (ret != 0) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "Login failed with error code: {}", ret);
        ClientFactory::Uninstance();
        engine.stop();
        hft::logger::shutdown();
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Login successful");

    // 订阅市场数据 - 订阅指定股票类型
    std::unique_ptr<SubscribeBySourceType> source_type(new SubscribeBySourceType());

    // 订阅上海股票的TICK、ORDER、TRANSACTION数据
    SubscribeBySourceTypeDetail* detail_shg = source_type->add_subscribebysourcetypedetail();
    SecuritySourceType* security_source_shg = new SecuritySourceType();
    security_source_shg->set_securitytype(StockType);
    security_source_shg->set_securityidsource(XSHG);
    detail_shg->set_allocated_securitysourcetypes(security_source_shg);
    detail_shg->add_marketdatatypes(MD_TICK);
    detail_shg->add_marketdatatypes(MD_ORDER);
    detail_shg->add_marketdatatypes(MD_TRANSACTION);
    detail_shg->add_marketdatatypes(AD_ORDERBOOK_SNAPSHOT);  // OrderBook快照（备份）

    // 订阅深圳股票的TICK、ORDER、TRANSACTION数据
    SubscribeBySourceTypeDetail* detail_she = source_type->add_subscribebysourcetypedetail();
    SecuritySourceType* security_source_she = new SecuritySourceType();
    security_source_she->set_securitytype(StockType);
    security_source_she->set_securityidsource(XSHE);
    detail_she->set_allocated_securitysourcetypes(security_source_she);
    detail_she->add_marketdatatypes(MD_TICK);
    detail_she->add_marketdatatypes(MD_ORDER);
    detail_she->add_marketdatatypes(MD_TRANSACTION);
    detail_she->add_marketdatatypes(AD_ORDERBOOK_SNAPSHOT);  // OrderBook快照（备份）

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Subscribing to market data (StockType, XSHG+XSHE)...");
    ret = udp_client->SubscribeBySourceType(interface_ip, source_type.get());
    if (ret != 0) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "Subscribe failed with error code: {}", ret);
        ClientFactory::Uninstance();
        engine.stop();
        hft::logger::shutdown();
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Subscription successful");

    // 保持运行直到用户中断
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Running... Press Ctrl+C to stop");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 清理（注意：这段代码在正常情况下不会执行，因为上面是无限循环）
    // 实际应该通过信号处理器来优雅关闭
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping ZMQ client...");
    if (g_zmq_client) {
        g_zmq_client->stop();
        g_zmq_client.reset();
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping strategy engine...");
    engine.stop();
    ClientFactory::Uninstance();
}

// ==========================================
// 主入口
// ==========================================
int main(int argc, char* argv[]) {
    // 初始化 FastFish SDK 环境 (ACE 框架)
    com::htsc::mdc::gateway::init_env();

    // 根据命令行参数选择模式
    std::string mode = "backtest"; // 默认回测模式

    if (argc > 1) {
        mode = argv[1];
    }

    std::cout << "Starting in " << mode << " mode..." << std::endl;

    // 统一初始化日志系统（根据模式设置日志文件名）
    hft::logger::LogConfig log_config;
    log_config.log_dir = "logs";
    log_config.log_file = (mode == "backtest") ? "backtest.log" : "live_trading.log";
    log_config.console_output = true;
    log_config.use_rdtsc = true;
    auto* logger = hft::logger::init(log_config);

    // 初始化业务日志器（交易信号等业务日志单独输出）
    hft::logger::BizLogConfig biz_config;
    biz_config.log_dir = "logs";
    biz_config.log_file = (mode == "backtest") ? "backtest_biz.log" : "live_biz.log";
    biz_config.console_output = true;
    hft::logger::init_biz(biz_config);

    // 根据模式运行
    if (mode == "backtest") {
        run_backtest_mode(logger);
    } else if (mode == "live") {
        run_live_mode(logger);
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        std::cerr << "Usage: " << argv[0] << " [backtest|live]" << std::endl;
        hft::logger::shutdown();
        return 1;
    }

    // 统一关闭日志系统
    hft::logger::shutdown();

    // 清理 FastFish SDK 环境
    com::htsc::mdc::gateway::fini_env();

    return 0;
}
