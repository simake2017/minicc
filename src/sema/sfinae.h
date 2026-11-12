#pragma once

// SFINAE 协议：直接上下文中的替换失败移除候选，硬错误继续传播。
// 参考 clang/Sema/SemaTemplateDeduction.cpp 与 Sema::SFINAETrap。
// 例：缺失 typename T::type 可使候选失效；无关 runtime_error 不被吸收。
// SfinaeContext 用 RAII 跟踪上下文，SfinaeSink 统一处理失败日志。

#include <cstddef>     // size_t —— 本文件是唯一在【函数声明】里用 size_t 的头文件，
#include <stdexcept>
#include <string>
#include <utility>

namespace minicc {

// 可恢复的替换失败信号，只由候选尝试边界捕获。
class SubstitutionFailure : public std::runtime_error {
public:
    explicit SubstitutionFailure(const std::string& what)
        : std::runtime_error(what) {}
};

// RAII 记录直接上下文深度；嵌套尝试结束后恢复原深度。
class SfinaeContext {
public:
    SfinaeContext();
    ~SfinaeContext();
    SfinaeContext(const SfinaeContext&) = delete;
    SfinaeContext& operator=(const SfinaeContext&) = delete;

    static bool inImmediateContext();
    static int  depth();

private:
    static int s_depth;
};

// 候选尝试边界及日志出口。
class Sfinae {
public:
    // 产生替换失败信号；是否可恢复由调用方的候选边界决定。
    [[noreturn]] static void fail(const std::string& reason);

    // 在直接上下文中尝试候选，只吸收 SubstitutionFailure。
    template <typename Fn>
    static bool attempt(const std::string& candidateDesc, Fn&& fn,
                        std::string* outReason = nullptr) {
        SfinaeContext ctx;                 // ← 进入直接上下文
        try {
            fn();
            return true;
        } catch (const SubstitutionFailure& e) {
            if (outReason) *outReason = e.what();
            rejected(candidateDesc, e.what());
            return false;
        }
        // 不捕获 std::runtime_error 等其它异常 —— 那是硬错误，必须让它穿过去。
        // 这正是"软/硬"分界的代码形态。
    }

    // 候选移除的统一日志出口。
    static void rejected(const std::string& candidateDesc, const std::string& reason,
                         const std::string& indent = "");

    // ── void_t 探测链路 ──
    static void probeBegin(size_t argCount);
    static void probeStep(size_t index1based, const std::string& gotType);
    static void probeAllOk();
    static void probeNoArgs();
    static void probeNoEvaluator();

    // ── decltype 求值失败被降级为软失败 ──
    static void demote(const std::string& innerError);
};

} // namespace minicc
