// =============================================================================
// 测试：NTTP 常量表达式实参（主线 D —— Buf<2+2>）
// =============================================================================
// 预期行为：
//   非类型模板实参可以是常量表达式（[temp.arg.nontype]/1 的 converted
//   constant expression）：Parser 只把表达式树存进实参（[parse:targ] ★
//   constant expression 行），Sema 在【算缓存键之前】求值归一（
//   [sema:targ] ⤷ 常量表达式实参求值行），此后与普通字面量实参完全同形。
//   `Buf<2+2>` 与 `Buf<4>` 是【同一个实例】（缓存键一致）；
//   `template<auto V>` 下形态区分仍然成立：K<2+2> 是 K<4:int>、
//   K<4L> 是 K<4:long>，两个实例（docs/learn/35 的撞键教训）。
//   全部检查点通过返回 0，任一失败返回该检查点编号。
//
// 考察理论点：
//   · [expr.const]/10 的裁决权在语义期 —— Parser 不评价（对照 clang：
//     Parser 存 TemplateArgument::Expression 形态，Sema 的
//     CheckTemplateArgument 里 EvaluateAsInt 转 Value 形态）。
//   · 求值必须在缓存键计算【之前】，否则 Buf<2+2> 与 Buf<4> 各建一份
//     （键里值位是占位 0，先来者把缓存占了）。
//   · 模板实参表内的 '>' 歧义（[stmt.ambig] 的实参表版）：comparison 层
//     在实参表达式里遇裸 '>' 必须停（它是收尾不是大于号）；括号内豁免。
// 回归红线：test_tmpl_61..71 永远全绿（字面量 fast path 逐字节不变）。
// =============================================================================

template<int N> struct Buf {
    int size() { return N; }
};

template<bool B> struct Flag {
    int pick() { return B ? 10 : 20; }
};

template<auto V> struct K {
    int big() { return V > 100 ? 1 : 2; }
};

int main() {
    // 1. 加法表达式实参：Buf<2+2> ⇒ 实例 Buf<4>
    Buf<2 + 2> a;
    if (a.size() != 4) return 1;

    // 2. 括号 + 混合运算：Buf<(1+2)*3> ⇒ Buf<9>
    Buf<(1 + 2) * 3> b;
    if (b.size() != 9) return 2;

    // 3. 一元负号入表达式：Buf<-3+10> ⇒ Buf<7>
    Buf<-3 + 10> c;
    if (c.size() != 7) return 3;

    // 4. 除法 + bool 形参：Flag<8/8> ⇒ Flag<true>
    Flag<8 / 8> d;
    if (d.pick() != 10) return 4;

    // 5. 同一实例缓存：Buf<2+2> 与 Buf<4> 同键（cache hit 日志可见）
    Buf<4> e;
    if (e.size() != 4) return 5;

    // 6. auto 形参形态区分：K<2+2>（int）与 K<4L>（long）是两个实例
    K<2 + 2> k1;
    K<4L>    k2;
    if (k1.big() != 2) return 6;
    if (k2.big() != 2) return 7;

    return 0;
}
