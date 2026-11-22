#!/bin/sh
# curl 冒烟测试：用真实 HTTP 客户端验证服务端基本行为。
# 用法：sh tools/stage3/curl_smoke.sh <base_url> <token> <dev8>
# 例：  sh tools/stage3/curl_smoke.sh http://127.0.0.1:8000 tok-xxx 98c377f4
set -u

BASE=${1:?用法: curl_smoke.sh <base_url> <token> <dev8>}
TOKEN=${2:?缺少 token}
DEV=${3:?缺少设备前缀（8 位十六进制）}
SID="${DEV}-$(date +%s)-c0ffee01"
URL="$BASE/v1/upload/$SID"
PASS=0
TOTAL=0

# 几乎所有 NAS 都带 curl；--fail-with-body 需要 7.76+，退化为普通 -o。
chk() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "$2" ]; then
        PASS=$((PASS + 1))
        printf '  PASS  %s (got %s)\n' "$3" "$1"
    else
        printf '  FAIL  %s (got %s, want %s)\n' "$3" "$1" "$2"
    fi
}

printf 'BASE=%s\nSID=%s\n\n' "$BASE" "$SID"

printf 'hello-aipass' > /tmp/smoke_a.bin
printf 'world-second' > /tmp/smoke_b.bin

echo '== 健康检查 =='
code=$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/healthz")
chk "$code" 200 "healthz"

echo '== 首段 =='
code=$(curl -sS -o /tmp/smoke_r1.json -w '%{http_code}' -X PUT "$URL" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    -H 'X-Offset: 0' -H 'X-Seq: 0' --data-binary @/tmp/smoke_a.bin)
chk "$code" 200 "首段 offset=0"

echo '== 续传 =='
code=$(curl -sS -o /tmp/smoke_r2.json -w '%{http_code}' -X PUT "$URL" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    -H 'X-Offset: 12' -H 'X-Seq: 1' --data-binary @/tmp/smoke_b.bin)
chk "$code" 200 "续传 offset=12"

echo '== 幂等重复 =='
code=$(curl -sS -o /tmp/smoke_r3.json -w '%{http_code}' -X PUT "$URL" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    -H 'X-Offset: 12' -H 'X-Seq: 1' --data-binary @/tmp/smoke_b.bin)
chk "$code" 200 "重复段仍 200"
if grep -q '"duplicate"' /tmp/smoke_r3.json; then
    chk ok ok "返回 duplicate"
else
    chk "$(cat /tmp/smoke_r3.json)" duplicate "返回 duplicate"
fi

echo '== 偏移超前 =='
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    -H 'X-Offset: 9999' -H 'X-Seq: 2' --data-binary @/tmp/smoke_b.bin)
chk "$code" 409 "offset 超前 -> 409"

echo '== 鉴权 =='
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL" \
    -H 'Authorization: Bearer wrong-token' -H 'X-Offset: 0' \
    --data-binary @/tmp/smoke_a.bin)
chk "$code" 401 "错 token -> 401"

echo '== final =='
TOTAL_LEN=$((12 + 12))
code=$(curl -sS -o /tmp/smoke_r4.json -w '%{http_code}' -X PUT "$URL" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
    -H 'X-Offset: 24' -H 'X-Seq: 3' -H 'X-Final: 1' -H "X-Total-Len: $TOTAL_LEN" \
    --data-binary '')
chk "$code" 200 "final"

echo
printf 'PASS %s/%s\n' "$PASS" "$TOTAL"
[ "$PASS" = "$TOTAL" ] || exit 1