// =============================================================================
// constant_folder.cpp —— 常量折叠实现
// 对照 clang：ExprConstant.cpp（EvaluateAsInt 的裁判逻辑）、
//   CodeGen 前的 ConstantFolding。教学版只折三种节点：
//   BinaryExpr / UnaryExpr / ConditionalExpr，且只在操作数全是字面量时。
// =============================================================================

#include "constant_folder.h"

#include <iostream>
#include <format>

namespace minicc {

// ── 日志用的表达式简写 ─────────────────────────────────────────────────────────
std::string ConstantFolder::describe(const ExprPtr& expr) {
    if (!expr) return "?";
    switch (expr->kind) {
        case NodeKind::IntLiteral:
            return std::format("{}", static_cast<IntLiteralExpr&>(*expr).value);
        case NodeKind::BoolLiteral:
            return static_cast<BoolLiteralExpr&>(*expr).value ? "true" : "false";
        case NodeKind::CharLiteral: {
            auto& c = static_cast<CharLiteralExpr&>(*expr);
            char ch = static_cast<char>(c.value);
            if (ch >= 32 && ch < 127 && ch != '\'' && ch != '\\')
                return std::format("'{}'", ch);
            return std::format("'\\x{:02x}'", c.value & 0xff);
        }
        case NodeKind::Binary: {
            auto& b = static_cast<BinaryExpr&>(*expr);
            const char* opStr = "?";
            switch (b.op) {
                case BinaryOp::Add: opStr = "+";  break;
                case BinaryOp::Sub: opStr = "-";  break;
                case BinaryOp::Mul: opStr = "*";  break;
                case BinaryOp::Div: opStr = "/";  break;
                case BinaryOp::Mod: opStr = "%";  break;
                case BinaryOp::Eq:  opStr = "=="; break;
                case BinaryOp::Neq: opStr = "!="; break;
                case BinaryOp::Lt:  opStr = "<";  break;
                case BinaryOp::Gt:  opStr = ">";  break;
                case BinaryOp::Le:  opStr = "<="; break;
                case BinaryOp::Ge:  opStr = ">="; break;
                case BinaryOp::And: opStr = "&&"; break;
                case BinaryOp::Or:  opStr = "||"; break;
            }
            return std::format("({} {} {})", describe(b.left), opStr, describe(b.right));
        }
        case NodeKind::Unary: {
            auto& u = static_cast<UnaryExpr&>(*expr);
            return std::format("({}{})", u.op == UnaryOp::Neg ? "-" : "!",
                               describe(u.operand));
        }
        case NodeKind::Conditional: {
            auto& c = static_cast<ConditionalExpr&>(*expr);
            return std::format("({} ? {} : {})", describe(c.condition),
                               describe(c.thenExpr), describe(c.elseExpr));
        }
        case NodeKind::Comma: {
            auto& c = static_cast<CommaExpr&>(*expr);
            return std::format("({}, {})", describe(c.left), describe(c.right));
        }
        case NodeKind::Call: {
            auto& c = static_cast<CallExpr&>(*expr);
            std::string name = "?";
            if (c.callee && c.callee->kind == NodeKind::Var)
                name = static_cast<VarExpr&>(*c.callee).name;
            std::string args;
            for (size_t i = 0; i < c.arguments.size(); i++) {
                if (i) args += ", ";
                args += describe(c.arguments[i]);
            }
            return std::format("{}({})", name, args);
        }
        default:
            return "<expr>";
    }
}

// ── 纯函数试算 ────────────────────────────────────────────────────────────────
std::optional<int64_t> ConstantFolder::tryBinary(BinaryOp op, int64_t l, int64_t r) {
    switch (op) {
        case BinaryOp::Add: return l + r;
        case BinaryOp::Sub: return l - r;
        case BinaryOp::Mul: return l * r;
        case BinaryOp::Div: return r == 0 ? std::nullopt : std::optional<int64_t>(l / r);
        case BinaryOp::Mod: return r == 0 ? std::nullopt : std::optional<int64_t>(l % r);
        case BinaryOp::Eq:  return l == r;
        case BinaryOp::Neq: return l != r;
        case BinaryOp::Lt:  return l < r;
        case BinaryOp::Gt:  return l > r;
        case BinaryOp::Le:  return l <= r;
        case BinaryOp::Ge:  return l >= r;
        case BinaryOp::And: return (l != 0 && r != 0);
        case BinaryOp::Or:  return (l != 0 || r != 0);
    }
    return std::nullopt;
}

// ── 递归折叠 ──────────────────────────────────────────────────────────────────
ExprPtr ConstantFolder::foldNode(ExprPtr expr) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        // ── 二元表达式：子树先折，双字面量则试算 ──
        // demo：(2 + 3) + x ⇒ 5 + x（左子树折掉，本节点保留，换过孩子）
        case NodeKind::Binary: {
            auto& b = static_cast<BinaryExpr&>(*expr);
            ExprPtr l = foldNode(b.left);
            ExprPtr r = foldNode(b.right);
            // ★ 先回写孩子再判定/打日志：孩子可能被常量传播【换成新节点】
            //   （VarExpr → 字面量），describe(expr) 读的是 b.left/b.right，
            //   不回写会对着旧节点描述（VarExpr 落进 default 打成 <expr>）。
            b.left = std::move(l);
            b.right = std::move(r);

            auto* li = (b.left && b.left->kind == NodeKind::IntLiteral)
                           ? &static_cast<IntLiteralExpr&>(*b.left) : nullptr;
            auto* ri = (b.right && b.right->kind == NodeKind::IntLiteral)
                           ? &static_cast<IntLiteralExpr&>(*b.right) : nullptr;
            if (li && ri) {
                if (auto v = tryBinary(b.op, li->value, ri->value)) {
                    // 比较/逻辑运算的产物是 bool 字面量，算术的是 int 字面量
                    const bool isBool = (b.op >= BinaryOp::Eq && b.op <= BinaryOp::Or);
                    std::cout << std::format("  [fold] {} → {}\n",
                                             describe(expr), *v);
                    if (isBool) return std::make_shared<BoolLiteralExpr>(*v != 0);
                    return std::make_shared<IntLiteralExpr>(*v);
                }
                // 除零：不折（保留运行期语义，见头注 3）
            }
            return expr;
        }

        // ── 一元表达式：-字面量 / !字面量 ──
        // ★ 只折 Neg/Not：Addr（&x）/ Deref（*p）的操作数必须是真 lvalue，
        //   折成字面量会让 codegen 生成野地址 —— 整棵子树原样保留。
        case NodeKind::Unary: {
            auto& u = static_cast<UnaryExpr&>(*expr);
            if (u.op != UnaryOp::Neg && u.op != UnaryOp::Not) return expr;
            u.operand = foldNode(u.operand);
            auto* li = (u.operand && u.operand->kind == NodeKind::IntLiteral)
                           ? &static_cast<IntLiteralExpr&>(*u.operand) : nullptr;
            if (li) {
                if (u.op == UnaryOp::Neg) {
                    std::cout << std::format("  [fold] {} → {}\n",
                                             describe(expr), -li->value);
                    return std::make_shared<IntLiteralExpr>(-li->value);
                }
                if (u.op == UnaryOp::Not) {
                    std::cout << std::format("  [fold] {} → {}\n",
                                             describe(expr), (li->value == 0) ? "true" : "false");
                    return std::make_shared<BoolLiteralExpr>(li->value == 0);
                }
            }
            return expr;
        }

        // ── 三元表达式：常量条件 ⇒ 死分支消除 ──
        // demo：true ? 11 : 22 ⇒ 11（else 支整体消掉，副作用不会生成）
        case NodeKind::Conditional: {
            auto& c = static_cast<ConditionalExpr&>(*expr);
            c.condition = foldNode(c.condition);
            c.thenExpr  = foldNode(c.thenExpr);
            c.elseExpr  = foldNode(c.elseExpr);

            // 条件折成字面量（bool 或 int 都可当条件，与 codegen testq 一致）
            const ExprPtr& cond = c.condition;
            int64_t cv = 0;
            bool haveConst = false;
            if (cond && cond->kind == NodeKind::BoolLiteral) {
                cv = static_cast<BoolLiteralExpr&>(*cond).value ? 1 : 0;
                haveConst = true;
            } else if (cond && cond->kind == NodeKind::IntLiteral) {
                cv = static_cast<IntLiteralExpr&>(*cond).value;
                haveConst = true;
            }
            if (haveConst) {
                ExprPtr picked = (cv != 0) ? c.thenExpr : c.elseExpr;
                std::cout << std::format("  [fold] {} → {}\n",
                                         describe(expr), describe(picked));
                return picked;
            }
            return expr;
        }

        // ── 变量引用：查编译期常量表 ⇒ 常量传播 ──────────────────────
        // `const int c = 4; int x = c * 2;` 里 c 在此处替换成字面量 4，
        // 外层二元节点下一轮自然折成 8 —— 传播与折叠交替，正是
        // 教科书"constant propagation + folding"的流水线形态。
        // 查不到（普通变量 / 变量是类的对象 / 指针const 等）原样保留。
        case NodeKind::Var: {
            if (!m_constResolver) return expr;
            auto& v = static_cast<VarExpr&>(*expr);
            auto cv = m_constResolver(v.name);
            if (!cv.has_value()) return expr;
            std::cout << std::format(
                "  [fold] {} → {} （常量传播：编译期常量表命中）\n",
                v.name, cv->value);
            return std::make_shared<IntLiteralExpr>(cv->value, cv->type);
        }

        // ── 逗号表达式：左操作数纯字面量（无副作用）⇒ 丢弃，值即右操作数 ──
        // 与恒定三元同款死代码消除；左操作数有副作用（调用/赋值…）则保留。
        case NodeKind::Comma: {
            auto& cm = static_cast<CommaExpr&>(*expr);
            cm.left  = foldNode(cm.left);
            cm.right = foldNode(cm.right);
            const bool leftPure =
                cm.left && (cm.left->kind == NodeKind::IntLiteral
                            || cm.left->kind == NodeKind::BoolLiteral
                            || cm.left->kind == NodeKind::CharLiteral);
            if (leftPure) {
                std::cout << std::format("  [fold] {} → {}\n",
                                         describe(expr), describe(cm.right));
                return cm.right;
            }
            return expr;
        }

        // ── 其他节点：本折迭代不深入（Call/Member 等保持原样）──
        default:
            return expr;
    }
}

ExprPtr ConstantFolder::fold(ExprPtr expr) {
    if (!expr) return nullptr;
    return foldNode(std::move(expr));
}

// ── NTTP 实参求值 ─────────────────────────────────────────────────────────────
std::optional<ConstantFolder::IntegralValue>
ConstantFolder::evaluateIntegral(ExprPtr expr) {
    ExprPtr folded = fold(std::move(expr));
    if (!folded) return std::nullopt;
    if (folded->kind == NodeKind::IntLiteral) {
        auto& lit = static_cast<IntLiteralExpr&>(*folded);
        // 形态：字面量自己带的（4L 是 long）；老构造点没标 ⇒ int（[lex.icon]/2）
        return IntegralValue{lit.value, lit.literalType ? lit.literalType
                                                        : Type::makeInt()};
    }
    if (folded->kind == NodeKind::BoolLiteral)
        return IntegralValue{static_cast<BoolLiteralExpr&>(*folded).value ? 1 : 0,
                             Type::makeBool()};
    if (folded->kind == NodeKind::CharLiteral)
        return IntegralValue{static_cast<CharLiteralExpr&>(*folded).value,
                             Type::makeChar()};
    return std::nullopt;
}

}  // namespace minicc
