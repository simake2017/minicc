// =============================================================================
// 测试（错误用例）：类模板的嵌套类 —— 名字不存在时必须响亮拒收
// =============================================================================
// 理论点：
//   · [basic.lookup.qual]：限定名 `Box<int>::Missing` 的限定者 `Box<int>` 解析成
//     【类型】后，成员名 Missing 必须在该类型的成员表（嵌套类 + 类内类型别名）里找
//     —— 找不到就是 ill-formed，诊断指向【根因】（哪个类的哪个成员）。
//   · ★ 本用例钉住的是"克隆了嵌套类"这件事**没有把边界一起放大**：
//     克隆的是"蓝图里声明过的那些嵌套类"，不是"任意名字" —— 否则
//     `Box<int>::Missing` 会被静默当成一个空类接受下来（连症状都没有）。
//   · 对照：非模板的同款现场见 tests/lang/test_basics_33_nested_class_error.cpp
//     （`A::C` ⇒ "no type named 'C' in 'A'"，与 clang 逐字相同）。
// 预期行为：rc=1，报错文案
//   no type named 'Missing' in 'Box_int'
// ★ 与 clang 的差异（**同族、全项目统一**，不是本批引入的）：
//   clang 报 "no type named 'Missing' in 'Box<int>'" —— 差在【拼写】：
//   minicc 把模板实例改名为 "Box_int"（全项目统一，见 docs/learn/13 与 learn/49
//   的"作用域即身份"），诊断里印的是**改名后的实例名**；clang 保留源码拼写。
//   同族差异还有 `No member 'x' in class 'Box_int'`（clang: no member named 'x'
//   in 'Box<int>'）—— 已登记于 docs/ROADMAP.md §0.3 降级区。
// 对照 clang：clang++-18 -std=c++20 同样 rc=1（文案差异见上）。
// =============================================================================

template <class T> struct Box {
    struct Inner { T v; };          // 只有嵌套类 Inner，没有 Missing
};

int main() {
    Box<int>::Missing m;             // ✗ Box<int> 里没有名为 Missing 的成员类型
    return 0;
}
