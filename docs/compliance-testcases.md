# GA/T 2155.2-2024 自测用例骨架（P6.0）

- 文档版本：1.0，2026-11-24
- 对应差距矩阵：[`compliance-gap.md`](compliance-gap.md)
- 适用代码：`night/p6-0-gap`（基线提交 `443e78b`）
- 执行环境：Linux x86_64，已按 README 构建出 `build/uvnvr`；需要 `curl`、`python3`、`openssl`、`timeout`。
  `tcpdump`/`jq` 不是必需依赖。

> 本文件是**自测用例骨架**，不是检测机构报告。每个用例都可在当前代码上执行并得到可观察结果。
> 对“部分/缺失”条目，用例同时写明：
> - **当前预期（差距）**：当前代码实际会呈现的行为，用于证明差距存在；
> - **整改后验收**：P6.1–P6.4 完成整改后，同一用例应得到的结果。
>
> 用例按 `compliance-gap.md` 的 22 个“部分”和 13 个“缺失”条目一一对应，共 35 条。
> 11 个“满足”条目不重复写用例，其证据位置已在矩阵中给出，回归时并入现有
> `tests/integration/m1_auth.sh` 与新增 CI 用例。

## 1. 约定

- 所有命令默认在仓库根目录执行，二进制为 `./build/uvnvr`。
- 每个用例在**独立的 shell 和临时数据目录**中执行，避免污染现有数据，也避免前一个用例的锁定/
  安全设置影响后一个用例。不要在同一次 `source` 中连续运行多个会修改 `security.*` 的用例。
- 用例中的 `$BASE`、`$DATA`、`$COOKIE`、`$CSRF`、`api`、`start_server`、`setup_admin`、
  `login_admin` 来自 §2 的公共环境脚本。
- HTTP 状态码用 `curl -w '%{http_code}'` 采集；响应体保存在 `/tmp/uvnvr-case-body.json`。
- “证据形式”列给出应归档的文件或终端记录；P6.4 送检材料按用例编号建目录。
- 行号基于基线提交 `443e78b`；整改后行号会变化，代码位置以函数/路由名称为准。

## 2. 公共测试环境

将以下内容保存为 `/tmp/uvnvr_compliance_env.sh`，每个用例先 `source` 它。
脚本故意不启用 `set -e`，因为部分用例需要观察非 2xx 响应。

```bash
#!/usr/bin/env bash
# UvNVR GA/T 2155.2 自测公共环境（仅用于测试，不进入仓库）
set -uo pipefail

BIN="${BIN:-./build/uvnvr}"
PORT="${PORT:-18180}"
BASE="http://127.0.0.1:${PORT}"
DATA="${DATA:-$(mktemp -d)}"
COOKIE="${COOKIE:-$(mktemp)}"
CSRF=""
PID=""
BODY="/tmp/uvnvr-case-body.json"

start_server() {
  "$BIN" --port "$PORT" --data-dir "$DATA" --log-level warn "$@" \
    >"${DATA}/server.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 100); do
    # 同时探测 HTTP 与 HTTPS：TLS 模式下 healthz 只在 https 上可达。
    if curl -fsS --max-time 2 "${BASE}/healthz" >/dev/null 2>&1 \
       || curl -kfsS --max-time 2 "https://127.0.0.1:${PORT}/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "server did not start; log:" >&2
  cat "${DATA}/server.log" >&2
  return 1
}

stop_server() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  PID=""
}

setup_admin() {
  local out
  out=$(curl -fsS --max-time 10 -c "${COOKIE}" -H 'Content-Type: application/json' \
    -d '{"password":"Admin123!@#","confirmPassword":"Admin123!@#","timezone":"UTC"}' \
    "${BASE}/api/v1/setup")
  CSRF=$(python3 -c 'import json,sys;print(json.load(sys.stdin)["csrfToken"])' <<<"${out}")
}

login_admin() {
  local out
  out=$(curl -fsS --max-time 10 -c "${COOKIE}" -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"Admin123!@#"}' \
    "${BASE}/api/v1/auth/login")
  CSRF=$(python3 -c 'import json,sys;print(json.load(sys.stdin)["csrfToken"])' <<<"${out}")
}

api() { # api METHOD PATH [JSON_BODY]
  local method="$1" path="$2" body="${3-}"
  if [[ -n "${body}" ]]; then
    curl -sS --max-time 10 -o "${BODY}" -w '%{http_code}' \
      -b "${COOKIE}" -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
      -X "${method}" -d "${body}" "${BASE}${path}"
  else
    curl -sS --max-time 10 -o "${BODY}" -w '%{http_code}' \
      -b "${COOKIE}" -X "${method}" "${BASE}${path}"
  fi
}

cleanup_env() {
  stop_server
  rm -rf "${DATA}" "${COOKIE}" "${BODY}"
}
trap cleanup_env EXIT
```

用例执行前的最小步骤：

```bash
source /tmp/uvnvr_compliance_env.sh
start_server
setup_admin
```

> 如果用例需要新的数据目录，在 `source` 前设置 `DATA=$(mktemp -d)`，或直接 `unset DATA`。
> 需要重新登录时使用 `login_admin`（已有账户）。

## 3. 身份鉴别用例

### TC-IDN-05 登录失败锁定的账户/来源隔离

- **关联条目**：IDN-05（部分）
- **前置条件**：§2 环境；已 `setup_admin`；当前仅有 admin 一个账户。
- **步骤**：

```bash
# 1. 把 Web 失败阈值降到 3 次，锁定 1 分钟
api PUT /api/v1/settings '{"security":{"webMaxFailures":3,"webLockMinutes":1}}'; echo

# 2. 用不存在的账户 alice 连续失败 3 次（不携带 Cookie）
for i in 1 2 3; do
  curl -sS -o /dev/null -w "alice#$i=%{http_code}\n" \
    -H 'Content-Type: application/json' \
    -d '{"username":"alice","password":"WrongPass1!"}' \
    "${BASE}/api/v1/auth/login"
done

# 3. 立刻用正确的 admin 口令登录：当前全局锁会让 admin 也被拒绝
curl -sS -D /tmp/tc-idn05-headers -o /tmp/tc-idn05-body -w "admin=%{http_code}\n" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"Admin123!@#"}' \
  "${BASE}/api/v1/auth/login"
grep -i '^retry-after:' /tmp/tc-idn05-headers || true
cat /tmp/tc-idn05-body; echo

# 4. 查询审计中的登录失败记录
api GET '/api/v1/audit?category=auth&pageSize=50'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
for it in d.get('items',[])[:10]:
    print(it['actor'], it['action'], it['result'], it.get('errorCode'))
PY
```

- **期望**：
  - **当前预期（差距）**：第 3 步返回 `429`，响应体 `code=locked`，表示全局锁把管理员也锁住；
    审计中有 `auth/login/failure`，但无法区分“alice 被锁”和“admin 被锁”。
  - **整改后验收**：alice 的第 4 次尝试返回 `401`/`429` 且只影响 alice+来源 IP；
    来自另一来源 IP 的 admin 正确登录返回 `200`；审计含 `locked=true`、账户、IP、解锁时间；
    管理员账户有独立阈值；锁定事件触发 `auth_lockout` 告警。
- **证据形式**：终端状态码输出、`/tmp/tc-idn05-headers`、审计 JSON、`server.log` 中的失败记录。

### TC-IDN-06 会话绝对超时、空闲超时与注销失效

- **关联条目**：IDN-06（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 登录并抓取 Set-Cookie 与登录响应
curl -sS -D /tmp/tc-idn06-headers -o /tmp/tc-idn06-login -c "${COOKIE}" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"Admin123!@#"}' \
  "${BASE}/api/v1/auth/login"
grep -i '^set-cookie:' /tmp/tc-idn06-headers
python3 - <<'PY'
import json
d=json.load(open('/tmp/tc-idn06-login'))
print('expiresAt', d.get('expiresAt'))
PY

# 2. 检查会话表：只有绝对过期时间，没有空闲时间/续期计数
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1]); db.row_factory=sqlite3.Row
rows=db.execute("select created_at, expires_at, expires_at-created_at as ttl from sessions").fetchall()
for r in rows: print(dict(r), 'ttl_hours=', r['ttl']/3600)
print([c[1] for c in db.execute("pragma table_info(sessions)")])
PY

# 3. 注销后用旧 Cookie 访问
CSRF=$(python3 -c 'import json;print(json.load(open("/tmp/tc-idn06-login"))["csrfToken"])')
curl -sS -o /dev/null -w "logout=%{http_code}\n" -b "${COOKIE}" \
  -H "X-CSRF-Token: ${CSRF}" -X POST "${BASE}/api/v1/auth/logout"
curl -sS -o /dev/null -w "after_logout=%{http_code}\n" -b "${COOKIE}" \
  "${BASE}/api/v1/auth/me"
```

- **期望**：
  - **当前预期（差距）**：`ttl_hours=24`；会话表没有 `last_seen`/`idle_expires_at`/`renew_count`；
    注销后旧 Cookie 返回 `401`（这一项已满足），但空闲 24 小时内不会失效，也没有“会话管理/踢出”。
  - **整改后验收**：默认空闲 30 分钟、绝对 24 小时；每次请求滑动续期但不超过绝对上限；
    会话表含 `last_seen_at`；管理员可列出/撤销会话；改密后旧会话立即 `401`。
- **证据形式**：Set-Cookie 头、`pragma table_info(sessions)` 输出、注销前后状态码、会话表快照。

### TC-IDN-07 敏感操作二次认证与双因子

- **关联条目**：IDN-07（缺失）
- **前置条件**：§2 环境；为验证维护操作，使用 `start_server --maintenance` 启动并 `setup_admin`。
- **步骤**：

```bash
# 1. 仅凭已登录管理员会话执行高危操作，不提供口令/OTP
api POST /api/v1/audit/purge '{"confirm":"purge"}'; echo
cat "${BODY}"; echo

# 2. 检查是否存在 MFA/TOTP/WebAuthn 实现
grep -RniE 'totp|webauthn|two[ -]?factor|\bmfa\b' src/ || echo "NO MFA IMPLEMENTATION"

# 3. 检查是否存在再认证入口
grep -RniE 'reauth|re-auth|confirmPassword.*maintenance|currentPassword' src/ || echo "NO REAUTH"
```

- **期望**：
  - **当前预期（差距）**：第 1 步返回 `200`，审计清除在只有会话 + CSRF 的情况下完成；
    第 2、3 步均输出 `NO ...`。
  - **整改后验收**：第 1 步返回 `401/403`，要求 `otp` 或再次输入口令；
    管理员登录需要 TOTP（首次绑定 + 恢复码）；审计清除、格式化、密钥/证书导入导出均要求二次认证；
    失败与成功都写审计。
- **证据形式**：状态码、响应体、两条 grep 输出、审计中的 `purge` 记录。

### TC-IDN-08 口令/角色变更后的会话失效

- **关联条目**：IDN-08（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 创建 operator 账户并用第二个 Cookie 登录
api POST /api/v1/users '{"username":"op1","password":"Operator123!","roleId":2}'; echo
cat "${BODY}"; echo
OP_ID=$(python3 -c 'import json;print(json.load(open("/tmp/uvnvr-case-body.json"))["id"])')

COOKIE2=$(mktemp)
curl -sS -c "${COOKIE2}" -o /tmp/tc-idn08-login \
  -H 'Content-Type: application/json' \
  -d '{"username":"op1","password":"Operator123!"}' \
  "${BASE}/api/v1/auth/login"
curl -sS -o /dev/null -w "op_before=%{http_code}\n" -b "${COOKIE2}" "${BASE}/api/v1/auth/me"

# 2. 管理员重置 op1 口令（不改变 enabled）
api PUT "/api/v1/users/${OP_ID}" '{"password":"NewOperator123!"}'; echo

# 3. 旧会话仍然可用？
curl -sS -o /dev/null -w "op_after_password_change=%{http_code}\n" \
  -b "${COOKIE2}" "${BASE}/api/v1/auth/me"

# 4. 作为对照：禁用账户后旧会话立即失效
api PUT "/api/v1/users/${OP_ID}" '{"enabled":false}'; echo
curl -sS -o /dev/null -w "op_after_disable=%{http_code}\n" \
  -b "${COOKIE2}" "${BASE}/api/v1/auth/me"
rm -f "${COOKIE2}"
```

- **期望**：
  - **当前预期（差距）**：`op_before=200`，`op_after_password_change=200`（旧会话未失效），
    `op_after_disable=401`（禁用会删会话）。
  - **整改后验收**：改密后 `op_after_password_change=401`；改角色后同样 `401`；
    API Token 同时失效；审计记录 `passwordChanged=true` 与会话撤销数量。
- **证据形式**：三个状态码、用户更新审计 JSON、`sessions` 表行数。

## 4. 访问控制用例

### TC-ACC-02 通道/录像/抓图的对象级越权

- **关联条目**：ACC-02（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 创建两个通道（不要求摄像机在线）
for n in cam-alpha cam-beta; do
  api POST /api/v1/channels "{\"name\":\"${n}\",\"protocol\":\"rtsp\",\"host\":\"127.0.0.1\",\"port\":8554,\"mainUrl\":\"rtsp://127.0.0.1:9/none\"}"; echo
  cat "${BODY}"; echo
done

# 2. 创建 operator（有 channel.view / preview.view / api.snapshot，无 user.manage）
api POST /api/v1/users '{"username":"op2","password":"Operator123!","roleId":2}'; echo
COOKIE3=$(mktemp)
curl -sS -c "${COOKIE3}" -o /dev/null -H 'Content-Type: application/json' \
  -d '{"username":"op2","password":"Operator123!"}' "${BASE}/api/v1/auth/login"

# 3. operator 能看到多少通道？能否按 id 直接访问第二个通道的编码器配置？
curl -sS -b "${COOKIE3}" "${BASE}/api/v1/channels" -o /tmp/tc-acc02-channels
python3 - <<'PY'
import json
d=json.load(open('/tmp/tc-acc02-channels'))
items=d.get('items',[]) if isinstance(d,dict) else d
if not isinstance(items,list): items=[]
print('visible_channels=',len(items), [i.get('name') for i in items])
PY
curl -sS -b "${COOKIE3}" -o /dev/null -w "encoder_channel_2=%{http_code}\n" \
  "${BASE}/api/v1/channels/2/encoder"
```

- **期望**：
  - **当前预期（差距）**：operator 的 `visible_channels=2`；对第二个通道的编码器接口
    `encoder_channel_2` 不是 `403`（无摄像机时可能是 `502`/`404`，但说明已越过对象授权进入业务逻辑），
    证明没有通道级 ACL。
  - **整改后验收**：只授予 `cam-alpha` 时，列表仅返回 1 条；对 `cam-beta` 的列表/编码器/快照/
    回放/导出返回 `403` 或 `404`，并写 `permission` 审计；私有协议 `CHANNEL_LIST`/`LIVE_OPEN` 同样受限。
- **证据形式**：通道列表 JSON、状态码、operator 的权限集合、审计。

### TC-ACC-04 管理面来源限制

- **关联条目**：ACC-04（缺失）
- **前置条件**：§2 环境；已 `setup_admin`；主机至少有一个非回环 IPv4 地址。
- **步骤**：

```bash
SECOND_IP=$(hostname -I | awk '{print $1}')
echo "second_ip=${SECOND_IP}"

# 1. 确认默认监听地址
api GET /api/v1/system/config; echo
python3 -c 'import json;d=json.load(open("/tmp/uvnvr-case-body.json"));print(d["http"])'

# 2. 从非回环地址登录并访问管理员接口
COOKIE4=$(mktemp)
curl -sS -c "${COOKIE4}" -o /dev/null -w "login_via_lan=%{http_code}\n" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"Admin123!@#"}' \
  "http://${SECOND_IP}:${PORT}/api/v1/auth/login"
curl -sS -b "${COOKIE4}" -o /dev/null -w "users_via_lan=%{http_code}\n" \
  "http://${SECOND_IP}:${PORT}/api/v1/users"
rm -f "${COOKIE4}"
```

- **期望**：
  - **当前预期（差距）**：`http.host=0.0.0.0`；`login_via_lan=200`，`users_via_lan=200`，
    说明管理接口接受任意来源。
  - **整改后验收**：配置 `security.admin_allow_cidrs` 后，非允许来源的登录/管理请求返回 `403`
    或连接被拒绝；允许来源正常；拒绝事件写审计并触发告警；业务面（直播/回放）可单独放行。
- **证据形式**：`system/config` JSON、两个状态码、`ss -ltnp` 监听地址、审计/告警。

## 5. 安全审计用例

### TC-AUD-01 关键操作与媒体访问留痕

- **关联条目**：AUD-01（部分）
- **前置条件**：§2 环境；使用 `start_server --rtsp-port 18554` 启动并已 `setup_admin`。
- **步骤**：

```bash
# 1. 触发一组操作：失败登录、创建用户、修改设置、创建通道
curl -sS -o /dev/null -H 'Content-Type: application/json' \
  -d '{"username":"nobody","password":"WrongPass1!"}' "${BASE}/api/v1/auth/login"
api POST /api/v1/users '{"username":"audit1","password":"AuditPass123!","roleId":3}'; echo
api PUT /api/v1/settings '{"audit":{"retentionDays":180}}'; echo
api POST /api/v1/channels '{"name":"audit-cam","protocol":"rtsp","host":"127.0.0.1","port":8554,"mainUrl":"rtsp://127.0.0.1:9/none"}'; echo

# 2. 打开一次 RTSP 会话（会返回 401 或 200），随后检查审计
python3 - "${PORT}" <<'PY'
import socket,sys
port=18554  # 见 TC-COM-03；若未用该端口，改为实际 RTSP 端口
try:
    s=socket.create_connection(('127.0.0.1',port),2); s.settimeout(2)
    s.sendall(b'OPTIONS rtsp://127.0.0.1:%d/live/1/main RTSP/1.0\r\nCSeq: 1\r\n\r\n'%port)
    print(s.recv(256).split(b'\r\n')[0]); s.close()
except Exception as e:
    print('rtsp:',e)
PY

# 3. 导出审计并列出类别/动作
api GET '/api/v1/audit?pageSize=200'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
pairs=sorted({(i['category'],i['action'],i['result']) for i in d.get('items',[])})
print('total',d.get('total'),'pairs',pairs)
PY

# 4. 静态确认 RTSP/私有协议没有审计调用
grep -RniE '\bAudit\(' src/rtsp.cpp src/proto/ || echo "NO AUDIT IN RTSP/PRIVATE PROTOCOL"
```

- **期望**：
  - **当前预期（差距）**：审计中有 `auth/user/system/channel` 等记录，但类别集合中没有
    `media`/`rtsp`/`protocol`；第 4 步输出 `NO AUDIT ...`。
  - **整改后验收**：出现 `media/session_open`、`media/session_close`、`protocol/download_*`
    等记录，含用户、通道、流、来源 IP、会话 ID、字节数；权限拒绝也有审计。
- **证据形式**：审计 JSON 的 category/action 集合、grep 输出、RTSP 会话记录、审计 CSV 导出。

### TC-AUD-02 审计要素完整性与 request_id

- **关联条目**：AUD-02（部分）
- **前置条件**：§2 环境；已 `setup_admin`；至少产生一条审计（登录即可）。
- **步骤**：

```bash
# 1. 带自定义 Request-Id 发起一次写操作
curl -sS -o /dev/null -X PUT -b "${COOKIE}" -H "X-CSRF-Token: ${CSRF}" \
  -H 'X-Request-Id: tc-aud-02-001' -H 'Content-Type: application/json' \
  -d '{"audit":{"retentionDays":180}}' "${BASE}/api/v1/settings"

# 2. 查询审计接口，检查字段与 requestId
api GET '/api/v1/audit?pageSize=5'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
it=d['items'][0]
print('keys=',sorted(it.keys()))
print('requestId_present=', 'requestId' in it, 'value=', it.get('requestId'))
print('sessionId_present=', 'sessionId' in it)
match=[i for i in d['items'] if i.get('action')=='update' and i.get('category')=='system']
print('settings_update=', match[0] if match else None)
PY

# 3. 直接查库确认 request_id 列全为空
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print(db.execute("select count(*), sum(case when request_id is null or request_id='' then 1 else 0 end) from audit_log").fetchone())
print(db.execute("select id, request_id from audit_log order by id desc limit 3").fetchall())
PY
```

- **期望**：
  - **当前预期（差距）**：`requestId_present=False`；SQL 结果为 `(N,N)`，即全部为空；
    接口没有 `sessionId`。
  - **整改后验收**：审计接口返回 `requestId`，且最新一条等于 `tc-aud-02-001`；
    记录含 `sessionId`、`protocol`、`sourcePort`；数据库 `request_id` 非空。
- **证据形式**：接口字段列表、SQL 输出、带 `X-Request-Id` 的请求日志。

### TC-AUD-03 审计日志防篡改

- **关联条目**：AUD-03（缺失）
- **前置条件**：§2 环境；已 `setup_admin`；至少产生 3 条审计。
- **步骤**：

```bash
# 1. 记录当前审计条数与最后一条
api GET '/api/v1/audit?pageSize=200'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
print('total=',d['total'],'last=',d['items'][0] if d['items'] else None)
PY

# 2. 停服后直接修改数据库中的审计记录
stop_server
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1]); db.execute("pragma busy_timeout=5000")
row=db.execute("select max(id) from audit_log").fetchone()[0]
db.execute("update audit_log set actor='tampered', detail='{\"forged\":true}' where id=?",(row,))
db.commit()
print('tampered_id=',row)
PY

# 3. 重启并查询：当前没有任何完整性告警
start_server
login_admin
api GET '/api/v1/audit?pageSize=5'; echo
cat "${BODY}"; echo
grep -iE 'tamper|hash|chain|integrity' "${DATA}/server.log" || echo "NO TAMPER DETECTION"
```

- **期望**：
  - **当前预期（差距）**：重启成功，接口直接返回 `actor=tampered` 的记录；日志中没有告警；
    `audit_log` 表没有 `prev_hash`/`record_hash` 列。
  - **整改后验收**：启动或查询时检测到哈希链断点，返回 `audit_integrity_failed` 并告警；
    导出附带链头签名；`purge` 只能标记删除且需要双人审批；原始篡改可通过外部锚点定位。
- **证据形式**：篡改前后的审计 JSON、`pragma table_info(audit_log)`、服务日志、告警记录。

### TC-AUD-06 审计写入失败的 fail-open 行为

- **关联条目**：AUD-06（缺失）
- **前置条件**：§2 环境；已 `setup_admin`；能停止并重启服务。
- **步骤**：

```bash
# 1. 停服，在 audit_log 上安装一个必然失败的触发器，模拟审计存储不可用
stop_server
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
db.execute("drop trigger if exists fail_audit")
db.execute("create trigger fail_audit before insert on audit_log "
           "begin select raise(abort,'audit unavailable'); end")
db.commit()
print('trigger installed')
PY

# 2. 重启并执行会写审计的操作（登录成功 + 创建用户）
start_server
login_admin
api POST /api/v1/users '{"username":"auditfail1","password":"AuditFail123!","roleId":3}'; echo
cat "${BODY}"; echo

# 3. 查看服务端日志：审计失败只打印 stderr
grep -i 'audit write failed' "${DATA}/server.log" || echo "NO AUDIT FAILURE LOG"
```

- **期望**：
  - **当前预期（差距）**：创建用户返回 `200/201`，响应正常；`server.log` 出现
    `audit write failed: ...`，但操作没有被阻止，也没有告警/重试队列。
  - **整改后验收**：普通操作可继续但产生 `audit_write_failure` 告警并写入本地兜底日志；
    高危操作（用户/权限/删除/格式化/升级）审计失败时返回 `503` 并拒绝执行（fail-closed）；
    审计恢复后可重放兜底队列。
- **证据形式**：状态码、响应体、`server.log` 片段、`alerts` 表、兜底日志文件。

### TC-AUD-07 时钟源与日志外送

- **关联条目**：AUD-07（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 查看 NTP 配置与审计时间来源
api GET /api/v1/settings; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
print('ntp_enabled=', d.get('system',{}).get('ntpEnabled'))
PY
grep -RniE 'syslog|remote.?log|log.?forward|tls.*log' src/ || echo "NO REMOTE LOG FORWARDING"

# 2. 查看审计时间与系统时间的关系
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys,time
db=sqlite3.connect(sys.argv[1])
row=db.execute("select ts from audit_log order by id desc limit 1").fetchone()
print('audit_ts=',row[0] if row else None,'now=',int(time.time()))
PY

# 3. 查看进程日志去向
grep -nE 'StandardOutput|StandardError|journal' packaging/linux/uvnvr.service
```

- **期望**：
  - **当前预期（差距）**：`ntp_enabled=False`；无远程日志实现；审计时间直接取本机 `time()`；
    日志只输出到 journald/stdout，本机被攻陷后可被清除。
  - **整改后验收**：默认启用可信时间源（NTP/NTS 或 GPS）并告警漂移；审计时间带时区/同步状态；
    审计与安全事件通过 TLS syslog/HTTP 外送；外送失败可重试且不阻塞本地审计。
- **证据形式**：设置 JSON、grep 输出、审计 `ts` 与 `date +%s`、journald 配置。

## 6. 数据完整性用例

### TC-INT-01 配置完整性校验

- **关联条目**：INT-01（缺失）
- **前置条件**：§2 环境但**不要**先启动；准备一个包含未知键和篡改安全项的配置文件。
- **步骤**：

```bash
DATA=$(mktemp -d)
cat > "${DATA}/uvnvr.json" <<EOF
{
  "http": {"port": ${PORT}},
  "security": {"admin_allow_cidrs": ["10.0.0.0/8"]},
  "injected": true
}
EOF
"${BIN}" --config "${DATA}/uvnvr.json" --data-dir "${DATA}" --log-level warn \
  >"${DATA}/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -fsS "http://127.0.0.1:${PORT}/healthz" >/dev/null 2>&1 && break; sleep 0.1; done
curl -sS -o /dev/null -w "healthz=%{http_code}\n" "http://127.0.0.1:${PORT}/healthz"
grep -iE 'injected|unknown|signature|integrity' "${DATA}/server.log" || echo "CONFIG ACCEPTED SILENTLY"
grep -n 'Unknown keys are ignored' docs/configuration.md
```

- **期望**：
  - **当前预期（差距）**：`healthz=200`，未知键 `injected` 与未实现的 `security.admin_allow_cidrs`
    被静默忽略；日志无签名/完整性提示。
  - **整改后验收**：安全相关配置必须经 API/签名写入；启动时校验 SM2 签名或哈希锚点；
    未知安全键返回错误并拒绝启动；`--verify-config` 返回可机读结果；配置变更写审计。
- **证据形式**：健康检查状态码、服务日志、配置文件与 `--print-config` 输出、签名校验结果。

### TC-INT-02 录像文件与索引完整性

- **关联条目**：INT-02（部分）
- **前置条件**：§2 环境；已 `setup_admin`；默认数据目录中有 `recordings` 目录。
- **步骤**：

```bash
# 1. 制造一个假录像文件并写入索引（模拟已落盘分段）
mkdir -p "${DATA}/recordings/uvnvr/1/main"
head -c 1880 /dev/urandom > "${DATA}/recordings/uvnvr/1/main/20260101T000000.ts"
SHA=$(sha256sum "${DATA}/recordings/uvnvr/1/main/20260101T000000.ts" | awk '{print $1}')
echo "file_sha256=${SHA}"
python3 - "$DATA/uvnvr.db" "${DATA}/recordings/uvnvr/1/main/20260101T000000.ts" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1]); db.execute("pragma busy_timeout=5000")
db.execute("insert into recordings(channel_id,stream,volume_id,path,start_ms,end_ms,duration_ms,"
           "size_bytes,codec,record_type,created_at) values(1,'main',1,?,?,?,?,?,'h264','timed',?)",
           (sys.argv[2],1767225600000,1767225660000,60000,1880,int(__import__('time').time())))
db.commit()
print('row inserted')
PY

# 2. 查询接口是否返回摘要/完整性字段
api GET '/api/v1/recordings?channelId=1'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
print(json.dumps(d, ensure_ascii=False)[:800])
print('has_sha256=', 'sha256' in json.dumps(d).lower() or 'digest' in json.dumps(d).lower())
PY
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print([c[1] for c in db.execute("pragma table_info(recordings)")])
PY

# 3. 篡改文件后再次查询：没有校验，仍返回该段
printf 'TAMPERED' | dd of="${DATA}/recordings/uvnvr/1/main/20260101T000000.ts" bs=1 seek=0 conv=notrunc status=none
api GET '/api/v1/recordings?channelId=1'; echo
grep -iE 'hash|digest|corrupt|integrity' "${DATA}/server.log" || echo "NO FILE INTEGRITY CHECK"
```

- **期望**：
  - **当前预期（差距）**：索引表没有摘要列；查询返回该段；文件被改写后查询仍成功，
    日志无任何完整性告警。
  - **整改后验收**：`recordings`/新清单表存每段 SM3；播放/导出前校验，篡改段返回
    `integrity_failed` 并告警/隔离；启动扫描孤立 `.part` 与索引不一致并给出恢复报告。
- **证据形式**：文件 SHA-256、表结构、查询 JSON、篡改前后响应、服务日志。

### TC-INT-03 数据库完整性自检

- **关联条目**：INT-03（缺失）
- **前置条件**：§2 环境；已 `setup_admin`；可停服。
- **步骤**：

```bash
# 1. 静态确认启动路径没有 integrity_check/quick_check
grep -RniE 'integrity_check|quick_check' src/ || echo "NO DB INTEGRITY CHECK IN STARTUP PATH"

# 2. 手工执行一次 quick_check，作为对照
stop_server
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print('quick_check=', db.execute('pragma quick_check').fetchall())
PY

# 3. 重启并观察启动日志：没有完整性结果
start_server
grep -iE 'integrity|quick_check|corrupt' "${DATA}/server.log" || echo "NO INTEGRITY LOG AT STARTUP"
```

- **期望**：
  - **当前预期（差距）**：第 1 步 `NO DB INTEGRITY CHECK...`；手工检查正常，但启动日志没有结果；
    数据库损坏只会在具体查询时报错，且无内置备份/恢复。
  - **整改后验收**：启动执行 `quick_check` 并记录结果；失败时拒绝写入并告警；
    每日 `integrity_check`；提供带 SM3 清单的在线备份/恢复 API（维护模式 + 管理员）。
- **证据形式**：grep 输出、`pragma quick_check` 结果、启动日志、备份/恢复记录。

### TC-INT-04 传输帧完整性（CRC32 可伪造）

- **关联条目**：INT-04（部分）
- **前置条件**：§2 环境；服务使用 `start_server --private-port 15151 --private-tcp` 启动。
- **步骤**：

```bash
python3 - 15151 <<'PY'
import socket,struct,zlib,sys
port=int(sys.argv[1])
def frame(op,payload,flip_crc=False):
    hdr=bytearray(32)
    struct.pack_into('<I',hdr,0,0x524E5655)  # magic
    hdr[4]=1; hdr[5]=32                      # version, header_len
    struct.pack_into('<H',hdr,6,0)           # flags
    struct.pack_into('<H',hdr,8,op)          # opcode
    struct.pack_into('<Q',hdr,12,0)          # session_id
    struct.pack_into('<I',hdr,20,1)          # request_id
    struct.pack_into('<I',hdr,24,len(payload))
    crc=zlib.crc32(bytes(hdr[:28])+b'\0\0\0\0'+payload)&0xffffffff
    if flip_crc: crc ^= 0xff
    struct.pack_into('<I',hdr,28,crc)
    return bytes(hdr)+payload
def send(data):
    s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
    s.sendall(data)
    try: r=s.recv(4096)
    except Exception: r=b''
    s.close(); return r
print('valid_hello_reply_bytes=', len(send(frame(1,b'{}'))))
print('bad_crc_reply_bytes=', len(send(frame(1,b'{}',flip_crc=True))))
print('forged_payload_reply_bytes=', len(send(frame(1,b'{"forged":true}'))))
PY
```

- **期望**：
  - **当前预期（差距）**：`valid` 与 `forged` 都收到 HELLO_ACK（字节数 > 0），
    `bad_crc` 为 0/连接关闭。证明 CRC 只能发现误码，攻击者可重算 CRC 后伪造帧。
  - **整改后验收**：控制帧使用会话密钥的 HMAC-SM3/AEAD；`forged` 被拒绝并断开；
    RTP 使用 SRTP 或国密媒体完整性；拒绝事件写审计。
- **证据形式**：三种帧的响应字节数、抓包（可选）、协议文档 §2.2、审计拒绝记录。

### TC-INT-05 录像/导出水印与来源证明

- **关联条目**：INT-05（缺失）
- **前置条件**：仓库源码可用；无需运行服务（静态证据为主，可选运行导出）。
- **步骤**：

```bash
# 1. 录像/导出路径没有烧录水印
grep -RniE 'watermark|drawtext|overlay' src/recording.cpp src/media.cpp src/exporter.cpp \
  || echo "NO BURN-IN WATERMARK IN RECORDING/EXPORT PATH"

# 2. 水印仅存在于前端展示与通道配置
grep -n 'live-watermark' frontend/src/components/LiveTile.vue
grep -n '"watermark"' src/app.cpp | head

# 3. 导出没有清单/签名
grep -RniE 'manifest|sm3|sm2|signature|sign\(' src/exporter.cpp src/playback.cpp \
  || echo "NO EXPORT MANIFEST OR SIGNATURE"
```

- **期望**：
  - **当前预期（差距）**：第 1、3 步输出 `NO ...`；水印只是前端 CSS 文本（`LiveTile.vue:183`）
    和通道 JSON，不进入录像/导出文件。
  - **整改后验收**：可选烧录设备序列号/通道/时间水印；导出时生成 SM3 清单并用设备 SM2 证书签名，
    产出 `.manifest.json`；平台可离线验签并检测剪辑。
- **证据形式**：grep 输出、前端水印代码位置、导出文件与清单样例、验签结果。

## 7. 数据保密性用例

### TC-CON-01 RTSP HA1 的离线可破解性

- **关联条目**：CON-01（部分）
- **前置条件**：§2 环境；已 `setup_admin` 并至少成功登录一次（写入 `rtsp_ha1`）。
- **步骤**：

```bash
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys,hashlib
db=sqlite3.connect(sys.argv[1]); db.row_factory=sqlite3.Row
row=db.execute("select username, rtsp_ha1 from users where username='admin'").fetchone()
print('stored_ha1=', row['rtsp_ha1'])
print('md5_expected=', hashlib.md5(b'admin:UvNVR:Admin123!@#').hexdigest())
print('equal=', row['rtsp_ha1'] == hashlib.md5(b'admin:UvNVR:Admin123!@#').hexdigest())
print('length=', len(row['rtsp_ha1']))
PY
# 静态确认算法
grep -n 'RtspHa1' -A3 src/app.cpp | head -8
```

- **期望**：
  - **当前预期（差距）**：`equal=True`、`length=32`，即数据库保存的是无盐
    `MD5(username:UvNVR:password)`，可直接离线字典攻击。
  - **整改后验收**：数据库中的 RTSP 校验值不是口令的 MD5；使用 scrypt/Argon2 派生或
    SRP/HTTP Digest-SHA-256；旧值在登录后自动迁移；Basic 默认关闭或仅 TLS 下允许。
- **证据形式**：SQL 输出、MD5 对照、`RtspHa1` 源码位置、迁移前后数据库快照。

### TC-CON-03 密钥存储、权限与轮换

- **关联条目**：CON-03（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 密钥与数据库同目录、32 字节、创建时 0600
ls -l "${DATA}/secret.key" "${DATA}/uvnvr.db"
stat -c '%a %U:%G %n' "${DATA}/secret.key"
wc -c < "${DATA}/secret.key"

# 2. 没有轮换/版本/keystore 接口
grep -RniE 'key_id|keyVersion|rotate_key|rotateKey|tpm2|DPAPI|Tss2' src/ \
  || echo "NO KEY ROTATION/KEYSTORE"
api GET /api/v1/settings; echo

# 3. 把权限改成 0644 后重启：加载路径是否校正？
stop_server
chmod 0644 "${DATA}/secret.key"
start_server
login_admin
stat -c '%a %n' "${DATA}/secret.key"
```

- **期望**：
  - **当前预期（差距）**：初始 `600`；第 2 步无轮换/keystore；第 3 步重启后仍为 `644`，
    说明只在创建时设置权限，加载时不校正。密钥与数据库同目录，复制数据目录即可解密相机口令。
  - **整改后验收**：加载时强制 `0600` 且校验属主，否则拒绝启动；密钥带 `key_id` 支持轮换与重加密；
    优先使用 TPM/DPAPI/OS keystore；密钥与数据分离；密钥使用与轮换写审计。
- **证据形式**：`stat` 输出、`wc -c`、grep 输出、重启前后权限、密钥使用审计。

### TC-CON-04 录像/抓图/数据库/备份的静态加密

- **关联条目**：CON-04（缺失）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 制造一个明文 JPEG 抓图并写入 alerts 索引，模拟告警抓图落盘
mkdir -p "${DATA}/alerts"
printf '\xff\xd8\xff\xe0JFIF-UvNVR-PLAINTEXT\xff\xd9' > "${DATA}/alerts/cam1.jpg"
file "${DATA}/alerts/cam1.jpg"
python3 - "$DATA/uvnvr.db" "${DATA}/alerts/cam1.jpg" <<'PY'
import sqlite3,sys,time
db=sqlite3.connect(sys.argv[1]); db.execute("pragma busy_timeout=5000")
cur=db.execute("insert into alerts(channel_id,type,level,message,snapshot_path,acknowledged,created_at) "
               "values(1,'motion','warning','test',?,0,?)",(sys.argv[2],int(time.time())))
db.commit()
open('/tmp/tc-con04-alert-id','w').write(str(cur.lastrowid))
print('alert_row_id=',cur.lastrowid)
PY
ALERT_ID=$(cat /tmp/tc-con04-alert-id)
curl -sS -b "${COOKIE}" -o /tmp/tc-con04-snap.jpg -w "snapshot_api=%{http_code}\n" \
  "${BASE}/api/v1/alerts/${ALERT_ID}/snapshot"
file /tmp/tc-con04-snap.jpg

# 2. 数据库明文：直接 strings 可见用户名/审计
strings "${DATA}/uvnvr.db" | grep -E 'admin|audit_login' | head -5
file "${DATA}/uvnvr.db"

# 3. 录像写入权限与明文（静态证据）
grep -n 'O_WRONLY | O_CREAT | O_TRUNC' src/recording.cpp
grep -RniE 'sm4|aes.*record|encrypt.*record|gcm.*record' src/recording.cpp src/exporter.cpp \
  || echo "NO RECORDING/EXPORT ENCRYPTION"
```

- **期望**：
  - **当前预期（差距）**：`snapshot_api=200`，返回的与磁盘上的都是可识别 JPEG；数据库 `strings`
    可读；录像以 `0644` 明文写入；录像/导出路径无加密。
  - **整改后验收**：录像、抓图、导出默认信封加密（SM4-GCM）；数据库敏感列列级加密；
    备份默认加密并带 SM3 清单；只有持有数据密钥的授权路径能解密，磁盘上无明文可识别内容。
- **证据形式**：`file`/`strings` 输出、快照 API 状态码、`recording.cpp` 打开标志、加密配置与密钥层级说明。

### TC-CON-05 人脸/车牌个人信息保护

- **关联条目**：CON-05（部分）
- **前置条件**：§2 环境（`UVNVR_ENABLE_AI=ON`）；已 `setup_admin`；创建一个 viewer 账户。
- **步骤**：

```bash
# 1. 写入一条人脸事件：原始特征 BLOB + 明文全图/裁剪图
mkdir -p "${DATA}/ai/faces"
printf '\xff\xd8\xff\xe0FACE-IMAGE\xff\xd9' > "${DATA}/ai/faces/1-full.jpg"
printf '\xff\xd8\xff\xe0FACE-CROP\xff\xd9' > "${DATA}/ai/faces/1-crop.jpg"
python3 - "$DATA/uvnvr.db" "${DATA}/ai/faces/1-full.jpg" "${DATA}/ai/faces/1-crop.jpg" <<'PY'
import sqlite3,sys,time
db=sqlite3.connect(sys.argv[1]); db.execute("pragma busy_timeout=5000")
blob=bytes(range(256))*4  # 模拟 1024 字节原始特征
cur=db.execute("insert into ai_face_events(channel_id,ts,quality,confidence,x,y,w,h,image_path,"
               "crop_path,feature_blob,engine,created_at) values(1,?,0.9,0.9,1,1,10,10,?,?,?,'test',?)",
               (int(time.time()),sys.argv[2],sys.argv[3],blob,int(time.time())))
db.commit()
open('/tmp/tc-con05-event-id','w').write(str(cur.lastrowid))
print('face_event_id=',cur.lastrowid)
PY

# 2. viewer 登录后读取事件与图片
api POST /api/v1/users '{"username":"viewer1","password":"Viewer123!","roleId":3}'; echo
COOKIE5=$(mktemp)
curl -sS -c "${COOKIE5}" -o /dev/null -H 'Content-Type: application/json' \
  -d '{"username":"viewer1","password":"Viewer123!"}' "${BASE}/api/v1/auth/login"
curl -sS -b "${COOKIE5}" "${BASE}/api/v1/ai/events?type=face&limit=10" -o /tmp/tc-con05-events
cat /tmp/tc-con05-events; echo
EV_ID=$(cat /tmp/tc-con05-event-id)
curl -sS -b "${COOKIE5}" -o /tmp/tc-con05-snap.jpg -w "viewer_face_image=%{http_code}\n" \
  "${BASE}/api/v1/ai/events/${EV_ID}/snapshot?type=face&kind=full"
file /tmp/tc-con05-snap.jpg

# 3. 检查特征是否原样存储
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print(db.execute("select length(feature_blob), hex(substr(feature_blob,1,8)) from ai_face_events").fetchall())
PY
rm -f "${COOKIE5}"
```

- **期望**：
  - **当前预期（差距）**：viewer（仅有 `alert.view`）可读取人脸事件和图片，`viewer_face_image=200`；
    特征 BLOB 原样存储（长度/十六进制可见）；没有 `ai.pii.view`/`ai.pii.export` 权限。
  - **整改后验收**：读取/导出人脸图片与特征需要独立 PII 权限；特征模板加密存储且可删除；
    抓图使用短期签名 URL；导出需审批与脱敏；保留期到期自动清理并留审计。
- **证据形式**：viewer 权限集合、事件 JSON、图片状态码与 `file` 输出、特征 BLOB 长度、权限/审计记录。

## 8. 通信安全用例

### TC-COM-01 管理面 HTTPS 默认与 HSTS

- **关联条目**：COM-01（部分）
- **前置条件**：§2 环境；可停服重启。
- **步骤**：

```bash
# 1. 默认启动（无 TLS）
start_server
curl -sS -o /dev/null -w "http_default=%{http_code}\n" "${BASE}/healthz"
curl -sS -o /dev/null -w "https_default=%{http_code}\n" -k "https://127.0.0.1:${PORT}/healthz" || true
curl -sS -D - -o /dev/null "${BASE}/healthz" | grep -iE 'strict-transport-security' \
  || echo "NO HSTS (plain HTTP)"

# 2. 显式启用自签 TLS 后访问
stop_server
start_server --tls-self-signed
curl -sS -o /dev/null -w "https_selfsigned=%{http_code}\n" -k "https://127.0.0.1:${PORT}/healthz"
curl -sS -D - -o /dev/null -k "https://127.0.0.1:${PORT}/healthz" \
  | grep -iE 'strict-transport-security' || echo "NO HSTS (TLS)"
```

- **期望**：
  - **当前预期（差距）**：默认 `http_default=200`，`https_default` 失败；启用 TLS 后
    `https_selfsigned=200`，但无 HSTS，HTTP/HTTPS 由配置决定，无最低 TLS 版本/套件配置。
  - **整改后验收**：生产模式默认 HTTPS 或 HTTP 仅回环/重定向；HSTS 含 `max-age` 与子域；
    可配置 `tls.minVersion`/`tls.ciphers`；证书过期告警；自签仅允许显式开发模式。
- **证据形式**：四个状态码、响应头、配置 `system/config`、TLS 版本/套件扫描结果。

### TC-COM-02 国密 TLS 套件

- **关联条目**：COM-02（缺失）
- **前置条件**：构建机与目标机具备 OpenSSL；可选安装 Tongsuo/GmSSL。
- **步骤**：

```bash
openssl version
echo "--- SM ciphers offered by this OpenSSL ---"
openssl ciphers -v 'ECDHE-SM4-CBC-SM3' 2>&1 | head -5
openssl ciphers 'ALL:eNULL' | tr ':' '\n' | grep -iE 'SM4|SM3|SM2' || echo "NO GM CIPHER SUITE"
echo "--- build switches / backend ---"
grep -RniE 'GM_TLS|Tongsuo|GmSSL|ECDHE_SM4' CMakeLists.txt src/ || echo "NO GM TLS BACKEND"
```

- **期望**：
  - **当前预期（差距）**：OpenSSL 3.0.13 不提供 `ECDHE-SM4-CBC-SM3`；grep 无 GM 后端/开关。
  - **整改后验收**：存在 `UVNVR_ENABLE_GM_TLS`（默认 OFF）；启用后 TLS 握手协商
    `ECDHE_SM4_CBC_SM3` 等国密套件；双证书（签名/加密）可用；与支持 GB 35114 的平台互通；
    证书链与套件在握手中可见。
- **证据形式**：`openssl version`、套件列表、CMake 开关、抓包/握手输出、互通报告。

### TC-COM-03 视频与控制通道加密

- **关联条目**：COM-03（部分）
- **前置条件**：§2 环境；`start_server --rtsp-port 18554 --private-port 15151 --private-tcp`；
  已 `setup_admin`。
- **步骤**：

```bash
# 1. RTSP 端口是明文 RTSP：未认证请求直接收到可读的 401 挑战
python3 - 18554 <<'PY'
import socket,sys
port=int(sys.argv[1])
s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
s.sendall(b'OPTIONS rtsp://127.0.0.1:%d/live/1/main RTSP/1.0\r\nCSeq: 1\r\n\r\n'%port)
data=s.recv(512); print(data.decode('latin1').split('\r\n')[0]); s.close()
PY

# 2. RTSP 端口不接受 TLS
timeout 3 openssl s_client -connect 127.0.0.1:18554 -brief </dev/null 2>&1 | head -3 || true

# 3. 私有协议 TCP 为明文：HELLO/HELLO_ACK 可直接读取 JSON
python3 - 15151 <<'PY'
import socket,struct,zlib,sys
port=int(sys.argv[1]); payload=b'{}'
hdr=bytearray(32); struct.pack_into('<I',hdr,0,0x524E5655); hdr[4]=1; hdr[5]=32
struct.pack_into('<H',hdr,8,1); struct.pack_into('<I',hdr,20,1)
struct.pack_into('<I',hdr,24,len(payload))
struct.pack_into('<I',hdr,28,zlib.crc32(bytes(hdr[:28])+b'\0\0\0\0'+payload)&0xffffffff)
s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
s.sendall(bytes(hdr)+payload)
print(s.recv(512)[:160]); s.close()
PY
```

- **期望**：
  - **当前预期（差距）**：RTSP 返回可读的 `RTSP/1.0 401 Unauthorized`；TLS 握手失败；
    TCP 私有协议响应中包含可读 JSON（`HELLO_ACK`），说明控制通道明文。
  - **整改后验收**：RTSP 提供 RTSPS/SRTP（可强制）；私有协议默认 QUIC 或 TCP+会话加密；
    抓包看不到可读 RTSP/JSON；明文模式只在显式“可信网络”开关下可用并告警。
- **证据形式**：RTSP 首行、TLS 握手输出、HELLO_ACK 十六进制、抓包文件、配置开关。

### TC-COM-04 设备接入认证强度（RTSP Basic / ONVIF 明文）

- **关联条目**：COM-04（部分）
- **前置条件**：§2 环境；`start_server --rtsp-port 18554`；已 `setup_admin`。
- **步骤**：

```bash
# 1. RTSP 在明文通道接受 Basic（口令仅 Base64）
python3 - 18554 <<'PY'
import base64,socket,sys
port=int(sys.argv[1]); cred=base64.b64encode(b'admin:Admin123!@#').decode()
req=(f'OPTIONS rtsp://127.0.0.1:{port}/live/1/main RTSP/1.0\r\n'
     f'CSeq: 1\r\nAuthorization: Basic {cred}\r\n\r\n')
s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
s.sendall(req.encode()); print(s.recv(512).decode('latin1').split('\r\n')[0]); s.close()
PY

# 2. 401 挑战同时提供 Digest-MD5 与 Basic
python3 - 18554 <<'PY'
import socket,sys
port=int(sys.argv[1])
s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
s.sendall(f'OPTIONS rtsp://127.0.0.1:{port}/live/1/main RTSP/1.0\r\nCSeq: 1\r\n\r\n'.encode())
print(s.recv(1024).decode('latin1')); s.close()
PY

# 3. ONVIF/brand 接入只支持明文 HTTP
grep -n 'only http:// ONVIF endpoints are supported' src/onvif.cpp
grep -n 'int HttpRequest' src/brand_http.cpp
grep -RniE 'SSL_CTX|https://' src/brand_http.cpp src/onvif.cpp || echo "NO TLS IN CAMERA ACCESS"
```

- **期望**：
  - **当前预期（差距）**：Basic 请求返回 `200`；401 同时给出 Digest 与 Basic；
    ONVIF 代码明确拒绝非 `http://`；品牌 HTTP 无 TLS。
  - **整改后验收**：Basic 默认禁用（或仅 TLS 下允许）；RTSP 使用 Digest-SHA-256 或 RTSPS；
    相机接入优先 HTTPS/ONVIF over TLS 并校验证书；失败计数、来源限制与审计。
- **证据形式**：Basic 响应首行、401 挑战全文、grep 输出、相机连接抓包。

### TC-COM-05 抗重放（RTSP nonce 复用）

- **关联条目**：COM-05（部分）
- **前置条件**：§2 环境；`start_server --rtsp-port 18554`；已 `setup_admin`。
- **步骤**：

```bash
python3 - 18554 <<'PY'
import hashlib,re,socket,sys
port=int(sys.argv[1]); url=f'rtsp://127.0.0.1:{port}/live/1/main'
def raw(req):
    s=socket.create_connection(('127.0.0.1',port),3); s.settimeout(2)
    s.sendall(req.encode()); data=s.recv(2048).decode('latin1'); s.close(); return data
# 1. 取挑战 nonce
challenge=raw(f'OPTIONS {url} RTSP/1.0\r\nCSeq: 1\r\n\r\n')
print(challenge.split('\r\n')[0])
nonce=re.search(r'nonce="([^"]+)"',challenge).group(1)
print('nonce=',nonce)
# 2. 计算一次 Digest，并原样重放两次
ha1=hashlib.md5(b'admin:UvNVR:Admin123!@#').hexdigest()
ha2=hashlib.md5(f'OPTIONS:{url}'.encode()).hexdigest()
resp=hashlib.md5(f'{ha1}:{nonce}:{ha2}'.encode()).hexdigest()
auth=(f'Digest username="admin", realm="UvNVR", nonce="{nonce}", '
      f'uri="{url}", response="{resp}"')
for cseq in (2,3):
    r=raw(f'OPTIONS {url} RTSP/1.0\r\nCSeq: {cseq}\r\nAuthorization: {auth}\r\n\r\n')
    print('replay',cseq,r.split('\r\n')[0])
PY
```

- **期望**：
  - **当前预期（差距）**：两次重放都返回 `RTSP/1.0 200 OK`；nonce 在进程生命周期内固定，
    不校验 `nc`/时间窗。
  - **整改后验收**：第一次重放成功，第二次返回 `401`/`403`（nonce 一次性或时间窗+`nc` 单调）；
    私有协议请求带单调序号/时间戳/HMAC，重放被拒绝并写审计。
- **证据形式**：挑战、nonce、两次重放响应首行、私有协议重放测试输出、审计拒绝记录。

### TC-COM-06 证书校验与双向身份

- **关联条目**：COM-06（部分）
- **前置条件**：§2 环境；可停服重启。
- **步骤**：

```bash
# 1. 自签 HTTPS：未配置信任库的客户端加 -k 才能通过
stop_server
start_server --tls-self-signed
timeout 5 openssl s_client -connect 127.0.0.1:${PORT} -servername localhost </dev/null \
  2>/dev/null | openssl x509 -noout -subject -issuer
curl -sS -o /dev/null -w "ssl_verify_without_k=%{http_code}\n" \
  "https://127.0.0.1:${PORT}/healthz" 2>/dev/null || echo "ssl_verify_without_k=failed"
curl -sS -k -o /dev/null -w "ssl_with_k=%{http_code}\n" "https://127.0.0.1:${PORT}/healthz"

# 2. 服务端是否要求客户端证书
timeout 5 openssl s_client -connect 127.0.0.1:${PORT} -servername localhost </dev/null 2>&1 \
  | grep -iE 'client certificate|No client certificate CA names' || echo "NO CLIENT CERT REQUEST"

# 3. 私有 QUIC 默认自签；相机接入无 TLS
grep -n 'generate_self_signed' src/proto/proto_server.cpp
grep -n 'SSL_get_verify_result' src/app.cpp
grep -n 'only http:// ONVIF endpoints are supported' src/onvif.cpp
```

- **期望**：
  - **当前预期（差距）**：证书为 `CN=localhost` 自签；不带 `-k` 校验失败，带 `-k` 通过；
    服务端不要求客户端证书；QUIC 默认自签；相机接入无 TLS（SMTP 是唯一做证书校验的客户端）。
  - **整改后验收**：私有协议支持 CA 签发证书、证书固定与可选 mTLS；QUIC 客户端校验证书；
    GB 35114 使用设备证书双向认证；相机接入支持 HTTPS 并校验证书；证书过期/吊销告警。
- **证据形式**：`openssl x509` 输出、两次 curl 状态码、客户端证书请求片段、grep 输出、证书链配置。

## 9. 软件容错用例

### TC-FLT-01 异常输入与资源上限

- **关联条目**：FLT-01（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 非法 JSON
curl -sS -o /dev/null -w "bad_json=%{http_code}\n" -b "${COOKIE}" \
  -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
  --data-binary '{bad' "${BASE}/api/v1/settings"

# 2. 超大请求体（框架默认 16 MiB，超过应 413）
head -c 17825792 /dev/zero | curl -sS -o /dev/null -w "oversize=%{http_code}\n" \
  -b "${COOKIE}" -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
  --data-binary @- "${BASE}/api/v1/settings"

# 3. 非法 ID / 类型
curl -sS -o /dev/null -w "bad_id=%{http_code}\n" -b "${COOKIE}" \
  "${BASE}/api/v1/channels/999999999999999999999/snapshot"
curl -sS -o /dev/null -w "bad_type=%{http_code}\n" -b "${COOKIE}" \
  -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
  -d '{"security":{"webMaxFailures":"abc"}}' "${BASE}/api/v1/settings"

# 4. 服务仍健康
curl -sS -o /dev/null -w "healthy_after=%{http_code}\n" "${BASE}/healthz"

# 5. Windows 命令拼接的静态风险
grep -n 'CreateProcessA' src/media.cpp | head -3
grep -n 'command =' src/media.cpp | head -3
```

- **期望**：
  - **当前预期（差距）**：`bad_json=400`、`oversize=413`、`bad_id=400`、`bad_type=400`，
    `healthy_after=200`，基本健壮；但无统一 schema，Windows 侧用拼接命令字符串构造 ffmpeg
    （`src/media.cpp:1601-1617`），需要更严格的 URL/参数校验。
  - **整改后验收**：所有写接口有统一 schema 与字段上限；超大/畸形请求返回稳定错误码；
    Windows 使用参数数组或严格转义；模糊测试纳入 CI；服务在异常输入后保持健康。
- **证据形式**：四个状态码、健康检查、`server.log`、模糊测试报告、Windows 参数构造代码审查。

### TC-FLT-04 进程/媒体故障恢复

- **关联条目**：FLT-04（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 无服务管理器时强杀进程，观察是否自愈
start_server
kill -9 "${PID}"; wait "${PID}" 2>/dev/null || true; PID=""
sleep 6
curl -sS --max-time 2 -o /dev/null -w "after_kill=%{http_code}\n" "${BASE}/healthz" \
  || echo "after_kill=connection_refused"

# 2. 随包服务单元提供外部拉起与最小权限
grep -nE 'Restart=|NoNewPrivileges|AmbientCapabilities|CapabilityBoundingSet' \
  packaging/linux/uvnvr.service

# 3. 媒体子进程有退避重连
grep -n 'kDelays' src/media.cpp
grep -n 'PR_SET_PDEATHSIG' src/media.cpp

# 4. 监听失败只告警不退出（静态证据）
grep -n 'RTSP server unavailable' -A3 src/app.cpp | head -8
```

- **期望**：
  - **当前预期（差距）**：强杀后 6 秒健康检查失败，说明应用自身没有 watchdog；
    systemd 单元有 `Restart=on-failure`，但非 systemd 运行方式无自动拉起；
    RTSP/HTTP 监听失败只告警不退出。
  - **整改后验收**：关键监听失败按配置退出以便服务管理器重启；提供可选内建看护；
    记录重启次数/原因；Windows 服务恢复策略显式配置；媒体重连有上限与告警。
- **证据形式**：强杀前后状态码、systemd 单元片段、媒体退避代码、重启/告警记录。

### TC-FLT-05 崩溃/断电后的 `.part` 恢复

- **关联条目**：FLT-05（部分）
- **前置条件**：§2 环境；已 `setup_admin`；可停服。
- **步骤**：

```bash
# 1. 制造一个未完成的录像分段（模拟断电）
mkdir -p "${DATA}/recordings/uvnvr/1/main"
head -c 3760 /dev/urandom > "${DATA}/recordings/uvnvr/1/main/20260101T000000.ts.part"
ls -l "${DATA}/recordings/uvnvr/1/main/"

# 2. 重启：当前不扫描孤儿 .part
stop_server
start_server
sleep 1
ls -l "${DATA}/recordings/uvnvr/1/main/"*.part
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print('recordings_rows=',db.execute('select count(*) from recordings').fetchone()[0])
PY
grep -iE 'orphan|recover|part' "${DATA}/server.log" || echo "NO STARTUP RECOVERY"

# 3. 静态确认启动路径没有恢复逻辑
grep -n 'part_path' src/recording.cpp | head -10
```

- **期望**：
  - **当前预期（差距）**：重启后 `.part` 仍存在，`recordings_rows=0`，日志无恢复行为；
    启动不扫描孤儿分段，索引与文件可能长期不一致。
  - **整改后验收**：启动扫描 `.part`，可解析则补齐索引，不可解析则隔离并告警；
    文件与索引不一致可修复/标记；关键元数据使用更强同步策略；产出恢复报告。
- **证据形式**：重启前后文件列表、索引行数、恢复日志、恢复报告。

### TC-FLT-06 安全事件告警

- **关联条目**：FLT-06（部分）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 先创建 viewer 并触发权限拒绝（避免后面的全局锁定影响登录）
api POST /api/v1/users '{"username":"viewer2","password":"Viewer123!","roleId":3}'; echo
COOKIE6=$(mktemp)
curl -sS -c "${COOKIE6}" -o /dev/null -H 'Content-Type: application/json' \
  -d '{"username":"viewer2","password":"Viewer123!"}' "${BASE}/api/v1/auth/login"
curl -sS -b "${COOKIE6}" -o /dev/null -w "forbidden=%{http_code}\n" "${BASE}/api/v1/users"
rm -f "${COOKIE6}"

# 2. 再触发登录锁定（全局锁只影响新的登录，不影响已有 admin 会话）
api PUT /api/v1/settings '{"security":{"webMaxFailures":2,"webLockMinutes":1}}'; echo
for i in 1 2; do
  curl -sS -o /dev/null -H 'Content-Type: application/json' \
    -d '{"username":"ghost","password":"WrongPass1!"}' "${BASE}/api/v1/auth/login"
done

# 3. 查询告警：是否出现 auth_lockout / permission_denied
api GET '/api/v1/alerts'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
items=d.get('items',[]) if isinstance(d,dict) else d
if not isinstance(items,list): items=[]
types=sorted({i.get('type') for i in items if isinstance(i,dict)})
print('alert_types=',types)
PY
```

- **期望**：
  - **当前预期（差距）**：`forbidden=403`，但 `alert_types` 中没有 `auth_lockout`/
    `permission_denied`；安全事件不会进入告警/邮件/Webhook。
  - **整改后验收**：锁定、权限拒绝、审计写失败、证书过期、时钟漂移均产生告警；
    高频事件聚合限流；告警含来源、账户、时间与处置建议；可邮件/Webhook 外发。
- **证据形式**：状态码、告警列表 JSON、告警外发记录、审计。

## 10. 升级安全用例

### TC-UPG-01 升级包签名验证

- **关联条目**：UPG-01（缺失）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 不存在升级接口
api POST /api/v1/system/upgrade '{}'; echo
cat "${BODY}"; echo

# 2. 代码中没有固件/升级包验签
grep -RniE 'firmware.*(verify|signature)|upgrade_package|verify_signature|sm2_verify|EVP_PKEY_verify' src/ \
  || echo "NO UPGRADE/SIGNATURE VERIFICATION"

# 3. 检查安装包是否签名
grep -RniE 'signtool|codesign|gpg|minisign' packaging/ || echo "NO PACKAGE SIGNING STEP"
```

- **期望**：
  - **当前预期（差距）**：升级接口 `404`；grep 无升级验签实现；安装包脚本无签名步骤；
    Windows/macOS 包未签名（`docs/deployment.md` 已说明）。
  - **整改后验收**：定义升级包格式（版本/型号/SM3/SM2 签名/证书链）；设备内置升级公钥；
    验签、型号、版本与哈希全部通过才允许安装；提供离线验签工具与失败审计。
- **证据形式**：404 响应、grep 输出、升级包格式说明、验签成功/失败样例、审计。

### TC-UPG-02 防回滚

- **关联条目**：UPG-02（缺失）
- **前置条件**：§2 环境；`start_server --maintenance`；已 `setup_admin`。
- **步骤**：

```bash
# 1. 将显示版本升到 9.9.9，再降到 0.0.1：两次都被接受
api PUT /api/v1/system/version '{"version":"9.9.9"}'; echo
cat "${BODY}"; echo
api PUT /api/v1/system/version '{"version":"0.0.1"}'; echo
cat "${BODY}"; echo

# 2. 没有最低安全版本/回滚计数器
grep -RniE 'min_security_version|rollback_counter|anti.?rollback|downgrade' src/ \
  || echo "NO ANTI-ROLLBACK CONTROL"
python3 - "$DATA/uvnvr.db" <<'PY'
import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
print(db.execute("select key,value from system_meta").fetchall())
PY
```

- **期望**：
  - **当前预期（差距）**：两次 PUT 都返回 `200`，版本可从 `9.9.9` 降到 `0.0.1`；
    只有 `version_override`，没有最低安全版本/计数器。
  - **整改后验收**：安全存储保存单调递增的最低版本/回滚计数器；低于该值的包被拒绝；
    受控降级需维护模式 + 双人审批并写审计；版本显示接口不再影响真实升级判断。
- **证据形式**：两次响应体、`system_meta` 内容、grep 输出、降级拒绝审计。

### TC-UPG-03 升级原子性与失败回滚

- **关联条目**：UPG-03（缺失）
- **前置条件**：源码与打包脚本可用；无需运行服务。
- **步骤**：

```bash
# 1. 没有 A/B 槽、快照或回滚实现
grep -RniE 'boot_slot|ab_partition|upgrade.*rollback|rollback.*upgrade|previous_version|snapshot.*upgrade' \
  src/ packaging/ || echo "NO ATOMIC UPGRADE / ROLLBACK"

# 2. 安装脚本原地停止并替换
sed -n '1,80p' packaging/linux/postinst
sed -n '1,40p' packaging/linux/prerm
grep -n 'stop\|replace\|install' packaging/linux/build-package.sh | head -20
```

- **期望**：
  - **当前预期（差距）**：无 A/B/快照/回滚；`prerm`/`postinst` 停止服务后原地替换；
    升级中断只能人工重装。
  - **整改后验收**：新版本写入备用槽，健康检查通过后切换启动指针；保留上一版本与配置/数据库备份；
    失败自动回滚并告警；升级状态可查询。
- **证据形式**：grep 输出、安装脚本片段、A/B 目录结构、失败注入回滚演示、恢复日志。

### TC-UPG-04 升级权限与审计

- **关联条目**：UPG-04（缺失）
- **前置条件**：§2 环境；已 `setup_admin`。
- **步骤**：

```bash
# 1. 权限表中没有升级权限
grep -n 'system.upgrade' src/app.cpp || echo "NO UPGRADE PERMISSION"

# 2. 审计中没有升级动作
api GET '/api/v1/audit?pageSize=200'; echo
python3 - <<'PY'
import json
d=json.load(open('/tmp/uvnvr-case-body.json'))
acts=sorted({i['action'] for i in d.get('items',[])})
print('actions=',acts)
print('has_upgrade=', any('upgrade' in a for a in acts))
PY

# 3. 唯一“版本”写接口只改显示值，且审计为 system/version update
api PUT /api/v1/system/version '{"version":"1.2.3"}'; echo
```

- **期望**：
  - **当前预期（差距）**：`AllPermissions` 没有 `system.upgrade`；审计动作中没有 upgrade；
    版本写接口只改显示值。
  - **整改后验收**：新增 `system.upgrade`（默认仅 admin）；升级前再次认证；
    审计记录包名、签名主体、哈希、源/目标版本、开始/结束、结果、回滚原因；失败告警。
- **证据形式**：权限表、审计动作集合、升级审计样例、二次认证记录。

### TC-UPG-05 SBOM 与版本可追溯

- **关联条目**：UPG-05（部分）
- **前置条件**：仓库与构建产物可用。
- **步骤**：

```bash
# 1. 仓库中没有 SBOM 文件
find . -path './frontend/node_modules' -prune -o \( -iname '*sbom*' -o -iname '*.cdx.json' \
  -o -iname '*.spdx*' \) -print | head -20

# 2. 文档声称随版本附带 SBOM
grep -n 'SBOM' THIRD_PARTY_NOTICES.md
cat VERSION

# 3. 版本接口与构建信息
api GET /api/v1/system/info; echo
cat "${BODY}"; echo
```

- **期望**：
  - **当前预期（差距）**：仓库/构建产物没有 SBOM；`THIRD_PARTY_NOTICES.md:30` 只是承诺；
    版本接口返回版本与 commit，但没有 SBOM 摘要/签名。
  - **整改后验收**：CI 生成 CycloneDX/SPDX SBOM 并签名，随发行包附带；SBOM 含组件版本、
    许可证、哈希与依赖关系；版本接口可返回 SBOM 摘要；送检材料可追溯到源码提交。
- **证据形式**：`find` 输出、SBOM 文件与签名、版本接口 JSON、CI 构建记录。

## 11. 证据归档建议

每个用例在 P6.4 送检材料中建立独立目录，建议结构：

```text
evidence/
  P6.0/
    TC-IDN-05/
      transcript.txt        # 终端命令与输出
      audit.json            # 审计查询响应
      server.log            # 服务端日志片段
    TC-AUD-03/
      before.json
      after.json
      db_schema.txt
      server.log
    ...
  summary/
    gap-matrix.csv          # 由 docs/compliance-gap.md 表格导出
    test-index.csv          # 用例编号 ↔ 条目 ↔ 结果
```

通用采集命令：

```bash
# 保存一次用例的完整终端记录
script -q -c "bash /tmp/run-TC-AUD-03.sh" evidence/P6.0/TC-AUD-03/transcript.txt

# 保存审计与告警
curl -sS -b "$COOKIE" "$BASE/api/v1/audit?pageSize=200" > evidence/P6.0/TC-AUD-03/audit.json
curl -sS -b "$COOKIE" "$BASE/api/v1/alerts?pageSize=50" > evidence/P6.0/TC-AUD-03/alerts.json

# 计算证据文件摘要，防止材料被替换
sha256sum evidence/P6.0/TC-AUD-03/* > evidence/P6.0/TC-AUD-03/SHA256SUMS
```

## 12. 通过标准与已知限制

- **当前阶段**：35 条用例预期全部呈现“当前预期（差距）”。用例“通过”指**差距被稳定复现**并留下证据，
  不是指整改要求已满足。
- **整改后阶段**：同一条用例必须同时满足“整改后验收”，并在干净数据目录重跑两次；
  连续两次结果一致才记为通过。
- **已知限制**：
  - GA/T 2155.2-2024 正式文本尚未取得，用例与条款的对应关系待补。
  - RTSP/ONVIF 用例使用内置 RTSP 服务与静态代码证据；真实摄像机/平台的接入测试在 P6.2/P6.3 补充。
  - 升级安全用例目前是“缺失功能的负向验证”；P6.1 实现升级器后需替换为正向安装/回滚用例。
  - 国密 TLS 用例需要 Tongsuo/GmSSL 构建环境，P6.1 提供 CI 任务后再执行完整握手。
  - 时间同步/断电一致性用例需要可控的时钟与电源注入工具，建议在送检实验室环境补充。
