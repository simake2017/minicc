#!/usr/bin/env bash
# 突变验证：摘掉本批的判据 ⇒ 对应单测/集成用例必须变红（红线 ⑦）
#
# 本批的判据（ROADMAP §0.1；BUGS.md B6 —— 引用的别名语义）：
#   m1 局部引用变量【绑定时取地址】改成【拷值】⇒ 槽里放的是副本，不是别名
#   m2 引用形参【不登记为别名】           ⇒ 被调点不按别名解释那个槽
#   m3 调用点【不按地址传】引用形参       ⇒ 两侧口径漂移（传值 vs 按别名读）
#   m4 引用赋值【不写被绑定对象】         ⇒ 写的是引用自己的槽 ⇒ 静默算错
#   m5 作用域链【不弹层】                 ⇒ 旧块的别名盖住新块的同名声明
#   m6 const 引用形参【改成按地址传】     ⇒ 实参可能是右值（没有地址）
#
# 用法：./tools/verify-ref-alias-mutations.sh
#
# ★ 还原用 cp 备份（项目铁律：严禁 git checkout 未提交文件，B34 批次教训）。
set -u
cd "$(dirname "$0")/.."

CODEGEN=src/codegen/codegen.cpp
BAK_CODEGEN=/tmp/ref_alias_mut_codegen.cpp
cp "$CODEGEN" "$BAK_CODEGEN"

restore() {
  cp "$BAK_CODEGEN" "$CODEGEN"
}
trap restore EXIT

# 单测：期望"有红"（即至少一条失败）
run_unit() {
  local desc="$1"; local filter="$2"
  cmake --build build-linux -j6 --target unit_tests > /tmp/ref_alias_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获"
    grep -E "error" /tmp/ref_alias_mut_build.log | head -3 | sed 's/^/        /'
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

# 集成用例：编译该文件并跑，看退出码是否不再是期望值
run_integration() {
  local desc="$1"; local src="$2"; local expect_rc="$3"
  ./build-linux/minicc "$src" -o /tmp/ref_alias_mut_it.bin > /tmp/ref_alias_mut_it.log 2>&1
  local crc=$?
  if [ $crc -ne 0 ]; then
    echo "   ✅ $desc ⇒ 编译期就红了（rc=$crc，已捕获）"
    return 0
  fi
  /tmp/ref_alias_mut_it.bin > /dev/null 2>&1
  local rrc=$?
  if [ "$rrc" != "$expect_rc" ]; then
    echo "   ✅ $desc ⇒ 运行退出码 $rrc ≠ 期望 $expect_rc（已捕获）"
  else
    echo "   ★ 突变未被捕获（运行退出码仍是 $expect_rc）: $desc"
  fi
}

mutate() {
  python3 - "$1" <<'PY'
import sys
which = sys.argv[1]
p = "src/codegen/codegen.cpp"
s = open(p).read()

if which == "m1":       # 局部引用变量：绑定时取地址 ⇒ 改成拷值
    old = """        emitAddressOf(decl.initializer);              // 绑到初始化式的地址"""
    new = """        emitExpr(decl.initializer);  // MUTATION m1: 绑定时拷值而不是取地址"""
elif which == "m2":     # 引用形参：不登记为别名
    old = """            if (isAliasBinding(func->parameters[i].type, false)) {
                m_refScopes.back()[func->parameters[i].name] = RefLocalInfo{
                    paramOffset, func->parameters[i].type->referencedType};
            }"""
    new = """            // MUTATION m2: 引用形参不登记为别名"""
elif which == "m3":     # 调用点：引用形参不按地址传
    old = """    return isAliasBinding(fn->parameters[argIndex].type, false);"""
    new = """    return false;  // MUTATION m3: 引用形参一律按值传"""
elif which == "m4":     # 引用赋值：不写被绑定对象
    old = """                emit(std::format("movq {}(%rbp), %rcx    # 取引用 {} 持有的地址",
                    rit->offset, var->name));
                emit("movq %rax, (%rcx)           # 写被绑定对象");"""
    new = """                emit(std::format("movq %rax, {}(%rbp)    # MUTATION m4: 写引用自己的槽",
                    rit->offset, var->name));"""
elif which == "m5":     # 作用域链不弹层（退回平表语义）
    old = """    m_refScopes.pop_back();          // 引用表出块：内层遮蔽随之解除"""
    new = """    // MUTATION m5: 引用表出块不弹层（旧块的别名盖住新块的同名声明）"""
elif which == "m6":     # const 引用形参改成按地址传
    old = """static bool isWritableLValueRefType(TypePtr t) {
    if (!t || !t->isLValueReference()) return false;"""
    new = """static bool isWritableLValueRefType(TypePtr t) {
    if (t && t->isLValueReference()) return true;  // MUTATION m6: const 引用也走别名通道
    if (!t || !t->isLValueReference()) return false;"""
else:
    raise SystemExit("unknown mutation " + which)

if s.count(old) != 1:
    raise SystemExit("mutation %s: anchor count = %d (expected 1)" % (which, s.count(old)))
open(p, "w").write(s.replace(old, new))
PY
}

echo "════ 突变验证：引用的别名语义（6 条判据）════"
for m in m1 m2 m3 m4 m5 m6; do
  restore
  mutate "$m"
  cmake --build build-linux -j6 --target minicc > /tmp/ref_alias_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获: $m"
    grep -E "error" /tmp/ref_alias_mut_build.log | head -3 | sed 's/^/        /'
    continue
  fi
  case "$m" in
    m1) run_unit "m1 局部引用变量绑定时拷值" 'RefAlias.*' ;;
    m2) run_unit "m2 引用形参不登记为别名" 'RefAlias.*' ;;
    m3) run_unit "m3 调用点不按地址传引用形参" 'RefAlias.*' ;;
    m4) run_unit "m4 引用赋值不写被绑定对象" 'RefAlias.*' ;;
    m5) run_unit "m5 作用域链不弹层" 'RefAlias.*' ;;
    m6) run_unit "m6 const 引用形参改成按地址传" 'RefAlias.*' ;;
  esac
  # 集成侧：十四个检查点任一失守都会让退出码 ≠ 0
  run_integration "$m（集成侧）" tests/lang/test_basics_38_ref_alias.cpp 0
done
restore
cmake --build build-linux -j6 --target minicc > /dev/null 2>&1
echo "════ 已还原源文件 ════"
