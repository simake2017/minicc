#!/usr/bin/env bash
# =============================================================================
# verify-comment-mutations.sh —— check-comment-style.sh 的突变验证
# =============================================================================
# 【为什么要有这个脚本】
#   守卫脚本自己也会骗人：一个恒真的判据能让脚本永远绿。
#   本脚本对每条判据做【两次】实测，把"能抓到"和"判据是承重的"分开证：
#     ① 引入违规  ⇒ 脚本必须【红】（证明判据不是摆设，能真抓到）
#     ② 放松判据  ⇒ 脚本必须【绿】（证明红是这条判据造成的，不是别的原因）
#   只做①不做②，会把"脚本恰好因为别的原因红了"误当成"判据有效"。
#
# ★【隔离】每次只让【一条】判据有可能变红：其余判据在脚本副本里一律先放松。
#   否则"放松判据①后脚本仍红"不能证明①是原因（可能是②还在报）。
#
# 【探针机制】
#   违规写在临时文件 src/ast/zz_comment_mutant_probe.cpp 里，跑完即删（trap 保证）。
#   ★ 违规必须落在【第 60 行之后】—— check-comment-style.sh 豁免文件前 60 行的
#     结构性文件头（那是"这个文件是什么"的说明，不是实现注释）。
#   ★ 放松判据靠【改一份脚本副本】（.mutant-check-comment-style.sh），
#     原脚本不动 —— 否则"验证脚本"本身就把被验证对象改了。
#
# 【用法】
#   ./verify-comment-mutations.sh          # 跑全部判据的突变验证
# =============================================================================

set -u
cd "$(dirname "$0")"

PROBE="src/ast/zz_comment_mutant_probe.cpp"
MUTANT=".mutant-check-comment-style.sh"

cleanup() { rm -f "$PROBE" "$MUTANT"; }
trap cleanup EXIT

PASS=0
FAIL=0

# ── 生成探针文件：前 60 行是填充（把违规推到文件头豁免之外），之后是三类违规 ──
make_probe() {
    {
        echo "// 填充：把违规推到第 60 行之后（结构性文件头豁免只覆盖前 60 行）"
        for i in $(seq 1 62); do
            echo "int zz_pad_$i = $i;"
        done
        echo ""
        echo "// ── 判据② 违规：私人笔记标记命中黑名单 ──"
        echo "// FIXME: 临时探针行 —— 证明判据②承重"
        echo ""
        echo "// ── 判据④ 违规：裸锚点（clang 符号名没给文件路径、块内也无豁免登记）──"
        echo "// 对照 clang：Sema::ActOnIntegerConstant"
        echo ""
        echo "// ── 判据① 违规：超长注释块（32 行 > 上限 25）──"
        for i in $(seq 1 31); do
            echo "// 探针行 $i"
        done
        echo "int zz_probe_unused = 0;"
    } > "$PROBE"
}

# ── 放松判据：改脚本副本里的判据参数/条件 ──
#   $1 = 要【放松】的判据（空格分隔：block / private / bare）；未列出的保持严格。
#   ★ 必须逐条独立可开关 —— 否则"放松①后仍红"没法证明是①造成的。
relax() {
    local want="$1"
    local args=()
    if echo "$want" | grep -qw block; then
        args+=(-e 's/^MAX_BLOCK=25.*$/MAX_BLOCK=999            # 突变：放开块长上限/')
    fi
    if echo "$want" | grep -qw private; then
        args+=(-e 's/^PRIVATE_MARKS=.*$/PRIVATE_MARKS="zzzz-never-matches"   # 突变：清空黑名单/')
    fi
    if echo "$want" | grep -qw bare; then
        args+=(-e 's/if BARE_ANCHOR.search(l):/if False and BARE_ANCHOR.search(l):/')
    fi
    if [ ${#args[@]} -eq 0 ]; then
        cp check-comment-style.sh "$MUTANT"
    else
        sed "${args[@]}" check-comment-style.sh > "$MUTANT"
    fi
    chmod +x "$MUTANT"
}

report() {
    local name="$1" step="$2" want="$3" got="$4" detail="$5"
    local mark
    if [ "$want" = "$got" ]; then
        mark="✅"; PASS=$((PASS + 1))
    else
        mark="❌"; FAIL=$((FAIL + 1))
    fi
    printf "  %s %-16s %-12s 期望=%s 实得=%s  %s\n" \
           "$mark" "$name" "$step" "$want" "$got" "$detail"
}

run_guard() {
    "./$1" > /tmp/comment-mutant-guard.log 2>&1
    if [ $? -eq 0 ]; then echo 0; else echo 1; fi
}

echo "======================================================================"
echo "  注释守卫脚本的突变验证（check-comment-style.sh）"
echo "======================================================================"

make_probe

# ── 判据① 块长上限 ──────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据① 块长上限（其余判据先放松，保证只有①能变红）"
relax "private bare"
got=$(run_guard "$MUTANT")
report "① 超长注释块" "①严格时" "1" "$got" "（红：只剩①会因探针里的 32 行块而报）"
relax "private bare block"
got=$(run_guard "$MUTANT")
report "① 超长注释块" "①放松后" "0" "$got" "（绿：上限放到 999，①不再报）"

# ── 判据② 私人笔记 ──────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据② 私人笔记标记（其余判据先放松）"
relax "block bare"
got=$(run_guard "$MUTANT")
report "② 私人笔记" "②严格时" "1" "$got" "（红：只剩②会因探针里的 FIXME 行而报）"
relax "block bare private"
got=$(run_guard "$MUTANT")
report "② 私人笔记" "②放松后" "0" "$got" "（绿：黑名单清空，②不再报）"

# ── 判据④ 裸锚点 ────────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据④ 裸锚点（其余判据先放松）"
relax "block private"
got=$(run_guard "$MUTANT")
report "④ 裸锚点" "④严格时" "1" "$got" "（红：只剩④会因探针里的裸锚点而报）"
relax "block private bare"
got=$(run_guard "$MUTANT")
report "④ 裸锚点" "④放松后" "0" "$got" "（绿：正则改成永不匹配，④不再报）"

# ── 收官：原脚本在探针存在时必须红、探针撤掉后必须绿 ────────────────────────
echo ""
echo "  ▸ 收官：原脚本（未放松任何判据）"
got=$(run_guard check-comment-style.sh)
report "原脚本" "探针在场" "1" "$got" "（红：三类违规叠加仍必须被抓到）"
rm -f "$PROBE"
got=$(run_guard check-comment-style.sh)
report "原脚本" "探针撤掉" "0" "$got" "（绿：撤掉违规即恢复通过）"

echo ""
echo "======================================================================"
echo "  突变验证结果：通过 $PASS 项，失败 $FAIL 项"
echo "======================================================================"

if [ "$FAIL" -ne 0 ]; then
    echo "  ❌ 存在未通过项 —— 说明有判据是【摆设】（恒真 / 抓不到 / 抓错了原因）"
    exit 1
fi

echo "  ✅ 全部判据均为承重判据（能抓到违规，且放松后不再报）"
exit 0
