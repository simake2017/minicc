// =============================================================================
// 测试：NTTP 常量表达式实参的负向用例（[expr.const] 不成立时报错）
// =============================================================================
// 预期行为：
//   Buf<1/0> 中除法在常量求值下是【不可能的】—— 折叠器不替程序决定
//   UB（除零不折，原样保留），求值器看到非字面量树 ⇒ 响亮报错，
//   rc=1，文案与 clang++-18 -std=c++20 逐字相同：
//     error: non-type template argument is not a constant expression
//
// 考察理论点：
//   · [expr.const]/5 的 core constant expression 禁除零 —— 折叠器的
//     "除零不折"（tryBinary 返回 nullopt）与求值器的"非字面量即失败"
//     两道工序接力，正好把这条语义兜住（对照 clang
//     lib/AST/ExprConstant.cpp 的 IntExprEvaluator::VisitBinDiv 报
//     division by zero）。
// =============================================================================

template<int N> struct Buf {
    int size() { return N; }
};

int main() {
    Buf<1 / 0> a;
    return a.size();
}
