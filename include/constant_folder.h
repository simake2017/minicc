#pragma once
// =============================================================================
// constant_folder.h —— 常量折叠（主线 D）
// =============================================================================
// 学习要点：
//   1. 【折叠是什么】把"操作数全是字面量"的表达式在【编译期】算成一个
// 对照 clang：clang/lib/AST/ExprConstant.cpp（clang 的常量求值器；本版只做整型家族——【简化】）
//      字面量：2 + 3 ⇒ 5。运行期就不用再做这次运算了 —— 这是编译器
//      最古老、收益最稳定的优化（常量传播的另一面：折叠之后还能继续
//      传播，见 docs/learn/39 的折叠-传播交替图）。
//   2. 【在哪一层做】clang 在 Sema 阶段随手折（Parse 出的树往上冒时
//      逐层试折），Codegen 前还有 InstCombine 兜底。本项目照 Sema 层
//      做法：语句访问点拿到表达式槽位后先 fold 再定型 —— 折叠结果
//      直接写回 AST，后续阶段看到的就只有字面量。
//   3. 【折不动的要诚实】除零（1/0）、非常量操作数（x + 1）原样保留，
//      交给运行期或报错 —— 折叠器【只许变快，不许变语义】。
//   4. 【短路三元也是常量】`true ? a : b` 的 a 支无条件保留、b 支是死代码 ——
//      常量条件的三元表达式折叠 = 死分支消除的最小形态。
// 对照 clang：lib/AST/ExprConstant.cpp（求值器，constexpr 语义的裁判）、
//   lib/CodeGen/CGExprConstant.cpp（常量表达式的代码生成 = 直接造常量）。
// =============================================================================

#include "ast.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace minicc {

class ConstantFolder {
public:
    // 就地折叠：返回折叠后的表达式树（可能重建了部分节点，调用方必须
    // 接住返回值赋回原槽位）。只在【真的折掉东西】时打日志：
    //   [fold] 2 + 3 → 5
    //   [fold] true ? 11 : 22 → 11   （else 支被消成死代码）
    ExprPtr fold(ExprPtr expr);

    // NTTP 实参求值（[temp.arg.nontype]）：fold 后整树必须是整数字面量
    // （bool/char 也归一成整数值），否则返回 nullopt —— 调用方按
    // "template argument expression is not constant" 报错。
    // ★ 返回值带【形态】：auto 形参 K<4L> 与 K<4> 是两个实例（docs/learn/35），
    //   求值丢了类型会静默撞键。
    struct IntegralValue { int64_t value; TypePtr type; };
    std::optional<IntegralValue> evaluateIntegral(ExprPtr expr);

    // ── 主线 D 二批：编译期常量表查询（常量传播的另一半）───────────────
    // 折叠遇到 VarExpr 时经此回调问"这个名字有没有已知的整数值"
    // （const/constexpr 整型变量，由 Sema 在声明时登记）。查不到返回
    // nullopt ⇒ 变量原样保留（如旧）。对照 clang：常量传播走
    // DeclRefExpr → ValueDecl::getEvaluatedValue（Sema 层的常量值缓存）。
    using ConstResolver = std::function<std::optional<IntegralValue>(const std::string& name)>;
    void setConstResolver(ConstResolver r) { m_constResolver = std::move(r); }

private:
    ConstResolver m_constResolver;
    // 递归主力：单节点折叠（子树先折后试算本节点）。
    ExprPtr foldNode(ExprPtr expr);

    // 纯函数试算：两操作数都是整数字面量时给出运算结果，否则 nullopt。
    // 除零返回 nullopt（不折，保留运行期语义——和 clang 一致，UB 不在
    // 编译期替程序"决定"）。
    static std::optional<int64_t> tryBinary(BinaryOp op, int64_t l, int64_t r);

    // 日志用的表达式简写（只覆盖会出现在折叠日志里的形态：字面量/二元/
    // 一元/三元）。
    static std::string describe(const ExprPtr& expr);
};

}  // namespace minicc
