// =============================================================================
// 测试：具体类型 `int&` 形参配引用实参（[expr.type]/1 的 A 归一化）
// =============================================================================
// 考察理论点：
//   · [expr.type]/1：表达式的类型【永不可能是引用】。`int& r` 的声明类型是 int&，
//     但表达式 `r` 的类型是 int —— 值类别（左值）另由 argIsLValue 携带。
//     ⇒ 到达 deducePair 的 A 必须是裸类型，引用壳要在入口剥掉。
//   · 不剥的话【具体 P】配引用实参会死磕结构不等：
//       template<class T> int f(int& x, T y);
//       int& r = a;  f(r, 2);
//     P=int& 的绑定检查过（[dcl.init.ref]），但递归到非依赖 P=int vs A=int&
//     ⇒ "no match for non-dependent parameter: P='int' vs A='int&'"（修复前实测 rc=1）。
//   · P=T 形参位此前靠「值传递调整 A'=int」侥幸存活（T 被绑成 int& 而非 clang 的
//     int，靠折叠归一兜住）—— 统一从入口剥后两条路同一判据。
//   · ★ 偏特化匹配（structuralMatch）的 A 是【模板实参】（Box<int&> 的 int& 有意义），
//     不剥 —— 由 structuralMatch 守卫，与 T&& 分支跳过万能引用同一守卫。
//     （test_tmpl_52 的 RefKind 分类器走的就是这条路，回归时看它。）
// 预期行为：
//   · ① f(r, 2)（r 是 int&）⇒ T := int，运行返回 0
//   · ② f(T&, T) 两个形参共享 T ⇒ 两处推导一致（clang: T := int）
//   · ③ 右值引用变量 rr（int&&）是左值表达式 ⇒ deduce_kind(rr) 与 deduce_kind(a)
//      同一实例（T := int&）—— test_tmpl_52 已覆盖，这里只挂一条对照
// 对照 clang：本文件 clang++-18 -std=c++20 编译运行同样 rc=0。
// 回归背景：修复前 ① 报 no matching function（rc=1）—— 拒收合法程序，
// 与 B26 同族（A 带着声明类型的引用壳，P 侧按表达式类型比）。
// =============================================================================

// ① 具体类型 P=int&：修复前 "P='int' vs A='int&'" 死在这里
template <class T>
int f(int& x, T y) { return x + y; }

// ② P=T& 与 P=T 共享同一个 T：剥壳后两处都按 int 推，一致性检查过
template <class T>
int g(T& x, T y) { return x + y; }

// ③ deduce_kind：体内查 T 本身是不是引用（与 test_tmpl_52 同一手法 ——
//    实例化分类器对象再 .v()，不走限定名静态调用）
template <class T> struct IsLRef     { int v() { return 0; } };
template <class T> struct IsLRef<T&> { int v() { return 1; } };
template <class T> int deduce_kind(T&& x) {
    IsLRef<T> p;
    return p.v();
}

int main() {
    int  a  = 1;
    int& r  = a;
    int&& rr = 2;      // 右值引用变量：命名后仍是左值表达式

    // ① 具体 P=int& 配引用实参
    if (f(r, 2) != 3) return 1;

    // ② T& 与 T 共享 T（修复前 T 被绑成 int&，侥幸靠折叠兜住；归一后与 clang 一致）
    if (g(r, 2) != 3) return 2;

    // ③ 右值引用变量的值类别是左值 ⇒ T := int&（与 test_tmpl_52 的规则③同族）
    if (deduce_kind(rr) != 1) return 3;
    if (deduce_kind(a)  != 1) return 4;
    if (deduce_kind(7)  != 0) return 5;

    return 0;
}
