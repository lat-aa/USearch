#!/bin/sh
# shellcheck disable=SC2034  # 保留契约 flag（ActualModelSource/CtxPicked）由渲染器消费，本脚本暂不渲染
# 将 apex（rules / decide / cost）结果渲染成七块报告。
# 第 7 块：摘要一行 + 消息行（PromptFile=turn.prompt messages，自动剔除 knowledge 行）+ 人读 corpus（CorpusFile）。
# 前六行句式冻结；Compression = 保留上下文比例。用法见 AGENTS.md。
set -eu
SCRIPT_DIR=$(cd -- "$(dirname "$0")" && pwd)

Style=clear
IconPreset=a
Total=0; Matched=0; Naive=0; Optimized=0; Selected=0
Ids=
Model=; Depth=; Retrieval=; Confidence=0; Compression=0; Reason=
NaiveCost=0; OptimizedCost=0; SavedCost=0; OutputCost=0; TotalCost=0
CostModel=deepseek-flash
ActualModel=; ActualModelSource=
ViaV1=0
CacheHit=0; Peak=0
EstimateBias=-1
Nanbeige=; USearch=; Sqlite=
# turn 实测：空=未上报（禁止默认 0 冒充）
Gate=; Cache=; Saved=; Local=; Queued=; Distill=
Retain=; CtxNaive=; CtxPicked=; CtxKept=; PackTok=; PackN=
PromptSource=
TokenMode=
CorpusFile=
PromptFile=

# 读 flag 后一参；缺省为空（Windows 常把 -ActualModel '' 吃掉）
while [ $# -gt 0 ]; do
  key=$1
  val=${2-}
  # 有第二参则双移；否则单移（空值被宿主丢掉时）
  if [ $# -ge 2 ]; then shift 2; else shift 1; fi
  case "$key" in
    -Style|--style) Style=$val ;;
    -IconPreset|--icon-preset) IconPreset=$val ;;
    -Total|--total) Total=$val ;;
    -Matched|--matched) Matched=$val ;;
    -Naive|--naive) Naive=$val ;;
    -Optimized|--optimized) Optimized=$val ;;
    -Selected|--selected) Selected=$val ;;
    -Ids|--ids) Ids=$val ;;
    -Model|--model) Model=$val ;;
    -Depth|--depth) Depth=$val ;;
    -Retrieval|--retrieval) Retrieval=$val ;;
    -Confidence|--confidence) Confidence=$val ;;
    -Compression|--compression) Compression=$val ;;
    -Reason|--reason) Reason=$val ;;
    -NaiveCost|--naive-cost) NaiveCost=$val ;;
    -OptimizedCost|--optimized-cost) OptimizedCost=$val ;;
    -SavedCost|--saved-cost) SavedCost=$val ;;
    -OutputCost|--output-cost) OutputCost=$val ;;
    -TotalCost|--total-cost) TotalCost=$val ;;
    -CostModel|--cost-model) CostModel=$val ;;
    -ActualModel|--actual-model) ActualModel=$val ;;
    -ActualModelSource|--actual-model-source) ActualModelSource=$val ;;
    -ViaV1|--via-v1) ViaV1=$val ;;
    -CacheHit|--cache-hit) CacheHit=$val ;;
    -Peak|--peak) Peak=$val ;;
    -EstimateBias|--estimate-bias) EstimateBias=$val ;;
    -Nanbeige|--nanbeige) Nanbeige=$val ;;
    -USearch|--usearch) USearch=$val ;;
    -Sqlite|--sqlite) Sqlite=$val ;;
    -Gate|--gate) Gate=$val ;;
    -Cache|--cache) Cache=$val ;;
    -Saved|--saved) Saved=$val ;;
    -Local|--local) Local=$val ;;
    -Queued|--queued) Queued=$val ;;
    -Distill|--distill) Distill=$val ;;
    -Retain|--retain) Retain=$val ;;
    -CtxNaive|--ctx-naive) CtxNaive=$val ;;
    -CtxPicked|--ctx-picked) CtxPicked=$val ;;
    -CtxKept|--ctx-kept) CtxKept=$val ;;
    -PackTok|--pack-tok) PackTok=$val ;;
    -PackN|--pack-n) PackN=$val ;;
    -PromptSource|--prompt-source) PromptSource=$val ;;
    -TokenMode|--token-mode) TokenMode=$val ;;
    -CorpusFile|--corpus-file) CorpusFile=$val ;;
    -PromptFile|--prompt-file) PromptFile=$val ;;
    *) echo "unknown arg: $key" >&2; exit 2 ;;
  esac
done

# bool：接受 0/1/true/false
case "$(printf '%s' "$CacheHit" | tr 'A-Z' 'a-z')" in true|1|yes|on) CacheHit=1 ;; *) CacheHit=0 ;; esac
case "$(printf '%s' "$Peak" | tr 'A-Z' 'a-z')" in true|1|yes|on) Peak=1 ;; *) Peak=0 ;; esac
case "$(printf '%s' "$ViaV1" | tr 'A-Z' 'a-z')" in true|1|yes|on) ViaV1=1 ;; *) ViaV1=0 ;; esac

BiasNote=heuristic
if [ "$(printf '%.0f' "$EstimateBias" 2>/dev/null || echo -1)" = "-1" ] || [ "$EstimateBias" = "-1" ]; then
  biasPath="$SCRIPT_DIR/tokenbias.json"
  EstimateBias=0
  if [ -f "$biasPath" ]; then
    EstimateBias=$(python3 -c "import json;print(json.load(open('$biasPath'))['biasPercent'])" 2>/dev/null \
      || python -c "import json;print(json.load(open(r'$biasPath'))['biasPercent'])" 2>/dev/null \
      || echo 0)
    BiasNote=$(python3 -c "import json;b=json.load(open('$biasPath'));print(f\"{b.get('basis','?')}，{b.get('measuredAt','?')} 实测\")" 2>/dev/null \
      || echo '未实测')
  else
    BiasNote=未实测
  fi
fi

# 计算派生量（awk 避免依赖 bc）
saved=$((Naive - Optimized))
savedPct=$(awk -v n="$Naive" -v s="$saved" 'BEGIN{ if(n>0) printf "%.1f", (s/n)*100; else print "0.0"}')
pickSaved=0; trimSaved=0
if [ "$Selected" -gt 0 ] 2>/dev/null; then
  pickSaved=$((Naive - Selected))
  trimSaved=$((Selected - Optimized))
fi
confPct=$(awk -v c="$Confidence" 'BEGIN{ if (c>1) printf "%d", c+0.5; else printf "%d", c*100+0.5 }')
retainPct=$(awk -v c="$Compression" 'BEGIN{printf "%d", c*100+0.5}')
# 📦 摘要优先用 turn.retain；否则回退 Compression
if [ -n "$Retain" ]; then
  ctxRetainPct=$(awk -v c="$Retain" 'BEGIN{printf "%d", c*100+0.5}')
else
  ctxRetainPct=$retainPct
fi

modelCn=$Model
case "$Model" in weak) modelCn=轻量 ;; standard) modelCn=标准 ;; strong) modelCn=增强 ;; esac
depthCn=$Depth
case "$Depth" in shallow) depthCn=浅层推理 ;; medium) depthCn=中层推理 ;; deep) depthCn=深层推理 ;; esac
retCn=$Retrieval
case "$Retrieval" in L0) retCn=不做检索 ;; L1) retCn=关键词检索 ;; L2) retCn=语义检索 ;; L3) retCn=深度语义检索 ;; esac
if [ "$CacheHit" = 1 ]; then cacheCn=命中缓存; else cacheCn=未命中缓存; fi
if [ "$Peak" = 1 ]; then peakCn=高峰时段; else peakCn=空闲时段; fi

IdsFmt=$(printf '%s' "$Ids" | tr ',' ' ' | awk '{for(i=1;i<=NF;i++) if($i!=""){printf "%s%s",(n++?" · ":""),$i}}')

# 栈动作缺省：禁止瞎编；未传则显示「未上报」
[ -n "$Nanbeige" ] || Nanbeige=未上报
[ -n "$USearch" ] || USearch=未上报
[ -n "$Sqlite" ] || Sqlite=未上报

# 🔖 追加入队/蒸馏（有才写）
stackExtra=
if [ -n "$Distill" ]; then
  DistillFmt=$(printf '%s' "$Distill" | tr ',' ' ' | awk '{for(i=1;i<=NF;i++) if($i!=""){printf "%s%s",(n++?"·":""),$i}}')
  stackExtra="$stackExtra · 蒸馏 $DistillFmt"
fi
if [ -n "$Queued" ]; then
  QueuedFmt=$(printf '%s' "$Queued" | tr ',' ' ' | awk '{for(i=1;i<=NF;i++) if($i!=""){printf "%s%s",(n++?"·":""),$i}}')
  stackExtra="$stackExtra · 入队 $QueuedFmt"
fi

if [ "$Style" = plain ]; then
  printf 'totalRules = %s\nmatched = %s\nmatchedIds = %s\nnaiveTokens = %s\nselectedTokens = %s\noptimizedTokens = %s\nsavedTokens = %s (selection %s + trim %s)\nsavedPercent = %s\n' \
    "$Total" "$Matched" "$(printf '%s' "$Ids" | tr ' ' ',')" "$Naive" "$Selected" "$Optimized" "$saved" "$pickSaved" "$trimSaved" "$savedPct"
  printf 'stack = %s · USearch %s · SQLite %s\n' "$Nanbeige" "$USearch" "$Sqlite"
  printf 'decision = model=%s depth=%s retrieval=%s confidence=%s contextRetention=%s\nreason = %s\n' \
    "$Model" "$Depth" "$Retrieval" "$Confidence" "$Compression" "$Reason"
  printf 'cost = naive:%s optimized:%s saved:%s output:%s total:%s CNY (%s; %s; %s)\n' \
    "$NaiveCost" "$OptimizedCost" "$SavedCost" "$OutputCost" "$TotalCost" "$CostModel" "$cacheCn" "$peakCn"
  printf 'gate = %s cache = %s saved = %s local = %s\n' "${Gate:-未上报}" "${Cache:-未上报}" "${Saved:-未上报}" "${Local:-未上报}"
  exit 0
fi

case "$IconPreset" in
  b) ic_stat=📈; ic_stack=🔖; ic_route=🧭; ic_cost=💰; ic_hit=📌; ic_why=ℹ️; ic_ctx=📦 ;;
  c) ic_stat=🎯; ic_stack=🔖; ic_route=🧭; ic_cost=💰; ic_hit=📎; ic_why=📝; ic_ctx=📦 ;;
  *) ic_stat=⚡; ic_stack=🔖; ic_route=🧭; ic_cost=💰; ic_hit=🏷️; ic_why=💡; ic_ctx=📦 ;;
esac

routeName=
# A：有调 /v1 → 🧭 = 上游 model；B：纯 MCP → 客户端 ActualModel（可空，禁止 flash 冒充）
if [ "$ViaV1" = 1 ]; then
  routeName=${CostModel:-deepseek-flash}
  # 与纯 MCP 同级简洁：主名上游 id；客户端实模仅作次要标注
  if [ -n "$ActualModel" ] && [ "$ActualModel" != "$routeName" ]; then
    modelCn="客户端 ${ActualModel}"
  else
    modelCn=
  fi
elif [ -n "$ActualModel" ]; then
  routeName=$ActualModel
  modelCn=
else
  routeName=
  modelCn="建议档·${modelCn} · 未调/v1"
fi

reasonCn=$Reason
# shellcheck disable=SC2001
if printf '%s' "$Reason" | grep -Eq 'complexity[[:space:]]+[0-9]+.*\((low|mid|high)\)'; then
  n=$(printf '%s' "$Reason" | sed -n 's/.*complexity[[:space:]]*\([0-9][0-9]*\).*/\1/p')
  band=$(printf '%s' "$Reason" | sed -n 's/.*(\(low\|mid\|high\)).*/\1/p')
  case "$band" in low) band=浅层 ;; mid) band=中层 ;; high) band=深层 ;; esac
  reasonCn="复杂度${n}（${band}）"
fi
if [ "$TokenMode" = real ]; then
  biasCn='token 实测'
elif [ "$BiasNote" = 未实测 ]; then
  biasCn='token 为估算值'
else
  biasAbs=$(awk -v b="$EstimateBias" 'BEGIN{ if(b<0) b=-b; printf "%s", b}')
  if awk -v b="$EstimateBias" 'BEGIN{exit !(b<0)}'; then biasSign='-'; else biasSign='+'; fi
  biasCn="token 估算偏差 ${biasSign}${biasAbs}%"
fi

totalFmt=$(awk -v t="$TotalCost" 'BEGIN{printf "%.6f", t+0}')
outFmt=$(awk -v t="$OutputCost" 'BEGIN{printf "%.6f", t+0}')
if [ "$ViaV1" = 1 ]; then
  priceCn="计价 ${CostModel}"
else
  # 纯 MCP 未打 DeepSeek：无上游费用，禁止把规则估算当成实付
  totalFmt=0.00
  outFmt=0.00
  priceCn="未调/v1"
fi

# 💰 / 🏷️ 仅在 turn 明确上报时追加
costExtra=
if [ -n "$Saved" ]; then
  costExtra="$costExtra · 主 LLM 省 **${Saved}** 次"
fi
if [ -n "$Local" ]; then
  costExtra="$costExtra · 本地 chat **${Local}**"
fi
hitExtra=
if [ -n "$Gate" ]; then
  hitExtra="$hitExtra · gate=${Gate}"
fi
if [ -n "$Cache" ]; then
  hitExtra="$hitExtra · cache=${Cache}"
fi

# Markdown 聊天会吞单换行：行尾两空格强制硬换行
printf '%s 规则 **%s**/**%s** 命中 · token **%s → %s** · **省 %s%%**（仅规则/知识注入）  \n' \
  "$ic_stat" "$Matched" "$Total" "$Naive" "$Optimized" "$savedPct"
printf '%s 决策 %s · USearch %s · SQLite %s%s  \n' \
  "$ic_stack" "$Nanbeige" "$USearch" "$Sqlite" "$stackExtra"
if [ -n "$routeName" ] && [ -n "$modelCn" ]; then
  printf '%s 路由 %s（%s） · %s · %s · 保留上下文 %s%% · 置信 %s%%  \n' \
    "$ic_route" "$routeName" "$modelCn" "$depthCn" "$retCn" "$retainPct" "$confPct"
elif [ -n "$routeName" ]; then
  printf '%s 路由 %s · %s · %s · 保留上下文 %s%% · 置信 %s%%  \n' \
    "$ic_route" "$routeName" "$depthCn" "$retCn" "$retainPct" "$confPct"
else
  printf '%s 路由 %s · %s · %s · 保留上下文 %s%% · 置信 %s%%  \n' \
    "$ic_route" "$modelCn" "$depthCn" "$retCn" "$retainPct" "$confPct"
fi
printf '%s 费用 **¥%s** · %s · %s · %s · 输出 ¥%s%s  \n' \
  "$ic_cost" "$totalFmt" "$priceCn" "$cacheCn" "$peakCn" "$outFmt" "$costExtra"
printf '%s 命中规则 %s · 省量 筛选 **%s** ＋ 裁剪 **%s**%s  \n' \
  "$ic_hit" "$IdsFmt" "$pickSaved" "$trimSaved" "$hitExtra"
printf '%s 依据 %s · %s  \n' \
  "$ic_why" "$reasonCn" "$biasCn"

# 第 7 块：摘要一行 + 人读 corpus 正文（CorpusFile；禁止 "..." 截断）
fmtOrUnknown() {
  if [ -n "$1" ]; then printf '%s' "$1"; else printf '未上报'; fi
}
naiveCtx=$(fmtOrUnknown "$CtxNaive")
keptCtx=$(fmtOrUnknown "$CtxKept")
# 裁剪 token = naive - kept（缺任一则未上报）
if [ -n "$CtxNaive" ] && [ -n "$CtxKept" ]; then
  trimCtx=$((CtxNaive - CtxKept))
  if [ "$trimCtx" -lt 0 ] 2>/dev/null; then trimCtx=0; fi
else
  trimCtx=未上报
fi
if [ -n "$PackN" ] || [ -n "$PackTok" ]; then
  pn=${PackN:-0}
  pt=${PackTok:-0}
  packCtx="${pn}条/${pt}tok"
else
  packCtx=未上报
fi
srcCtx=$(fmtOrUnknown "$PromptSource")
printf '%s 上下文 保留 %s%% · naive %s → kept %s · 裁剪 %s · gatePack %s · 注入 %s  \n' \
  "$ic_ctx" "$ctxRetainPct" "$naiveCtx" "$keptCtx" "$trimCtx" "$packCtx" "$srcCtx"
shown=0
# 仅在 source=injected（真实 /v1 注入）时贴消息行；rebuild 是合成内容，不贴
if [ -n "$PromptFile" ] && [ -f "$PromptFile" ] && [ "$PromptSource" = "injected" ]; then
  # 消息行：一行一条；剔除 rules-engine 注入的 knowledge 行（以 Local knowledge JSON follows 开头）
  printf '## prompt\n'
  grep -v 'Local knowledge JSON follows' "$PromptFile" || true
  if [ "$(tail -c1 "$PromptFile" | wc -l)" -eq 0 ]; then
    printf '\n'
  fi
  if [ -n "$CorpusFile" ] && [ -f "$CorpusFile" ]; then
    sed '1{/^## prompt$/d;}' "$CorpusFile"
    if [ "$(tail -c1 "$CorpusFile" | wc -l)" -eq 0 ]; then
      printf '\n'
    fi
  fi
  shown=1
elif [ -n "$CorpusFile" ] && [ -f "$CorpusFile" ]; then
  cat "$CorpusFile"
  if [ "$(tail -c1 "$CorpusFile" | wc -l)" -eq 0 ]; then
    printf '\n'
  fi
  shown=1
fi
if [ "$shown" -eq 0 ]; then
  printf '未上报\n'
fi
