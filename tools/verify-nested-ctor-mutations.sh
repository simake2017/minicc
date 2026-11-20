#!/usr/bin/env bash
# 突变验证：摘掉本批的判据 ⇒ 对应单测/集成用例必须变红（红线 ⑦）
#
# 本批的判据（ROADMAP §0.1；BUGS.md B60、learn/62 —— 嵌套类的构造/析构身份）：
#   m1 合成 ctor 名【用本类裸名】改成【用登记名】 ⇒ 同一类两种符号形态
#   m2 合成 dtor 名同上（"~" + 登记名）
#   m3 初始化列表【不回填】构造符号         ⇒ CodeGen 退回硬拼（限定名拼不出来）
#   m4 成员函数以【限定名】登记进符号表     ⇒ 占用静态成员的 "类名::成员名" 键空间
#   m5 成员函数以【裸名】登记进全局作用域   ⇒ 成员名泄漏成全局可见的名字
#
# 用法：./tools/verify-nested-ctor-mutations.sh
#
# ★ 还原用 cp 备份（项目铁律：严禁 git checkout 未提交文件，B34 批次教训）。
set -u
cd "$(dirname "$0")/.."

SEMA=src/sema/semantic_analyzer.cpp
BAK=/tmp/nested_ctor_mut_sema.cpp
cp "$SEMA" "$BAK"

restore() { cp "$BAK" "$SEMA"; }
trap restore EXIT

run_unit() {
  local desc="$1"; local filter="$2"
  cmake --build build-linux -j6 --target unit_tests > /tmp/nested_ctor_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获: $desc"
    grep -E "error" /tmp/nested_ctor_mut_build.log | head -3 | sed 's/^/        /'
    return 0
  fi
  local out
  out=$(./build-linux/unit_tests --gtest_filter="$filter" 2>&1)
  if echo "$out" | grep -qE "\[  FAILED  \]"; then
    echo "   ✅ $desc ⇒ 对应单测变红（已捕获）"
    echo "$out" | grep -E "^\[  FAILED  \]" | head -4 | sed 's/^/        /'
  else
    echo "   ★ 突变未被捕获（应变红却绿了）: $desc"
    echo "$out" | tail -3 | sed 's/^/        /'
  fi
}

run_integration() {
  local desc="$1"; local src="$2"; local expect_rc="$3"
  ./build-linux/minicc "$src" -o /tmp/nested_ctor_mut_it.bin > /tmp/nested_ctor_mut_it.log 2>&1
  local crc=$?
  # 期望编译失败的用例（expect_rc != 0）：突变会把它变成"静默通过"
  if [ "$expect_rc" != "0" ]; then
    if [ $crc -eq "$expect_rc" ]; then
      echo "   ★ 突变未被捕获（本题应报错却仍 rc=$crc）: $desc"
    else
      echo "   ✅ $desc ⇒ 编译期不再拒收（rc=$crc，已捕获）"
    fi
    return 0
  fi
  if [ $crc -ne 0 ]; then
    echo "   ✅ $desc ⇒ 编译期就红了（rc=$crc，已捕获）"
    return 0
  fi
  /tmp/nested_ctor_mut_it.bin > /dev/null 2>&1
  local rrc=$?
  if [ "$rrc" != "$expect_rc" ]; then
    echo "   ✅ $desc ⇒ 运行退出码 $rrc ≠ 期望 $expect_rc（已捕获）"
  else
    echo "   ★ 突变未被捕获（运行退出码仍是 $expect_rc）: $desc"
  fi
}

# 日志级断言：某些突变不改变运行结果，只改变【可观测的语义事实】
# （符号形态 / 查找走了哪条规则）—— 这类只有日志能抓。
# 断言方向：正常状态【不出现】的缺陷形态，突变后【必须出现】（出现了才算抓到）。
run_log_assert() {
  local desc="$1"; local src="$2"; local pattern="$3"
  ./build-linux/minicc "$src" -o /tmp/nested_ctor_mut_it.bin > /tmp/nested_ctor_mut_it.log 2>&1
  if grep -qF "$pattern" /tmp/nested_ctor_mut_it.log; then
    echo "   ✅ $desc ⇒ 日志里出现了缺陷形态 '$pattern'（已捕获）"
  else
    echo "   ★ 突变未被捕获（日志里没有 '$pattern'）: $desc"
  fi
}

mutate() {
  python3 - "$1" <<'PY'
import sys
which = sys.argv[1]
p = "src/sema/semantic_analyzer.cpp"
s = open(p).read()

ctor_old = "        defaultCtor->name = ownName;"
ctor_new = "        defaultCtor->name = decl->name;   // MUTATION m1: 拿登记名当函数名"
dtor_old = '        defaultDtor->name = "~" + ownName;'
dtor_new = '        defaultDtor->name = "~" + decl->name;   // MUTATION m2'

init_old = """    if (decl->kind == NodeKind::Constructor) {
        resolveCtorInitList(std::static_pointer_cast<ConstructorDecl>(decl));
    }"""
init_new = """    // MUTATION m3: 初始化列表不回填构造符号"""

reg_old = """    if (decl->ownerClassName.empty()) {
        Symbol sym;
        sym.name = decl->name;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        sym.ownerClass = decl->ownerClassName;
        sym.definedAt = decl->location;
        m_symbolTable.define(decl->name, sym);
    }"""
reg_qualified = """    {
        Symbol sym;
        sym.name = fullName;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        sym.ownerClass = decl->ownerClassName;
        sym.definedAt = decl->location;
        m_symbolTable.define(fullName, sym);   // MUTATION m4
    }"""
reg_bare = """    {
        Symbol sym;
        sym.name = decl->name;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        sym.ownerClass = decl->ownerClassName;
        sym.definedAt = decl->location;
        m_symbolTable.define(decl->name, sym);   // MUTATION m5
    }"""

if which == "m1":
    old, new = ctor_old, ctor_new
elif which == "m2":
    old, new = dtor_old, dtor_new
elif which == "m3":
    old, new = init_old, init_new
elif which == "m4":
    old, new = reg_old, reg_qualified
elif which == "m5":
    old, new = reg_old, reg_bare
else:
    raise SystemExit("unknown mutation " + which)

if s.count(old) != 1:
    raise SystemExit("mutation %s: anchor count = %d (expected 1)" % (which, s.count(old)))
open(p, "w").write(s.replace(old, new))
PY
}

echo "════ 突变验证：嵌套类构造/析构身份（5 条判据）════"
for m in m1 m2 m3 m4 m5; do
  restore
  mutate "$m"
  cmake --build build-linux -j6 --target minicc > /tmp/nested_ctor_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获: $m"
    grep -E "error" /tmp/nested_ctor_mut_build.log | head -3 | sed 's/^/        /'
    continue
  fi
  case "$m" in
    m1)
      run_unit "m1 合成 ctor 用登记名当函数名" 'NestedCtorIdentity.*'
      run_log_assert "m1（集成侧）" tests/lang/test_basics_39_nested_class_ctor.cpp \
        "Outer::Plain_Outer::Plain"
      ;;
    m2)
      run_unit "m2 合成 dtor 用登记名当函数名" 'NestedCtorIdentity.*'
      run_log_assert "m2（集成侧）" tests/lang/test_basics_39_nested_class_ctor.cpp \
        "method: ~Outer::Plain("
      ;;
    m3)
      run_unit "m3 初始化列表不回填构造符号" 'NestedCtorIdentity.*'
      # 七个检查点里 ④ 直接依赖回填（基类构造目标丢了 ⇒ rc=4）
      run_integration "m3（集成侧）" tests/lang/test_basics_39_nested_class_ctor.cpp 0
      ;;
    m4)
      run_unit "m4 成员函数以限定名登记" 'NestedCtorIdentity.*'
      run_log_assert "m4（集成侧）" tests/lang/test_basics_03_virtual_with_params.cpp \
        "(static member"
      ;;
    m5)
      run_unit "m5 成员函数以裸名登记进全局" 'NestedCtorIdentity.*'
      # 非法程序用例：突变后"裸用成员名"会被静默接受 ⇒ 编译期不再拒收
      run_integration "m5（集成侧）" \
        tests/lang/test_basics_26_bare_type_name_namespace_not_visible_error.cpp 1
      ;;
  esac
done
restore
cmake --build build-linux -j6 --target minicc > /dev/null 2>&1
echo "════ 已还原源文件 ════"
