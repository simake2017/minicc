#pragma once
// 整型常量折叠与传播，结果写回 AST。
// 参考 clang/AST/ExprConstant.cpp、CodeGen/CGExprConstant.cpp。
// 仅处理可求值的节点；非常量、除零及需保持地址语义的子树原样保留。
// 例：2 + 3 → 5；true ? 11 : 22 → 11；x + 1 保留。

#include "ast.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace minicc {

class ConstantFolder {
public:
    // 可能返回新节点，调用方必须接住返回值；仅在发生替换时记录日志。
    ExprPtr fold(ExprPtr expr);

    // NTTP 求值失败返回 nullopt；成功时同时保留数值与类型。
    // 例：K<4L> 与 K<4> 的值相同，类型不同，不能共用实例键。
    struct IntegralValue { int64_t value; TypePtr type; };
    std::optional<IntegralValue> evaluateIntegral(ExprPtr expr);

    // 由 Sema 提供 const/constexpr 整型变量的值；未知名字返回 nullopt。
    // 参考 clang DeclRefExpr 对声明常量值的查询。
    using ConstResolver = std::function<std::optional<IntegralValue>(const std::string& name)>;
    void setConstResolver(ConstResolver r) { m_constResolver = std::move(r); }

private:
    ConstResolver m_constResolver;
    ExprPtr foldNode(ExprPtr expr);

    // 除零返回 nullopt，保留原表达式。
    static std::optional<int64_t> tryBinary(BinaryOp op, int64_t l, int64_t r);

    // 供折叠日志显示的表达式简写。
    static std::string describe(const ExprPtr& expr);
};

}  // namespace minicc
