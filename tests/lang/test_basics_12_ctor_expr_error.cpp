// =============================================================================
// 测试（错误用例）：构造表达式的临时对象类型必须与声明类型一致
// =============================================================================
// 理论点：
//   · [dcl.init]/17.6.2：拷贝初始化的实参是同类临时对象时才允许"按原样使用"；
//     `Box<int> b = Box<double>(7);` 声明 Box_int、右值 Box_double ——
//     本语言没有用户自定义转换，连合成的拷贝构造也只收同类对象，
//     故这是 ill-formed（type mismatch）。
//   · 修复前这条根本没有可达路径（构造表达式整体报 Undefined variable），
//     修复后它走 [ctor] 升级分支的类型一致性检查。
// 预期行为：rc=1，报错文案
//   Type mismatch in 'b': declared 'Box_int', got 'Box_double'
// 对照 clang：clang++-18 -std=c++20 同样 rc=1
//   （`no viable conversion from 'Box<double>' to 'Box<int>'`，
//     报错文案不同但同属"不可转换"裁决）。
// =============================================================================

template<class T>
struct Box {
    T v;
    Box(T x) { v = x; }
};

int main() {
    Box<int> b = Box<double>(7);   // ✗ Box_double 不是 Box_int，无转换路径
    return b.v;
}
