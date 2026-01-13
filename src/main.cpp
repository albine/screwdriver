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
#include "PriceLevelVolumeStrategy.h"

// 引入日志模块
#include "logger.h"

// 引入 fastfish SDK 所需头文件
#include "parameter_define.h"
#include "udp_client_interface.h"

// ==========================================
// 示例策略
// ==========================================
class PrintStrategy : public Strategy {
public:
    PrintStrategy(std::string n) { name = n; }

    void on_tick(const MDStockStruct& stock) override {
        // Simple tick print
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        // 每 1000 个订单打印一次最优报价
        if (order.applseqnum % 1000 == 0) {
            auto bid = book.get_best_bid();
            auto ask = book.get_best_ask();
            std::cout << "[Strat:" << name << "] OrderSeq:" << order.applseqnum
                      << " BestBid:" << (bid ? std::to_string(*bid) : "None")
                      << " BestAsk:" << (ask ? std::to_string(*ask) : "None")
                      << std::endl;
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        // Transaction logic
    }
};

// ==========================================
// 回测模式
// ==========================================
void run_backtest_mode(quill::Logger* logger) {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Backtest Mode ===");

    // 创建策略引擎
    StrategyEngine engine;

    // 创建价格档位挂单量监控策略
    // 策略会自动从TICK数据中获取昨收价和开盘价
    PriceLevelVolumeStrategy strategy("600759_PriceLevel");

    // 注册策略
    std::string symbol = "600759.SH";
    engine.register_strategy(symbol, &strategy);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting strategy engine...");
    engine.start();

    // 创建回测适配器
    BacktestAdapter adapter(&engine, StrategyEngine::SHARD_COUNT);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loading historical data for {}...", symbol);

    if (!adapter.load_tick_file("test_data/MD_TICK_StockType_600759.SH.csv")) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "Failed to load TICK file");
        return;
    }
    LOG_MODULE_INFO(logger, MOD_ENGINE, "TICK events loaded: {}", adapter.event_count());

    adapter.load_order_file("test_data/MD_ORDER_StockType_600759.SH.csv");
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Total events after ORDER: {}", adapter.event_count());

    adapter.load_transaction_file("test_data/MD_TRANSACTION_StockType_600759.SH.csv");
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Total events after TRANSACTION: {}", adapter.event_count());

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Replaying {} events...", adapter.event_count());
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting replay...");
    adapter.replay();
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Replay completed");

    // 等待队列排空
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping strategy engine...");
    engine.stop();
}

// ==========================================
// 实盘模式
// ==========================================
void run_live_mode(quill::Logger* logger) {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Live Trading Mode ===");

    // 创建策略引擎
    StrategyEngine engine;
    PrintStrategy strategy("LiveStrat");

    // 注册策略
    std::string symbol = "002603.SZ";
    engine.register_strategy(symbol, &strategy);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting strategy engine...");
    engine.start();

    // 创建实盘数据适配器
    LiveMarketAdapter adapter("market_data", &engine);

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

    // 订阅深圳股票的TICK、ORDER、TRANSACTION数据
    SubscribeBySourceTypeDetail* detail_she = source_type->add_subscribebysourcetypedetail();
    SecuritySourceType* security_source_she = new SecuritySourceType();
    security_source_she->set_securitytype(StockType);
    security_source_she->set_securityidsource(XSHE);
    detail_she->set_allocated_securitysourcetypes(security_source_she);
    detail_she->add_marketdatatypes(MD_TICK);
    detail_she->add_marketdatatypes(MD_ORDER);
    detail_she->add_marketdatatypes(MD_TRANSACTION);

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
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping strategy engine...");
    engine.stop();
    ClientFactory::Uninstance();
}

// ==========================================
// 主入口
// ==========================================
int main(int argc, char* argv[]) {
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
    return 0;
}
