#!/usr/bin/env bash
# 突变验证：摘掉本批的判据 ⇒ 对应单测/集成用例必须变红（红线 ⑦）
#
# 本批的判据（ROADMAP §0.1 批 0-1：按值返回 + 拷贝省略；BUGS.md B48/B49）：
#   m1 定义点【不 spill 隐藏返回缓冲指针】⇒ 调用方无处放对象（sret 形态塌了）
#   m2 返回路径【不把对象拷进缓冲】      ⇒ 返回的是 callee 帧内的地址（悬垂）
#   m3 调用点【不把缓冲装进 rdi】        ⇒ 被调方等着缓冲，拿到的是垃圾
#   m4 拷贝省略【退回临时区】            ⇒ 返回值没写进变量槽 ⇒ 变量恒为初值
#   m5 sret 判据【连引用一起剥】          ⇒ `S& f()` 被当成按值返回 ⇒ 引用语义被改写
#   m6 成员赋值【不保护对象地址】        ⇒ 右值求值踩 %rcx ⇒ 写到错的地址
#
# 用法：./tools/verify-sret-mutations.sh
#
# ★ 还原用 cp 备份（项目铁律：严禁 git checkout 未提交文件，B34 批次教训）。
set -u
cd "$(dirname "$0")/.."

CODEGEN=src/codegen/codegen.cpp
BAK_CODEGEN=/tmp/sret_mut_codegen.cpp
cp "$CODEGEN" "$BAK_CODEGEN"

restore() {
  cp "$BAK_CODEGEN" "$CODEGEN"
}
trap restore EXIT

# 单测：期望"有红"（即至少一条失败）
run_unit() {
  local desc="$1"; local filter="$2"
  cmake --build build-linux -j6 --target unit_tests > /tmp/sret_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获"
    grep -E "error" /tmp/sret_mut_build.log | head -3
    return 0
  fi
  local out
  out=$(./build-linux/unit_tests --gtest_filter="$filter" 2>&1)
  if echo "$out" | grep -qE "FAILED|\[  FAILED  \]"; then
    echo "   ✅ $desc ⇒ 对应单测变红（已捕获）"
    echo "$out" | grep -E "^\[  FAILED  \]" | head -3 | sed 's/^/        /'
  else
    echo "   ★ 突变未被捕获（应变红却绿了）: $desc"
    echo "$out" | tail -3 | sed 's/^/        /'
  fi
}

# 集成用例：编译该文件并跑，看退出码是否不再是期望值
run_integration() {
  local desc="$1"; local src="$2"; local expect_rc="$3"
  ./build-linux/minicc "$src" -o /tmp/sret_mut_it.bin > /tmp/sret_mut_it.log 2>&1
  local crc=$?
  if [ $crc -ne 0 ]; then
    echo "   ✅ $desc ⇒ 编译期就红了（rc=$crc，已捕获）"
    return 0
  fi
  /tmp/sret_mut_it.bin > /dev/null 2>&1
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

if which == "m1":       # 定义点不 spill 隐藏返回缓冲指针
    old = """        emit(std::format("movq %{}, {}(%rbp)         # 保存隐藏返回缓冲指针（sret 第 0 参数）",
            paramRegs[0], m_sretPtrSlot));"""
    new = """        // MUTATION m1: 不 spill 隐藏返回缓冲指针"""
elif which == "m2":     # 返回路径不把对象拷进缓冲
    old = """            emit("rep movsb                  # 逐字节复制对象表示到返回缓冲");"""
    new = """            // MUTATION m2: 不把对象拷进返回缓冲"""
elif which == "m3":     # 调用点不把缓冲装进 rdi（自由函数路径）
    old = """    if (sretCall) {
        emit(std::format("leaq {}(%rbp), %rdi       # rdi = 返回缓冲（sret 隐藏参数）", sretBuf));
    }
    emit(std::format("callq {}                  # 调用函数 {}", funcName, funcName));"""
    new = """    // MUTATION m3: 调用点不把返回缓冲装进 rdi
    emit(std::format("callq {}                  # 调用函数 {}", funcName, funcName));"""
elif which == "m4":     # 拷贝省略退回临时区（缓冲不落在变量槽）
    old = """            sretBuf = (m_sretTargetSlot != kNoSretTarget)
                ? m_sretTargetSlot : allocTempSlot(sretSize);"""
    new = """            sretBuf = allocTempSlot(sretSize);  // MUTATION m4: 拷贝省略退回临时区"""
elif which == "m5":     # sret 判据连引用一起剥（`S& f()` 被当成按值返回）
    old = """static TypePtr bareClassType(const TypePtr& t) {
    if (!t) return nullptr;
    if (t->isConst()) {
        TypePtr inner = t->stripConst();
        return inner ? inner : t;      // 拿不到内层就退回原类型（判据不崩、判"否"）
    }
    return t;
}"""
    new = """static TypePtr bareClassType(const TypePtr& t) {
    if (!t) return nullptr;
    TypePtr r = t;                     // MUTATION m5: 连引用一起剥
    while (r && (r->isConst() || r->isReference())) {
        r = r->isConst() ? r->stripConst() : r->referencedType;
    }
    return r;
}"""
elif which == "m6":     # 成员赋值不保护对象地址
    old = """            emit("pushq %rcx                   # 保护对象地址（右值求值会踩 rcx）");
            emitExpr(stmt.value);
            emit("popq %rcx                    # 对象地址回 rcx");"""
    new = """            emitExpr(stmt.value);  // MUTATION m6: 不保护对象地址"""
else:
    raise SystemExit("unknown mutation " + which)

if s.count(old) != 1:
    raise SystemExit("mutation %s: anchor count = %d (expected 1)" % (which, s.count(old)))
open(p, "w").write(s.replace(old, new))
PY
}

echo "════ 突变验证：按值返回 + 拷贝省略（6 条判据）════"
for m in m1 m2 m3 m4 m5 m6; do
  restore
  mutate "$m"
  cmake --build build-linux -j6 --target minicc > /tmp/sret_mut_build.log 2>&1
  case "$m" in
    m1) run_unit "m1 定义点不 spill 隐藏返回缓冲指针" 'SretReturn.*' ;;
    m2) run_unit "m2 返回路径不把对象拷进缓冲" 'SretReturn.*' ;;
    m3) run_unit "m3 调用点不把缓冲装进 rdi" 'SretReturn.*' ;;
    m4) run_unit "m4 拷贝省略退回临时区" 'SretReturn.*' ;;
    m5) run_unit "m5 sret 判据连引用一起剥" 'SretReturn.*' ;;
    m6) run_unit "m6 成员赋值不保护对象地址" 'MemberAssignClobber.*' ;;
  esac
  # 集成侧：按值返回的九个检查点任一失守都会让退出码 ≠ 0
  run_integration "$m（集成侧）" tests/lang/test_basics_37_return_class_by_value.cpp 0
done
restore
cmake --build build-linux -j6 --target minicc > /dev/null 2>&1
echo "════ 已还原源文件 ════"
