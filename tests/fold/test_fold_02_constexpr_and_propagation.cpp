// =============================================================================
// 测试：constexpr 关键字 + 常量传播（主线 D 二批）
// =============================================================================
// 预期行为：
//   `constexpr int k = 2+3;` 的初始化式折叠成 5 后登记【编译期常量表】
//   （[const] ★ 日志行）；此后 k 出现在任何常量表达式里都触发
//   【常量传播】（`[fold] k → 5` 行）—— `int x = k * 2;` 折成字面量 10，
//   这是教科书"constant propagation + folding 交替流水线"的最小形态。
//   constexpr 的初始化式不可常量求值时报错，文案与 clang 逐字同：
//     constexpr variable 'm' must be initialized by a constant expression
//   全部检查点通过返回 0，任一失败返回该检查点编号。
//
// 考察理论点：
//   · [dcl.constexpr]/9：constexpr 变量的初始化式【必须】是常量表达式
//     （编译器义务，不是优化）—— 与普通 const 的"可以运行期初始化"对比。
//   · constexpr 隐含 const（parseType 前缀处理）。
//   · 常量传播的地址边界：`&b` / `*p` 的操作数【不折】（折成字面量会让
//     codegen 生成野地址）—— Unary 折叠只收 Neg/Not。
// =============================================================================

int touch() { return 1; }

int main() {
    // 1. constexpr 局部变量：初始化式折叠 + 登记
    constexpr int k = 2 + 3;
    if (k != 5) return 1;

    // 2. 常量传播：k 在表达式里替换成 5 ⇒ 6 * 5 折成 30
    int x = 6 * k;
    if (x != 30) return 2;

    // 3. 传播 + 折叠交替：(k + 1) * (k - 1) ⇒ 6 * 4 ⇒ 24
    int y = (k + 1) * (k - 1);
    if (y != 24) return 3;

    // 4. constexpr 布尔量参与三元：条件传播后整棵折死
    constexpr bool big = k > 100;
    int z = big ? 1 : 2;
    if (z != 2) return 4;

    // 5. 取地址边界：const 变量的地址【不折】，解引用照常工作
    const int b = 7;
    const int* pb = &b;
    if (*pb != 7) return 5;

    // 6. 普通 const（非 constexpr）同样登记传播 —— const int c 也是编译期已知
    const int c = 4;
    int w = c * 2;
    if (w != 8) return 6;

    return 0;
}
