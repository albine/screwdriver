#ifndef LIVE_MARKET_ADAPTER_H
#define LIVE_MARKET_ADAPTER_H

#include "../fastfish/src/InsightHandle.h"
#include "strategy_engine.h"
#include "market_data_structs.h"
#include "ADOrderbookSnapshot.pb.h"
#include "logger.h"

#include <cstring>
#include <algorithm>
#include <type_traits>
#include <atomic>
#include <chrono>

// 持久化层 (可选)
#include "persist_layer.h"

    // ==========================================
// 高性能拷贝宏
// DEST_ARR: 目标数组
// DEST_CNT: 目标计数变量
// SRC_REP:  源 Protobuf repeated 字段
// MAX_CAP:  目标容量 (如 50 或 10)
// ==========================================
#define COPY_AND_COUNT(DEST_ARR, DEST_CNT, SRC_REP, MAX_CAP) \
    do { \
        /* 1. 计算拷贝数量 (取最小) */ \
        size_t src_size = (SRC_REP).size(); \
        size_t count = (src_size < (MAX_CAP)) ? src_size : (MAX_CAP); \
        \
        /* 2. 批量内存拷贝 */ \
        /* 编译期类型检查：sizeof 必须匹配 */ \
        if (count > 0) { \
            static_assert(sizeof((DEST_ARR)[0]) == sizeof(*(SRC_REP).data()), \
                         "Element type size mismatch!"); \
            std::memcpy((DEST_ARR), (SRC_REP).data(), count * sizeof((DEST_ARR)[0])); \
        } \
        \
        /* 3. 更新计数器 */ \
        (DEST_CNT) = static_cast<int32_t>(count); \
    } while(0)

// 对于没有 count 变量的定长数组 (如盘口 10 档)，我们可以直接拷贝，或者默认填满
// 如果你不想修改 10 档盘口的结构体，可以使用这个简化版宏
#define COPY_FIXED_10(DEST_ARR, SRC_REP) \
    do { \
        size_t count = std::min((size_t)(SRC_REP).size(), (size_t)10); \
        if (count > 0) std::memcpy((DEST_ARR), (SRC_REP).data(), count * sizeof((DEST_ARR)[0])); \
        /* 如果不足10个，剩下的部分如果不清零，可能会有脏数据。*/ \
        /* HFT 中通常假设交易所总是推满10档，或者策略层判断价格是否为 0 */ \
        if (count < 10) std::memset((DEST_ARR) + count, 0, (10 - count) * sizeof((DEST_ARR)[0])); \
    } while(0)

// ==========================================
// 实盘数据适配器
// ==========================================
// 继承自 InsightHandle，将实盘数据转换为策略引擎可用的格式
// 支持可选的持久化层 (PersistLayer) 用于高频数据落盘
class LiveMarketAdapter : public InsightHandle {
private:
    StrategyEngine* engine_;
    PersistLayer* persist_;  // 可选，为 nullptr 时不持久化

public:
    // 构造函数
    // @param folder_name  数据文件夹名
    // @param engine       策略引擎指针
    // @param persist      持久化层指针 (可选，默认为 nullptr)
    explicit LiveMarketAdapter(const std::string& folder_name,
                               StrategyEngine* engine,
                               PersistLayer* persist = nullptr)
        : InsightHandle(folder_name)
        , engine_(engine)
        , persist_(persist) {}

    // 重写 OnMarketData 方法，将实盘数据转发到策略引擎
    // 使用高性能 fast 转换函数
    void OnMarketData(const com::htsc::mdc::insight::model::MarketData& data) override {
        // 首先调用父类方法进行数据保存（如果需要）
        // InsightHandle::OnMarketData(data);  // 注释掉以提升性能，如需数据落盘可开启

        // 将 protobuf 格式的市场数据转换为策略引擎使用的结构体
        // 使用栈上对象 + fast 转换 + move 语义
        switch (data.marketdatatype()) {
            case MD_TICK: {
                if (data.has_mdstock()) {
                    MDStockStruct stock;  // 栈上对象，不初始化
                    convert_to_stock_fast(data.mdstock(), stock);
                    // 持久化 (入队，不阻塞)
                    if (persist_) persist_->log_tick(stock);
                    engine_->on_market_tick(stock);
                }
                break;
            }
            case MD_ORDER: {
                if (data.has_mdorder()) {
                    const auto& pb_order = data.mdorder();
                    // 过滤非股票类型 (StockType = 2)
                    if (pb_order.securitytype() != StockType) break;
                    MDOrderStruct order;  // 栈上对象，不初始化
                    convert_to_order_fast(pb_order, order);
                    // 持久化 (入队，不阻塞)
                    if (persist_) persist_->log_order(order);
                    engine_->on_market_order(order);
                }
                break;
            }
            case MD_TRANSACTION: {
                if (data.has_mdtransaction()) {
                    const auto& pb_txn = data.mdtransaction();
                    // 过滤非股票类型 (StockType = 2)
                    if (pb_txn.securitytype() != StockType) break;
                    MDTransactionStruct transaction;  // 栈上对象，不初始化
                    convert_to_transaction_fast(pb_txn, transaction);
                    // 持久化 (入队，不阻塞)
                    if (persist_) persist_->log_transaction(transaction);
                    engine_->on_market_transaction(transaction);
                }
                break;
            }
            case AD_ORDERBOOK_SNAPSHOT: {
                if (data.has_orderbooksnapshot()) {
                    const auto& snapshot = data.orderbooksnapshot();
                    // 过滤非股票类型
                    if (snapshot.securitytype() != StockType) break;
                    // 持久化 (在 on_orderbook_snapshot 内部处理)
                    on_orderbook_snapshot(snapshot);
                }
                break;
            }
            default:
                break;
        }
    }

private:
    // 转换函数：将 protobuf MDStock 转换为 MDStockStruct
    MDStockStruct convert_to_stock(const com::htsc::mdc::insight::model::MDStock& pb_stock) {
        MDStockStruct stock = {};

        // 基本字段
        std::strncpy(stock.htscsecurityid, pb_stock.htscsecurityid().c_str(), sizeof(stock.htscsecurityid) - 1);
        stock.mddate = pb_stock.mddate();
        stock.mdtime = pb_stock.mdtime();
        stock.datatimestamp = pb_stock.datatimestamp();
        stock.tradingphasecode = pb_stock.tradingphasecode().empty() ? '\0' : pb_stock.tradingphasecode()[0];
        stock.securityidsource = pb_stock.securityidsource();
        stock.securitytype = pb_stock.securitytype();

        // 价格限制
        stock.maxpx = pb_stock.maxpx();
        stock.minpx = pb_stock.minpx();
        stock.preclosepx = pb_stock.preclosepx();

        // 交易统计
        stock.numtrades = pb_stock.numtrades();
        stock.totalvolumetrade = pb_stock.totalvolumetrade();
        stock.totalvaluetrade = pb_stock.totalvaluetrade();

        // 当前价格
        stock.lastpx = pb_stock.lastpx();
        stock.openpx = pb_stock.openpx();
        stock.closepx = pb_stock.closepx();
        stock.highpx = pb_stock.highpx();
        stock.lowpx = pb_stock.lowpx();

        // 买卖统计
        stock.totalbuyqty = pb_stock.totalbuyqty();
        stock.totalsellqty = pb_stock.totalsellqty();
        stock.weightedavgbuypx = pb_stock.weightedavgbuypx();
        stock.weightedavgsellpx = pb_stock.weightedavgsellpx();

        // 撤单统计
        stock.withdrawbuynumber = pb_stock.withdrawbuynumber();
        stock.withdrawbuyamount = pb_stock.withdrawbuyamount();
        stock.withdrawbuymoney = pb_stock.withdrawbuymoney();
        stock.withdrawsellnumber = pb_stock.withdrawsellnumber();
        stock.withdrawsellamount = pb_stock.withdrawsellamount();
        stock.withdrawsellmoney = pb_stock.withdrawsellmoney();

        // 订单数量
        stock.totalbuynumber = pb_stock.totalbuynumber();
        stock.totalsellnumber = pb_stock.totalsellnumber();
        stock.numbuyorders = pb_stock.numbuyorders();
        stock.numsellorders = pb_stock.numsellorders();

        // 通道号
        stock.channelno = pb_stock.channelno();

        // 买卖盘口深度
        for (int i = 0; i < 10 && i < pb_stock.buypricequeue_size(); ++i) {
            stock.buypricequeue[i] = pb_stock.buypricequeue(i);
            stock.buyorderqtyqueue[i] = pb_stock.buyorderqtyqueue(i);
        }
        for (int i = 0; i < 10 && i < pb_stock.sellpricequeue_size(); ++i) {
            stock.sellpricequeue[i] = pb_stock.sellpricequeue(i);
            stock.sellorderqtyqueue[i] = pb_stock.sellorderqtyqueue(i);
        }

        // 订单队列
        for (int i = 0; i < 50 && i < pb_stock.buyorderqueue_size(); ++i) {
            stock.buyorderqueue[i] = pb_stock.buyorderqueue(i);
        }
        for (int i = 0; i < 50 && i < pb_stock.sellorderqueue_size(); ++i) {
            stock.sellorderqueue[i] = pb_stock.sellorderqueue(i);
        }

        // 订单数量队列
        for (int i = 0; i < 50 && i < pb_stock.buynumordersqueue_size(); ++i) {
            stock.buynumordersqueue[i] = pb_stock.buynumordersqueue(i);
        }
        for (int i = 0; i < 50 && i < pb_stock.sellnumordersqueue_size(); ++i) {
            stock.sellnumordersqueue[i] = pb_stock.sellnumordersqueue(i);
        }

        stock.datamultiplepowerof10 = pb_stock.datamultiplepowerof10();

        return stock;
    }

    // 转换函数：将 protobuf MDOrder 转换为 MDOrderStruct
    MDOrderStruct convert_to_order(const com::htsc::mdc::insight::model::MDOrder& pb_order) {
        MDOrderStruct order = {};

        std::strncpy(order.htscsecurityid, pb_order.htscsecurityid().c_str(), sizeof(order.htscsecurityid) - 1);
        order.mddate = pb_order.mddate();
        order.mdtime = pb_order.mdtime();
        order.securityidsource = pb_order.securityidsource();

        order.orderindex = pb_order.orderindex();
        order.ordertype = pb_order.ordertype();
        order.orderprice = pb_order.orderprice();
        order.orderqty = pb_order.orderqty();
        order.orderbsflag = pb_order.orderbsflag();
        order.channelno = pb_order.channelno();

        order.orderno = pb_order.orderno();
        order.tradedqty = pb_order.tradedqty();
        order.applseqnum = pb_order.applseqnum();

        order.datamultiplepowerof10 = pb_order.datamultiplepowerof10();

        return order;
    }

    // 转换函数：将 protobuf MDTransaction 转换为 MDTransactionStruct
    MDTransactionStruct convert_to_transaction(const com::htsc::mdc::insight::model::MDTransaction& pb_txn) {
        MDTransactionStruct txn = {};

        strncpy(txn.htscsecurityid, pb_txn.htscsecurityid().c_str(), sizeof(txn.htscsecurityid) - 1);
        txn.mddate = pb_txn.mddate();
        txn.mdtime = pb_txn.mdtime();
        txn.securityidsource = pb_txn.securityidsource();
        txn.securitytype = pb_txn.securitytype();

        txn.tradeindex = pb_txn.tradeindex();
        txn.tradebuyno = pb_txn.tradebuyno();
        txn.tradesellno = pb_txn.tradesellno();

        txn.tradetype = pb_txn.tradetype();
        txn.tradebsflag = pb_txn.tradebsflag();
        txn.tradeprice = pb_txn.tradeprice();
        txn.tradeqty = pb_txn.tradeqty();
        txn.trademoney = pb_txn.trademoney();

        txn.applseqnum = pb_txn.applseqnum();
        txn.channelno = pb_txn.channelno();

        txn.datamultiplepowerof10 = pb_txn.datamultiplepowerof10();

        return txn;
    }

    // 优化建议：传入引用 &out，复用内存，避免栈上对象的反复构造和销毁
    void convert_to_order_fast(const com::htsc::mdc::insight::model::MDOrder& pb_order, MDOrderStruct& out) {
        // 【关键优化1】移除 memset/初始化
        // 严禁写 MDOrderStruct out = {}; 
        // 我们假设 pb_order 会覆盖所有关键字段。如果结构体有 padding，脏数据不影响逻辑。

        // 【关键优化2】极速字符串拷贝
        const std::string& sec_id = pb_order.htscsecurityid();
        // 使用 std::min 避免缓冲区溢出，使用 memcpy 避免扫描
        size_t len = std::min(sec_id.size(), sizeof(out.htscsecurityid) - 1);
        std::memcpy(out.htscsecurityid, sec_id.data(), len);
        out.htscsecurityid[len] = '\0'; // 手动封口

        // 【关键优化3】标量赋值 (Scalar Assignment)
        // 现代编译器会将这些连续赋值优化为向量指令 (SIMD Store)
        out.mddate = pb_order.mddate();
        out.mdtime = pb_order.mdtime();
        out.securityidsource = pb_order.securityidsource();
        out.securitytype = pb_order.securitytype();
        out.orderindex = pb_order.orderindex();
        out.ordertype = pb_order.ordertype();
        out.orderprice = pb_order.orderprice();
        out.orderqty = pb_order.orderqty();
        out.orderbsflag = pb_order.orderbsflag();
        out.channelno = pb_order.channelno();
        out.orderno = pb_order.orderno();
        out.tradedqty = pb_order.tradedqty();
        out.applseqnum = pb_order.applseqnum();
        out.datamultiplepowerof10 = pb_order.datamultiplepowerof10();
    }

    void convert_to_transaction_fast(const com::htsc::mdc::insight::model::MDTransaction& pb_txn, MDTransactionStruct& out) {
        // 【关键优化1】不进行 memset 清零

        // 【关键优化2】极速字符串拷贝
        const std::string& sec_id = pb_txn.htscsecurityid();
        size_t len = std::min(sec_id.size(), sizeof(out.htscsecurityid) - 1);
        std::memcpy(out.htscsecurityid, sec_id.data(), len);
        out.htscsecurityid[len] = '\0';

        // 【关键优化3】标量赋值
        out.mddate = pb_txn.mddate();
        out.mdtime = pb_txn.mdtime();
        out.securityidsource = pb_txn.securityidsource();
        out.securitytype = pb_txn.securitytype();
        out.tradeindex = pb_txn.tradeindex();
        out.tradebuyno = pb_txn.tradebuyno();
        out.tradesellno = pb_txn.tradesellno();
        out.tradetype = pb_txn.tradetype();
        out.tradebsflag = pb_txn.tradebsflag();
        out.tradeprice = pb_txn.tradeprice();
        out.tradeqty = pb_txn.tradeqty();
        out.trademoney = pb_txn.trademoney();
        out.applseqnum = pb_txn.applseqnum();
        out.channelno = pb_txn.channelno();
        out.datamultiplepowerof10 = pb_txn.datamultiplepowerof10();
    }

    // ==========================================
    // 转换函数
    // ==========================================
    void convert_to_stock_fast(const com::htsc::mdc::insight::model::MDStock& pb_stock, MDStockStruct& stock) {
        // 1. 字符串处理 (优化 strncpy)
        const std::string& sec_id = pb_stock.htscsecurityid();
        size_t id_len = std::min(sec_id.size(), sizeof(stock.htscsecurityid) - 1);
        std::memcpy(stock.htscsecurityid, sec_id.data(), id_len);
        stock.htscsecurityid[id_len] = '\0';

        // 2. 标量字段直接赋值 (编译器会自动生成向量化指令)
        stock.mddate = pb_stock.mddate();
        stock.mdtime = pb_stock.mdtime();
        stock.datatimestamp = pb_stock.datatimestamp();
        stock.securityidsource = pb_stock.securityidsource();
        stock.securitytype = pb_stock.securitytype();
        
        // 字符处理
        const std::string& phase = pb_stock.tradingphasecode();
        stock.tradingphasecode = phase.empty() ? '\0' : phase[0];

        // 价格与统计
        stock.maxpx = pb_stock.maxpx();
        stock.minpx = pb_stock.minpx();
        stock.preclosepx = pb_stock.preclosepx();
        stock.numtrades = pb_stock.numtrades();
        stock.totalvolumetrade = pb_stock.totalvolumetrade();
        stock.totalvaluetrade = pb_stock.totalvaluetrade();
        stock.lastpx = pb_stock.lastpx();
        stock.openpx = pb_stock.openpx();
        stock.closepx = pb_stock.closepx();
        stock.highpx = pb_stock.highpx();
        stock.lowpx = pb_stock.lowpx();

        stock.totalbuyqty = pb_stock.totalbuyqty();
        stock.totalsellqty = pb_stock.totalsellqty();
        stock.weightedavgbuypx = pb_stock.weightedavgbuypx();
        stock.weightedavgsellpx = pb_stock.weightedavgsellpx();
        
        stock.withdrawbuynumber = pb_stock.withdrawbuynumber();
        stock.withdrawbuyamount = pb_stock.withdrawbuyamount();
        stock.withdrawbuymoney = pb_stock.withdrawbuymoney();
        stock.withdrawsellnumber = pb_stock.withdrawsellnumber();
        stock.withdrawsellamount = pb_stock.withdrawsellamount();
        stock.withdrawsellmoney = pb_stock.withdrawsellmoney();
        
        stock.totalbuynumber = pb_stock.totalbuynumber();
        stock.totalsellnumber = pb_stock.totalsellnumber();
        stock.numbuyorders = pb_stock.numbuyorders();
        stock.numsellorders = pb_stock.numsellorders();
        stock.channelno = pb_stock.channelno();
        stock.datamultiplepowerof10 = pb_stock.datamultiplepowerof10();

        // ==================================================
        // 3. 数组处理 (核心性能优化点)
        // ==================================================

        // 3.1 买卖盘口 (固定 10 档)
        // 使用简化宏，处理不足 10 档的情况
        COPY_FIXED_10(stock.buypricequeue, pb_stock.buypricequeue());
        COPY_FIXED_10(stock.buyorderqtyqueue, pb_stock.buyorderqtyqueue());
        COPY_FIXED_10(stock.sellpricequeue, pb_stock.sellpricequeue());
        COPY_FIXED_10(stock.sellorderqtyqueue, pb_stock.sellorderqtyqueue());

        // 3.2 订单队列 (上限 50 档，使用 count 追踪)
        // 这里使用上限 50，并自动更新 count 变量
        COPY_AND_COUNT(stock.buyorderqueue,     stock.buyorderqueue_count,     pb_stock.buyorderqueue(),     50);
        COPY_AND_COUNT(stock.sellorderqueue,    stock.sellorderqueue_count,    pb_stock.sellorderqueue(),    50);
        
        // 3.3 订单数量队列 (上限 50 档)
        COPY_AND_COUNT(stock.buynumordersqueue, stock.buynumordersqueue_count, pb_stock.buynumordersqueue(), 50);
        COPY_AND_COUNT(stock.sellnumordersqueue, stock.sellnumordersqueue_count, pb_stock.sellnumordersqueue(), 50);
    }

    // ==========================================
    // 转换函数：将 protobuf ADOrderbookSnapshot 转换为 MDOrderbookStruct
    // ==========================================
    void convert_to_orderbook_snapshot_fast(
        const com::htsc::mdc::insight::model::ADOrderbookSnapshot& pb_snap,
        MDOrderbookStruct& out) {

        // 字符串拷贝
        const std::string& sec_id = pb_snap.htscsecurityid();
        size_t len = std::min(sec_id.size(), sizeof(out.htscsecurityid) - 1);
        std::memcpy(out.htscsecurityid, sec_id.data(), len);
        out.htscsecurityid[len] = '\0';

        // 基本字段
        out.mddate = pb_snap.mddate();
        out.mdtime = pb_snap.mdtime();
        out.datatimestamp = pb_snap.datatimestamp();
        const std::string& phase = pb_snap.tradingphasecode();
        out.tradingphasecode = phase.empty() ? '\0' : phase[0];
        out.securityidsource = pb_snap.securityidsource();
        out.securitytype = pb_snap.securitytype();
        out.channelno = pb_snap.channelno();
        out.applseqnum = pb_snap.applseqnum();
        out.snapshotmddatetime = pb_snap.snapshotmddatetime();

        // 价格和统计
        out.numtrades = pb_snap.numtrades();
        out.totalvolumetrade = pb_snap.totalvolumetrade();
        out.totalvaluetrade = pb_snap.totalvaluetrade();
        out.lastpx = pb_snap.lastpx();
        out.highpx = pb_snap.highpx();
        out.lowpx = pb_snap.lowpx();
        out.maxpx = pb_snap.maxpx();
        out.minpx = pb_snap.minpx();
        out.preclosepx = pb_snap.preclosepx();
        out.openpx = pb_snap.openpx();
        out.closepx = pb_snap.closepx();

        // 买卖统计
        out.totalbuyqty = pb_snap.totalbuyqty();
        out.totalsellqty = pb_snap.totalsellqty();
        out.weightedavgbuypx = pb_snap.weightedavgbuypx();
        out.weightedavgsellpx = pb_snap.weightedavgsellpx();
        out.totalbuynumber = pb_snap.totalbuynumber();
        out.totalsellnumber = pb_snap.totalsellnumber();
        out.numbuyorders = pb_snap.numbuyorders();
        out.numsellorders = pb_snap.numsellorders();

        // 买盘档位
        int buy_count = std::min(pb_snap.buyentries_size(), 10);
        out.buyentries_count = buy_count;
        for (int i = 0; i < buy_count; ++i) {
            const auto& entry = pb_snap.buyentries(i);
            out.buyentries[i].level = entry.level();
            out.buyentries[i].price = entry.price();
            out.buyentries[i].totalqty = entry.totalqty();
            out.buyentries[i].numberoforders = entry.numberoforders();
        }

        // 卖盘档位
        int sell_count = std::min(pb_snap.sellentries_size(), 10);
        out.sellentries_count = sell_count;
        for (int i = 0; i < sell_count; ++i) {
            const auto& entry = pb_snap.sellentries(i);
            out.sellentries[i].level = entry.level();
            out.sellentries[i].price = entry.price();
            out.sellentries[i].totalqty = entry.totalqty();
            out.sellentries[i].numberoforders = entry.numberoforders();
        }

        out.datamultiplepowerof10 = pb_snap.datamultiplepowerof10();
    }

    // ==========================================
    // OrderBook Snapshot 处理（备份/对比用）
    // ==========================================
    void on_orderbook_snapshot(const com::htsc::mdc::insight::model::ADOrderbookSnapshot& snapshot) {
        // 转换并转发到策略引擎
        MDOrderbookStruct ob_struct;
        convert_to_orderbook_snapshot_fast(snapshot, ob_struct);
        // 持久化 (入队，不阻塞)
        if (persist_) persist_->log_snapshot(ob_struct);
        engine_->on_market_orderbook_snapshot(ob_struct);

        const std::string& security_id = snapshot.htscsecurityid();

        // 采样控制：每100000次 或 每10分钟 打印一次
        static std::atomic<uint64_t> snapshot_count{0};
        static std::atomic<int64_t> last_log_time{0};  // 上次打印时间（秒）

        uint64_t count = snapshot_count.fetch_add(1, std::memory_order_relaxed);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t last_time = last_log_time.load(std::memory_order_relaxed);

        bool should_log = (count % 100000 == 0) || (now - last_time >= 600);  // 600秒 = 10分钟

        if (should_log) {
            last_log_time.store(now, std::memory_order_relaxed);

            // 计算价格乘数
            int32_t power = snapshot.datamultiplepowerof10();
            double multiplier = 1.0;
            if (power > 0) {
                for (int i = 0; i < power; ++i) multiplier *= 10.0;
            } else if (power < 0) {
                for (int i = 0; i < -power; ++i) multiplier /= 10.0;
            }

            // 打印基本信息
            LOG_MODULE_DEBUG(hft::logger::get_logger(), "Market", "[OB_SNAP] sym={} time={} last={:.2f} vol={} cnt={}",
                security_id, snapshot.mdtime(),
                snapshot.lastpx() * multiplier,
                snapshot.totalvolumetrade(),
                count);

            // 打印买盘10档
            int buy_levels = std::min(snapshot.buyentries_size(), 10);
            for (int i = 0; i < buy_levels; ++i) {
                const auto& entry = snapshot.buyentries(i);
                LOG_MODULE_DEBUG(hft::logger::get_logger(), "Market", "[OB_SNAP] sym={} BID[{}] px={:.2f} qty={} orders={}",
                    security_id, i + 1,
                    entry.price() * multiplier,
                    entry.totalqty(),
                    entry.numberoforders());
            }

            // 打印卖盘10档
            int sell_levels = std::min(snapshot.sellentries_size(), 10);
            for (int i = 0; i < sell_levels; ++i) {
                const auto& entry = snapshot.sellentries(i);
                LOG_MODULE_DEBUG(hft::logger::get_logger(), "Market", "[OB_SNAP] sym={} ASK[{}] px={:.2f} qty={} orders={}",
                    security_id, i + 1,
                    entry.price() * multiplier,
                    entry.totalqty(),
                    entry.numberoforders());
            }
        }
    }
};

#endif // LIVE_MARKET_ADAPTER_H
