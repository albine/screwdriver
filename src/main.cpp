#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <algorithm>
#include <csignal>
#include <atomic>

// 引入策略引擎和适配器
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "live_market_adapter.h"

// 引入策略
#include "strategy/PriceLevelVolumeStrategy.h"
#include "strategy/BreakoutPriceVolumeStrategy.h"
#include "strategy/TestStrategy.h"
#include "strategy/PrintStrategy.h"
#include "strategy/OpeningRangeBreakoutStrategy.h"
#include "strategy/GapUpVolumeBreakoutStrategy.h"

// 引入配置和策略工厂
#include "backtest_config.h"
#include "strategy_factory.h"
#include "strategy_ids.h"
#include "strategy_context.h"

// 引入持久化层
#include "persist_layer.h"

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
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<PriceLevelVolumeStrategy>(symbol + "_PriceLevel", symbol);
            strat->strategy_type_id = StrategyIds::PRICE_LEVEL_VOLUME;
            return strat;
        });

    // BreakoutPriceVolumeStrategy: 配置格式 "股票代码,BreakoutPriceVolumeStrategy"
    // 突破价格通过 ControlMessage 的 param 在 enable 时传入
    // 策略默认禁用，需要通过 ZMQ 命令 enable 时带入 target_price
    factory.register_strategy("BreakoutPriceVolumeStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<BreakoutPriceVolumeStrategy>(symbol + "_Breakout", symbol);
            strat->strategy_type_id = StrategyIds::BREAKOUT_PRICE_VOLUME;
            // 构造函数中已默认禁用
            return strat;
        });

    factory.register_strategy("TestStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<TestStrategy>(symbol + "_Test", symbol);
            strat->strategy_type_id = StrategyIds::TEST_STRATEGY;
            return strat;
        });

    factory.register_strategy("PerformanceStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<PerformanceStrategy>(symbol + "_Perf", symbol);
            strat->strategy_type_id = StrategyIds::PERFORMANCE_STRATEGY;
            return strat;
        });

    factory.register_strategy("PrintStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<PrintStrategy>(symbol + "_Print", symbol);
            strat->strategy_type_id = StrategyIds::PRINT_STRATEGY;
            return strat;
        });

    factory.register_strategy("OpeningRangeBreakoutStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<OpeningRangeBreakoutStrategy>(symbol + "_ORB", symbol);
            strat->strategy_type_id = StrategyIds::OPENING_RANGE_BREAKOUT;
            return strat;
        });

    factory.register_strategy("GapUpVolumeBreakoutStrategy",
        [](const std::string& symbol, const std::string& /*params*/) -> std::unique_ptr<Strategy> {
            auto strat = std::make_unique<GapUpVolumeBreakoutStrategy>(symbol + "_GUVB", symbol);
            strat->strategy_type_id = StrategyIds::GAP_UP_VOLUME_BREAKOUT;
            return strat;
        });
}

// ==========================================
// 回测模式
// ==========================================
void run_backtest_mode(quill::Logger* logger, const std::string& config_file = "config/strategy_backtest.conf") {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Backtest Mode ===");

    // 注册所有策略
    register_all_strategies();

    // 解析配置文件
    auto configs = parse_strategy_config(config_file);
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
            auto strategy = factory.create(cfg.strategy_name, cfg.symbol, cfg.params);
            engine.register_strategy(cfg.symbol, std::move(strategy));  // 转移所有权
            valid_symbols.push_back(cfg.symbol);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create strategy for {}: {}", cfg.symbol, e.what());
        }
    }

    if (valid_symbols.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid symbols to backtest");
        return;
    }

    // 创建回测上下文
    auto backtest_ctx = std::make_unique<BacktestContext>();

    // 为所有策略设置上下文
    engine.set_context_for_all_strategies(backtest_ctx.get());

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
// 全局 ZMQ 客户端
// ==========================================
static std::unique_ptr<ZmqClient> g_zmq_client;

// ==========================================
// 信号处理
// ==========================================
static std::atomic<bool> g_running{true};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_running.store(false, std::memory_order_relaxed);
    }
}

// ==========================================
// 实盘模式
// ==========================================
void run_live_mode(quill::Logger* logger,
                   const std::string& strategy_config_file = "config/strategy_live.conf",
                   const std::string& engine_config_file = "config/engine.conf") {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Live Trading Mode ===");

    // 解析引擎配置
    auto engine_cfg = parse_engine_config(engine_config_file);
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loaded engine config from {}", engine_config_file);

    // 注册所有策略
    register_all_strategies();

    // 解析策略配置文件
    auto configs = parse_strategy_config(strategy_config_file);
    if (configs.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid configurations found in {}", strategy_config_file);
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Config format: stock_code,strategy_name (e.g., 600759,PriceLevelVolumeStrategy)");
        return;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loaded {} configurations from {}", configs.size(), strategy_config_file);

    // 统计每种策略的配置数量
    std::map<std::string, int> strategy_counts;
    for (const auto& cfg : configs) {
        strategy_counts[cfg.strategy_name]++;
    }
    for (const auto& [name, count] : strategy_counts) {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "  - {}: {} symbols", name, count);
    }

    // 创建策略引擎
    StrategyEngine engine;

    auto& factory = StrategyFactory::instance();

    // 为每个配置创建策略
    std::vector<std::string> valid_symbols;
    for (const auto& cfg : configs) {
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
            auto strategy = factory.create(cfg.strategy_name, cfg.symbol, cfg.params);
            engine.register_strategy(cfg.symbol, std::move(strategy));
            // 只添加一次（去重）
            if (std::find(valid_symbols.begin(), valid_symbols.end(), cfg.symbol) == valid_symbols.end()) {
                valid_symbols.push_back(cfg.symbol);
            }
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create strategy for {}: {}", cfg.symbol, e.what());
        }
    }

    if (valid_symbols.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid symbols to monitor");
        return;
    }

    // 为所有股票添加默认策略 BreakoutPriceVolumeStrategy（默认禁用，需通过 ZMQ 命令启用）
    for (const auto& symbol : valid_symbols) {
        try {
            auto default_strategy = factory.create("BreakoutPriceVolumeStrategy", symbol, "");
            engine.register_strategy(symbol, std::move(default_strategy));
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to create default strategy for {}: {}", symbol, e.what());
        }
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting strategy engine with {} symbols...", valid_symbols.size());
    engine.start();

    // 创建实盘上下文（稍后在 ZMQ 客户端启动后设置）
    std::unique_ptr<LiveContext> live_ctx;

    // 初始化 ZMQ 客户端（可通过 engine.conf 中 disable_zmq=true 禁用）
    if (engine_cfg.disable_zmq) {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "ZMQ client disabled via config");
    } else {
        g_zmq_client = std::make_unique<ZmqClient>(engine_cfg.zmq_endpoint);

        // 注入策略引擎引用（支持运行时动态添加/删除策略）
        g_zmq_client->set_engine(&engine);

        if (g_zmq_client->start()) {
            LOG_MODULE_INFO(logger, MOD_ENGINE, "ZMQ client started, endpoint: {}", engine_cfg.zmq_endpoint);
            LOG_MODULE_INFO(logger, MOD_ENGINE, "Runtime strategy management enabled via ZMQ");

            // 创建实盘上下文并设置给所有策略
            live_ctx = std::make_unique<LiveContext>(g_zmq_client.get());
            engine.set_context_for_all_strategies(live_ctx.get());
        } else {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Failed to start ZMQ client, continuing without it");
        }
    }

    // ========================================
    // 创建持久化层 (可通过 engine.conf 中 disable_persist=true 禁用)
    // ========================================
    std::unique_ptr<PersistLayer> persist;
    if (engine_cfg.disable_persist) {
        LOG_MODULE_INFO(logger, MOD_ENGINE, "PersistLayer disabled via config");
    } else {
        persist = std::make_unique<PersistLayer>();

        // 获取当前日期
        std::string date = get_current_date();

        // 数据目录 (通过 engine.conf 配置)
        std::string data_dir = engine_cfg.persist_data_dir;

        // Writer 线程绑核 (绑定到最后一个 CPU)
        int last_cpu = static_cast<int>(std::thread::hardware_concurrency()) - 1;

        if (!persist->init(date, data_dir, last_cpu)) {
            LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to initialize PersistLayer");
            engine.stop();
            hft::logger::shutdown();
            com::htsc::mdc::gateway::fini_env();
            return;
        }

        LOG_MODULE_INFO(logger, MOD_ENGINE, "PersistLayer initialized: date={} dir={}", date, data_dir);
    }

    // 创建实盘数据适配器 (传入持久化层指针)
    LiveMarketAdapter adapter("market_data", &engine, persist.get());

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Connecting to market data gateway...");

    // 创建 UDP 客户端 (模仿 fastfish/src/main.cc 的 test_udp_client 实现)
    using namespace com::htsc::mdc::gateway;
    using namespace com::htsc::mdc::udp;

    // 从环境变量读取配置参数
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

    // 保持运行直到收到退出信号
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Running... Press Ctrl+C to stop");
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 优雅关闭
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Received shutdown signal, cleaning up...");

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping ZMQ client...");
    if (g_zmq_client) {
        g_zmq_client->stop();
        g_zmq_client.reset();
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping PersistLayer...");
    if (persist) {
        persist->stop();
        persist.reset();
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping strategy engine...");
    engine.stop();
    ClientFactory::Uninstance();
}

// ==========================================
// 主入口
// ==========================================
int main(int argc, char* argv[]) {
    // 注册信号处理器
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

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
