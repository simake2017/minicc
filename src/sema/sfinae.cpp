// SFINAE 上下文与统一诊断出口，实现协议见 sfinae.h。
// 例：候选替换失败记录移除原因；decltype 探测失败转换为替换失败。

#include "sfinae.h"

#include <format>
#include <iostream>

namespace minicc {

// 直接上下文深度由 RAII 成对维护。
int SfinaeContext::s_depth = 0;

SfinaeContext::SfinaeContext()  { ++s_depth; }
SfinaeContext::~SfinaeContext() { --s_depth; }

bool SfinaeContext::inImmediateContext() { return s_depth > 0; }
int  SfinaeContext::depth()              { return s_depth; }

// 产生信号只记录上下文；是否吸收由外层候选尝试边界决定。
void Sfinae::fail(const std::string& reason) {
    std::cout << std::format(
        "  [sfinae] ⚡ 发出替换失败信号（{}）：{}\n",
        SfinaeContext::inImmediateContext() ? "直接上下文内 → 可被吸收为软失败"
                                            : "直接上下文【外】→ 无人吸收则升级为硬错误",
        reason);
    throw SubstitutionFailure(reason);
}

// ─────────────────────────────────────────────────────────────────────────────
// 候选被移除
// ─────────────────────────────────────────────────────────────────────────────
void Sfinae::rejected(const std::string& candidateDesc, const std::string& reason,
                      const std::string& indent) {
    std::cout << std::format(
        "{}  [sfinae] ⤵ 候选【{}】被移出候选集（SFINAE 软失败，非错误）\n"
        "{}  [sfinae]    原因：{}\n",
        indent, candidateDesc, indent, reason);
}

// void_t 日志保留每项探测结果，便于定位使候选失效的条件。
void Sfinae::probeBegin(size_t argCount) {
    std::cout << std::format(
        "  [sfinae] void_t<{} 个实参> —— 逐个替换 + 求值探测\n", argCount);
}

void Sfinae::probeStep(size_t index1based, const std::string& gotType) {
    std::cout << std::format("  [sfinae]   ├─ 实参 {} 探测通过 → {}\n",
                             index1based, gotType);
}

void Sfinae::probeAllOk() {
    std::cout << "  [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓\n";
}

void Sfinae::probeNoArgs() {
    std::cout << "  [sfinae] void_t<> 无实参 ⇒ 归约为 void ✓\n";
}

void Sfinae::probeNoEvaluator() {
    std::cout << "  [sfinae] void_t: no evaluator attached — 跳过求值\n";
}

// decltype 探测边界将求值错误转换为替换失败；其他上下文仍按硬错误处理。
void Sfinae::demote(const std::string& innerError) {
    std::cout << std::format(
        "  [sfinae] ⟲ decltype 操作数在内层报错，降级为【软失败】:\n"
        "  [sfinae]    {}\n", innerError);
}

} // namespace minicc
