#!/usr/bin/env bash
# 突变验证：摘掉 5 条判据 ⇒ 对应单测必须变红（红线 ⑦）
# 用法：./tools/verify-nested-mutations.sh
set -u
cd "$(dirname "$0")/.."

SRC=src/sema/semantic_analyzer.cpp
BAK=/tmp/nested_mutation_backup.cpp
cp "$SRC" "$BAK"

restore() { cp "$BAK" "$SRC"; }
trap restore EXIT

run_case() {
  local label="$1"; shift
  local expect_red="$1"; shift
  cmake --build build-linux -j6 >/dev/null 2>&1
  local out
  out=$(ctest --test-dir build-linux -R "$1" 2>&1 | tail -3)
  local failed
  failed=$(echo "$out" | grep -c "Failed\|failed out of")
  echo "── $label"
  echo "$out" | grep -E "tests passed|tests failed" | head -2
  if [ "$expect_red" = "red" ]; then
    if echo "$out" | grep -q "0 tests failed"; then
      echo "   ★ 突变未被捕获（应变红却绿了）"
    else
      echo "   ✓ 突变被捕获（变红）"
    fi
  else
    if echo "$out" | grep -q "0 tests failed"; then
      echo "   ✓ 放松后仍绿"
    else
      echo "   ★ 放松判据后变红（守卫自身有 bug）"
    fi
  fi
}

mutate() {
  python3 - "$1" <<'PY'
import sys, re
which = sys.argv[1]
p = "src/sema/semantic_analyzer.cpp"
s = open(p).read()
if which == "m1":       # 嵌套类不前缀化
    s = s.replace('nested->name = decl->name + "::" + nested->name;',
                  'nested->name = nested->name;  // MUTATION')
elif which == "m2":     # 类作用域链不查嵌套类
    s = s.replace("""            for (auto& nested : cit->second->nestedClasses) {
                if (nested->name == qualified && nested->classType) {""",
                  """            for (auto& nested : cit->second->nestedClasses) {
                if (false && nested->name == qualified && nested->classType) {""")
elif which == "m3":     # 类自身的名字不可见
    s = s.replace("if (selfVisible == name && cit->second->classType) {",
                  "if (false && selfVisible == name && cit->second->classType) {")
elif which == "m4":     # 限定名调用不改写成登记名
    s = s.replace("""            if (registered != funcName) {""",
                  """            if (false && registered != funcName) {""")
elif which == "m5":     # 缺成员时不报错（去掉限定名兜底）
    s = s.replace("""                error(std::format("no type named '{}' in '{}'", member, qualifier),
                      SourceLocation{});""",
                  """                // MUTATION: 不报错""")
else:
    raise SystemExit("unknown mutation " + which)
open(p, "w").write(s)
PY
}

echo "════ 突变验证：嵌套类 5 条判据 ════"
for m in m1 m2 m3 m4 m5; do
  restore
  mutate "$m"
  case "$m" in
    m1) run_case "① 嵌套类不前缀化（登记成裸名）" red "NestedClass\.(NestedAndGlobalStayDistinct|RegisteredUnderQualifiedName)";;
    m2) run_case "② 类作用域链不查嵌套类" red "NestedClass\.(UnqualifiedNameResolvesToNested)";;
    m3) run_case "③ 类自身的名字不可见" red "NestedClass\.(ClassNameVisibleInOwnScope)";;
    m4) run_case "④ 限定名调用不改写成登记名" red "NestedClass\.(QualifiedCallUsesRegisteredName)";;
    m5) run_case "⑤ 缺成员时不报错" red "NestedClass\.(MissingNestedMemberIsAnError)";;
  esac
done
restore
echo "════ 已还原源文件 ════"
