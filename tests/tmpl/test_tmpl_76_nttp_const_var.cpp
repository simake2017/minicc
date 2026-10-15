// =============================================================================
// 测试：常量变量作 NTTP 实参（主线 D 二批 —— Buf<k>）
// =============================================================================
// 预期行为：
//   `Buf<k>` 里 Parser 没有符号表，把 k 建成了 Class("k") 类型节点；
//   resolveType 见该位形参是 NTTP 时，查【编译期常量表】把实参改写成
//   值实参（[sema:targ] ⤷ 按编译期常量重解 行），此后与 `Buf<5>` 完全
//   同形（同缓存键）。全局 constexpr、函数内 constexpr 都可用；
//   别名模板 `BufA<k>` 走同款重解。`template<auto V>` 下形态随声明类型
//   （constexpr long ⇒ K<4:long>，docs/learn/35 撞键教训的正面应用）。
//   全部检查点通过返回 0，任一失败返回该检查点编号。
//
// 考察理论点：
//   · [temp.arg.nontype]/1：id-expression 形式的常量实参。
//   · 语义期消歧：clang 靠 Sema::isTypeName 在【解析期】区分"类型名 vs
//     常量名"；本实现推迟到语义期用「形参 kind + 常量表」近似 ——
//     与 docs/learn/22 的 [stmt.ambig] 近似同族（解析期没有符号表）。
//   · 查不到常量的裸名字（`Buf<q>`，q 未声明）维持原路径响亮报错。
// 回归红线：test_tmpl_61..75 永远全绿。
// =============================================================================

template<int N> struct Buf {
    int size() { return N; }
};

template<int N> using BufA = Buf<N>;

template<auto V> struct K {
    int big() { return V > 100 ? 1 : 2; }
};

constexpr int kGlobal = 2 + 3;

int main() {
    // 1. 全局 constexpr 作 NTTP 实参：Buf<kGlobal> ⇒ Buf<5>
    Buf<kGlobal> a;
    if (a.size() != 5) return 1;

    // 2. 与字面量实参同实例：Buf<5> 命中缓存（cache hit 日志可见）
    Buf<5> b;
    if (b.size() != 5) return 2;

    // 3. 函数内 constexpr 作实参（声明在引用之前即可）
    constexpr int n = 4;
    Buf<n> c;
    if (c.size() != 4) return 3;

    // 4. 别名模板路径：BufA<kGlobal> 同样重解
    BufA<kGlobal> d;
    if (d.size() != 5) return 4;

    // 5. auto 形参形态随声明类型：constexpr long ⇒ K<5:long>
    constexpr long wide = 5;
    K<wide> e;
    K<5>    f;
    if (e.big() != 2) return 5;
    if (f.big() != 2) return 6;

    return 0;
}
