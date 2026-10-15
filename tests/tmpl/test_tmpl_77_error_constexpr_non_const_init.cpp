// =============================================================================
// 测试：constexpr 初始化式不可常量求值的负向用例（[dcl.constexpr]/9）
// =============================================================================
// 预期行为：
//   `constexpr int m = touch();` 的初始化式调用了非 constexpr 函数 ——
//   折叠器对它无能为力（Call 节点不折），求值器拿不到整数字面量 ⇒
//   响亮报错，rc=1，文案与 clang++-18 -std=c++20 逐字相同：
//     error: constexpr variable 'm' must be initialized by a constant expression
//
// 考察理论点：
//   · [dcl.constexpr]/9 是【强制义务】而非优化选择：constexpr 承诺
//     "此变量编译期可知"，承诺落空即 ill-formed —— 哪怕运行期完全
//     算得出结果。对照普通 const：`const int m = touch();` 合法。
// =============================================================================

int touch() { return 1; }

int main() {
    constexpr int m = touch();
    return m;
}
