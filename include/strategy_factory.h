#ifndef STRATEGY_FACTORY_H
#define STRATEGY_FACTORY_H

#include "strategy_engine.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <stdexcept>

// ==========================================
// 策略工厂
// ==========================================
// 根据策略名称字符串动态创建策略实例
//
// 使用方法:
// 1. 在策略头文件末尾使用 REGISTER_STRATEGY 宏注册
// 2. 调用 StrategyFactory::create("StrategyName", "600759.SH") 创建实例

class StrategyFactory {
public:
    using CreatorFunc = std::function<std::unique_ptr<Strategy>(const std::string& symbol)>;

    // 获取单例
    static StrategyFactory& instance() {
        static StrategyFactory factory;
        return factory;
    }

    // 注册策略创建函数
    void register_strategy(const std::string& name, CreatorFunc creator) {
        creators_[name] = std::move(creator);
    }

    // 创建策略实例
    std::unique_ptr<Strategy> create(const std::string& name, const std::string& symbol) const {
        auto it = creators_.find(name);
        if (it == creators_.end()) {
            throw std::runtime_error("Unknown strategy: " + name);
        }
        return it->second(symbol);
    }

    // 检查策略是否已注册
    bool has_strategy(const std::string& name) const {
        return creators_.find(name) != creators_.end();
    }

    // 获取所有已注册的策略名称
    std::vector<std::string> get_registered_strategies() const {
        std::vector<std::string> names;
        names.reserve(creators_.size());
        for (const auto& kv : creators_) {
            names.push_back(kv.first);
        }
        return names;
    }

private:
    StrategyFactory() = default;
    std::unordered_map<std::string, CreatorFunc> creators_;
};

// ==========================================
// 策略注册辅助类
// ==========================================
template<typename T>
class StrategyRegistrar {
public:
    explicit StrategyRegistrar(const std::string& name) {
        StrategyFactory::instance().register_strategy(name,
            [](const std::string& symbol) -> std::unique_ptr<Strategy> {
                return std::make_unique<T>(symbol + "_" + typeid(T).name());
            });
    }
};

// ==========================================
// 策略注册宏
// ==========================================
// 用法: 在策略类定义之后添加
// REGISTER_STRATEGY(PriceLevelVolumeStrategy)
//
// 注意: 由于使用了静态初始化，需要确保策略头文件被包含

#define REGISTER_STRATEGY(ClassName) \
    static StrategyRegistrar<ClassName> _registrar_##ClassName(#ClassName)

#endif // STRATEGY_FACTORY_H
