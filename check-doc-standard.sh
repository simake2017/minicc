#!/usr/bin/env bash
# =============================================================================
# check-doc-standard.sh —— 文档骨架规范的机器断言
# =============================================================================
# 【为什么要有这个脚本】
#   文档规范写在 docs/constraints/06-文档骨架规范.md 里，但"写下来的规范"没人查
#   就会慢慢腐化。本脚本把可机检的部分变成断言（与 check-comment-style.sh 同款思路）：
#   注释规范管源码注释，本脚本管文档骨架。
#
# 【判定范围】
#   只查【产品文档】——docs/ 下的自有文档。third_party/ 与 googletest 语料不查。
#   测试目录（tests/）下的 .md 不查（那是用例说明）。
#
# 【用法】
#   ./check-doc-standard.sh          跑检查（有违规即退出码 1）
#   ./check-doc-standard.sh --report 只出统计、不判失败
# =============================================================================

set -u
cd "$(dirname "$0")"

REPORT_ONLY=0
[ "${1:-}" = "--report" ] && REPORT_ONLY=1

python3 - "$REPORT_ONLY" <<'PYEOF'
import os, re, subprocess, sys

REPORT_ONLY = int(sys.argv[1])

# ★ 突变验证开关：CHECK_DOC_RELAX=header,skeleton,meta,placeholder,asset
#   列出要【放松】的判据（英文代号）。只供 verify-doc-mutations.sh 使用 ——
#   判据本身不因此变弱，只是被旁路；脚本会用"引入违规后是否变红"来证明它承重。
RELAX = set(x for x in os.environ.get('CHECK_DOC_RELAX', '').split(',') if x)

DOC_ROOT = 'docs'

# ── 文档族 → 文件清单（与 docs/constraints/06 §1~§6 的归属表一致）────────────
TECH_TOPICS = [
    'docs/cmake-build-system.md',
    'docs/toolchain-search-paths.md',
    'docs/escape-and-file-reading.md',
    'docs/make-tutorial.md',
    'docs/upstream/clang-stdlib-and-header-search.md',
    'cpp-compilation-lifecycle/tutorial.md',
    'demos/README.md',
]

def collect(pattern_roots, suffix=None, name_filter=None):
    out = []
    for root in pattern_roots:
        if not os.path.isdir(root):
            continue
        for dp, dn, fn in os.walk(root):
            dn[:] = [d for d in dn if d not in ('legacy',)]
            for f in sorted(fn):
                if not f.endswith('.md'):
                    continue
                p = os.path.join(dp, f)
                if name_filter and not name_filter(p):
                    continue
                out.append(p)
    return sorted(out)

# ── 突变验证用的探针模式 ───────────────────────────────────────────────────
#   CHECK_DOC_PROBE=<目录> 时，本脚本【只】检查探针目录下的文档：
#     <探针>/tech/*.md    ⇒ 走技术文档判据
#     <探针>/learn/*.md   ⇒ 走 learn 文档判据（文件名须以数字开头）
#     <探针>/post/*.md    ⇒ 走复盘文档判据（YAML frontmatter）
#     <探针>/bugs.md      ⇒ 走 BUGS 条目级元数据判据
#   ★ 为什么要有这个模式：突变验证要证"判据承重"，必须在【干净的底子】上
#     注入一条违规。若连真实文档一起判，底子本身的几百处违规会淹没结果 ——
#     "红"分不清是探针造成的还是底子造成的。
#   ★ 探针模式只放宽【判定范围】，不放宽任何【判据本身】：
#     判据的严格性由 CHECK_DOC_RELAX 控制，两者是正交的闸。
PROBE = os.environ.get('CHECK_DOC_PROBE', '')

if PROBE:
    TECH_DOCS  = collect([os.path.join(PROBE, 'tech')])
    DEV_SPECS  = []
    DEV_DESIGNS= []
    DEV_CONST  = []
    DEV_ARCH   = []
    LINE_DOCS  = []
    LEARN_DOCS = [p for p in collect([os.path.join(PROBE, 'learn')])
                  if re.match(r'^\d+[-.]', os.path.basename(p))]
    POSTMORTEM = collect([os.path.join(PROBE, 'post')])
    BUGS_PATH  = os.path.join(PROBE, 'bugs.md')
else:
    TECH_DOCS  = collect([os.path.join(DOC_ROOT, 'business')]) + TECH_TOPICS
    DEV_SPECS  = [p for p in collect([os.path.join(DOC_ROOT, 'specs')]) if p.endswith('.spec.md')]
    DEV_DESIGNS= [p for p in collect([os.path.join(DOC_ROOT, 'designs')]) if p.endswith('.design.md')]
    DEV_CONST  = collect([os.path.join(DOC_ROOT, 'constraints')])
    DEV_ARCH   = [os.path.join(DOC_ROOT, 'REFACTOR-ast-visitor.md'),
                  os.path.join(DOC_ROOT, 'prompt-template-deduction.md')]
    LINE_DOCS  = collect([os.path.join(DOC_ROOT, 'lines')])
    LEARN_DOCS = [p for p in collect([os.path.join(DOC_ROOT, 'learn')])
                  if re.match(r'^\d+[-.]', os.path.basename(p))]
    POSTMORTEM = [os.path.join(DOC_ROOT, 'BUGS.md'),
                  os.path.join(DOC_ROOT, 'PITFALLS.md'),
                  os.path.join(DOC_ROOT, 'NOTES-阅读笔记.md')]
    BUGS_PATH  = os.path.join(DOC_ROOT, 'BUGS.md')

HEADER_MARKS = ('读者：', '审法：', '名词版本：')
FM_KEYS = ('title:', 'date:', 'category:', 'tags:', 'type:', 'severity:', 'status:', 'related:')

problems = []   # (判据, 文件, 说明)
stats = {}

def read(p):
    try:
        return open(p, encoding='utf-8').read()
    except (OSError, UnicodeDecodeError):
        return ''

def check(cond, crit, path, msg):
    if not cond:
        problems.append((crit, path, msg))

def has_header(txt):
    return all(m in txt for m in HEADER_MARKS) and '## 变更记录' in txt

def has_out_of_scope(txt):
    return re.search(r'Out of Scope', txt, re.I) is not None

def has_observability(txt):
    return re.search(r'观测点', txt) is not None

def has_change_log(txt):
    return '## 变更记录' in txt

def base_lines(path):
    """git 基线行数：先查 HEAD，查不到再查暂存区（新文件由突变验证脚本暂存后可比）。
    两处都查不到则返回 None（该文件不参与判据⑤）。"""
    for rev in (f'HEAD:{path}', f':{path}'):
        try:
            out = subprocess.run(['git', 'show', rev],
                                 capture_output=True, text=True, timeout=20)
            if out.returncode == 0:
                return len(out.stdout.split('\n'))
        except Exception:
            pass
    return None

# ── 判据① 头部：技术 / 开发 / 线文档必须有三行头部 + 变更记录 ─────────────────
n = 0
for p in TECH_DOCS + DEV_SPECS + DEV_DESIGNS + DEV_CONST + DEV_ARCH + LINE_DOCS:
    txt = read(p)
    if not txt:
        continue
    n += 1
    if 'header' not in RELAX:
        check(has_header(txt), '①头部', p,
              '缺「读者 / 审法 / 名词版本」三行头部或「变更记录」表')
stats['① 头部'] = (n, len([x for x in problems if x[0] == '①头部']))

# ── 判据② 骨架节 ───────────────────────────────────────────────────────────
n = 0
for p in TECH_DOCS:
    txt = read(p)
    if not txt:
        continue
    n += 1
    if 'skeleton' not in RELAX:
        check(has_out_of_scope(txt), '②骨架节', p, '缺「Out of Scope」节')
        check(has_observability(txt), '②骨架节', p, '缺「观测点」节')
stats['② 骨架节'] = (n, len([x for x in problems if x[0] == '②骨架节']))

n = 0
for p in LEARN_DOCS:
    txt = read(p)
    if not txt:
        continue
    n += 1
    if 'skeleton' not in RELAX:
        check('能力清单' in txt, '②骨架节', p, 'learn 文档缺「§0 定位与能力清单」')
        check('真实版多做了什么' in txt, '②骨架节', p, 'learn 文档缺「真实版多做了什么、为什么」小节')
        check('演进记录' in txt, '②骨架节', p, 'learn 文档缺文末「演进记录」表')
stats['② 骨架节'] = (stats['② 骨架节'][0] + n,
                     len([x for x in problems if x[0] == '②骨架节']))

# ── 判据③ 元数据：复盘文档 frontmatter + BUGS 条目级三字段 ──────────────────
n = 0
for p in POSTMORTEM:
    txt = read(p)
    if not txt:
        continue
    n += 1
    head = txt[:1200]
    if 'meta' not in RELAX:
        check(head.lstrip().startswith('---'), '③元数据', p, '顶部缺 YAML frontmatter')
        check(all(k in head for k in FM_KEYS), '③元数据', p, 'frontmatter 缺必需键')
stats['③ 元数据'] = (n, len([x for x in problems if x[0] == '③元数据']))

n = 0
bugtxt = read(BUGS_PATH)
if bugtxt:
    heads = re.findall(r'^##\s+(B\d+)\s*(.*)$', bugtxt, re.M)
    for bid, _t in heads:
        n += 1
        # 该条到下一个 ## 之间的正文
        m = re.search(r'^##\s+' + re.escape(bid) + r'\b.*?$(.*?)(?=^##\s|\Z)',
                      bugtxt, re.M | re.S)
        body = m.group(1) if m else ''
        if 'meta' not in RELAX:
            for f in ('severity:', 'status:', 'related:'):
                check(f in body, '③元数据', f'docs/BUGS.md#{bid}',
                      f'条目缺 {f} 行内元数据')
stats['③ 元数据'] = (stats['③ 元数据'][0] + n,
                     len([x for x in problems if x[0] == '③元数据']))

# ── 判据④ 占位符 ───────────────────────────────────────────────────────────
#   只抓"真的没写完"的洞：
#     · xx_ 前缀标识符 —— 一律是洞
#     · <type> / ..._ —— 只在 ```jsonc 数据形状块里才算洞
#   （散文里的 <type> 是 Itanium ABI 替换模板的**正规术语**，
#     docs/learn/35 与 docs/BUGS.md 里的就是，不是洞。）
#   constraints/06 本身定义了这条规则，豁免。
n = 0
for p in TECH_DOCS + DEV_SPECS + DEV_DESIGNS + DEV_CONST + DEV_ARCH + LEARN_DOCS + POSTMORTEM:
    if p.endswith('06-文档骨架规范.md'):
        continue
    txt = read(p)
    if not txt:
        continue
    n += 1
    if 'placeholder' not in RELAX:
        bad = re.findall(r'\bxx_[A-Za-z_]*', txt)
        for blk in re.findall(r'```jsonc\n(.*?)```', txt, re.S):
            bad += re.findall(r'<type>|\.\.\._', blk)
        check(not bad, '④占位符', p, f'出现占位符 {bad[:3]}')
stats['④ 占位符'] = (n, len([x for x in problems if x[0] == '④占位符']))

# ── 判据⑤ 资产：行数不得比 git 基线少（防"重写时丢内容"）────────────────────
n = 0
for p in TECH_DOCS + DEV_SPECS + DEV_DESIGNS + DEV_CONST + DEV_ARCH + LEARN_DOCS + POSTMORTEM:
    b = base_lines(p)
    if b is None:
        continue
    cur = len(read(p).split('\n'))
    n += 1
    if 'asset' not in RELAX:
        check(cur >= b, '⑤资产', p, f'行数 {cur} < git 基线 {b}（内容被删）')
stats['⑤ 资产'] = (n, len([x for x in problems if x[0] == '⑤资产']))

# ── 报告 ───────────────────────────────────────────────────────────────────
print("=" * 70)
print("  文档骨架合规检查（docs/constraints/06-文档骨架规范.md）")
print("=" * 70)
for k, (tot, bad) in stats.items():
    flag = '✅' if bad == 0 else '❌'
    print(f"  判据{k:8s}: {tot} 份，违规 {bad} 份  {flag}")

if problems:
    print()
    print(f"  ❌ 共 {len(problems)} 处违规：")
    for crit, p, msg in problems:
        print(f"     [{crit}] {p}  {msg}")

if REPORT_ONLY:
    print()
    print("  ℹ️  --report 模式：不判失败")
    sys.exit(0)

if problems:
    print()
    print("  ❌ 文档骨架未达标（修法见 docs/constraints/06-文档骨架规范.md）")
    sys.exit(1)

print()
print("  ✅ 文档骨架全部达标")
sys.exit(0)
PYEOF
