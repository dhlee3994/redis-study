# server.c 흐름 지도 — 루프를 조립하고 운영하는 쪽

> 기준 소스: `redis/redis` **unstable** 브랜치, 커밋 `b540ca4` (2026-09-30)
> 함께 볼 파일: [server/server_발췌.c](server/server_발췌.c) (원본 8615줄 중 이벤트루프 관련 부분만 서버 수명 순서대로 발췌 + 주석)

---

## 1. 한 줄 요약

**server.c는 시작할 때 루프에 콜백을 꽂아두고(`initServer`, `initListeners`), 매 바퀴 잠들기 전 정리(`beforeSleep`)와 100ms마다 하는 관리(`serverCron`)를 정하고, 명령이 들어오면 검사한 뒤 실행합니다(`processCommand`, `call`).**

세 파일의 역할을 정리하면 이렇습니다.

| 파일 | 역할 | 비유 |
|---|---|---|
| ae.c | 언제 부를지 (epoll, 타이머, 훅) | 엔진 |
| networking.c | 클라이언트 I/O (읽기, 파싱, 응답 버퍼, 쓰기) | 창구 직원 |
| server.c | 조립과 운영 (콜백 등록, 매 바퀴/주기 작업, 명령 실행 관문) | 지점장 |

---

## 2. 서버 시작 순서 (`main`)

```
main()
 ├ initServerConfig()        모든 설정 기본값
 ├ (설정 파일 / 명령줄 인자 읽기)
 ├ initServer()              ★ 루프 생성 + 타이머/훅 등록
 │   ├ aeCreateEventLoop(maxclients + 128)
 │   ├ aeCreateTimeEvent(1ms, serverCron)
 │   ├ aeCreateFileEvent(module_pipe, READABLE, modulePipeReadable)
 │   └ aeSetBeforeSleepProc(beforeSleep) / aeSetAfterSleepProc(afterSleep)
 ├ (모듈 로드)
 ├ initListeners()           ★ 6379 포트 listen + accept 핸들러 등록
 ├ InitServerLast()          백그라운드 스레드(bio), I/O 스레드 생성
 ├ loadDataFromDisk()        RDB/AOF 로딩 (오래 걸릴 수 있음)
 └ aeMain(server.el)         ★ 여기서 서버가 꺼질 때까지 머묾
```

리슨 소켓은 데이터 로딩 **전에** 열립니다. 로딩 중에 접속한 클라이언트에게도 `-LOADING` 에러로 응답하기 위해서입니다. 로딩 중에는 `aeMain`이 아직 돌지 않지만, 로딩 코드가 `processEventsWhileBlocked`를 주기적으로 불러서 루프를 잠깐씩 돌립니다.

---

## 3. 메인 루프(`server.el`)에 등록되는 것 전부

기본 설정의 단일 인스턴스에서 메인 루프가 감시하는 대표 항목입니다. 이 밖에 복제·클러스터 링크, RDB 전송 파이프, 모듈이 등록한 fd와 타이머 등은 기능을 켰을 때만 생깁니다.

| 종류 | 대상 | 콜백 | 등록 위치 | 역할 |
|---|---|---|---|---|
| 파일 READABLE | 리슨 소켓 (6379) | `connSocketAcceptHandler` | server.c `initListeners` | 새 접속 수락 |
| 파일 READABLE | 클라이언트 소켓 | `connSocketEventHandler` → `readQueryFromClient` | networking.c `createClient` | 명령 읽기 |
| 파일 WRITABLE | 클라이언트 소켓 (다 못 보냈을 때만) | `connSocketEventHandler` → `sendReplyToClient` | networking.c `installClientWriteHandler` | 남은 응답 쓰기 |
| 파일 READABLE | module_pipe | `modulePipeReadable` | server.c `initServer` | 모듈 스레드가 메인을 깨움 |
| 파일 READABLE | I/O 스레드 알림 fd (I/O 스레드마다 1개) | `handleClientsFromIOThread` | iothread.c | I/O 스레드가 메인을 깨움 |
| 파일 READABLE | bio 작업 완료 파이프 (항상) | `bioPipeReadJobCompList` | bio.c `bioInit` (← `InitServerLast`) | 백그라운드 스레드가 작업 완료를 알림 |
| 시간 | — | `serverCron` | server.c `initServer` | 100ms마다 관리 작업 |
| 훅 | — | `beforeSleep` | server.c `initServer` | 매 바퀴 잠들기 전 |
| 훅 | — | `afterSleep` | server.c `initServer` | 매 바퀴 깨어난 직후 |

---

## 4. 전체 그림: 루프 한 바퀴 (세 파일 합본)

ae.c의 ①~⑥단계에 networking.c와 server.c가 하는 일을 모두 겹쳤습니다.

```
aeMain() ─ while (!stop) ─▶ aeProcessEvents()
│
├─① beforeSleep()  ────────────────────────────────── server.c
│     (1) TLS 내부 버퍼에 남은 데이터 처리
│     (2) blockedBeforeSleep: 블록 timeout, WAIT 응답 등
│     (3) activeExpireCycle(FAST): 만료 키 빠른 삭제
│     (4) flushAppendOnlyFile: AOF 버퍼 → 디스크          ← 반드시 (6)보다 먼저
│     (5) I/O 스레드에서 넘어온 클라이언트 명령 실행
│     (6) handleClientsWithPendingWrites: 응답 write      ← networking.c
│     (7) sendPendingClientsToIOThreads
│     (8) freeClientsInAsyncFreeQueue: 종료 예약 해제     ← networking.c
│     (9) evictClients
│    (10) aeSetDontWait(dont_sleep)
│
├─② timeout 계산: DONT_WAIT면 0, 아니면 serverCron까지 남은 시간
│
├─③ epoll_wait()  ★ 잠듦 ★ ─────────────────────── ae_epoll.c
│
├─④ afterSleep()  ────────────────────────────────── server.c
│     시간 캐시 갱신(server.unixtime, mstime), 모듈 GIL 획득
│
├─⑤ 발생한 fd마다 콜백 ────────────────────────────── networking.c
│     리슨 소켓    → accept → createClient (읽기 이벤트 등록)
│     클라이언트   → readQueryFromClient
│                    → processInputBuffer (RESP 파싱)
│                    → processCommand ──────────────── server.c
│                         검사(존재/인자/인증/ACL/메모리/로딩/BUSY/PAUSE)
│                         → call → cmd->proc(c)  예: setCommand
│                              → addReply: 응답을 버퍼에 쌓고 대기열 등록
│                              → 데이터 바뀌었으면 AOF 버퍼/레플리카 버퍼에 기록
│     (쓰기 이벤트) → sendReplyToClient → 남은 응답 write
│
└─⑥ processTimeEvents ─▶ 시각이 됐으면 serverCron() ── server.c
      clientsCron, databasesCron, BGSAVE 조건 검사, replicationCron ...
      return 1000/hz  → 100ms 뒤 재예약
```

`SET k v` 하나를 따라가면 ⑤에서 읽고 실행해서 응답을 쌓고, ⑤의 `call` 안에서 AOF 버퍼에 기록합니다. 다음 ①의 (4)에서 AOF가 디스크에 쓰이고, (6)에서 응답이 나갑니다.

---

## 5. `beforeSleep`: 순서가 곧 설계

| 순서 | 작업 | 왜 이 위치인가 |
|---|---|---|
| (2) | 블록된 클라이언트 처리 | BLPOP이 깨어나면 LPOP을 실행해서 **데이터를 바꿉니다**. 그 변경도 (4)에서 AOF에 들어가야 합니다. |
| (4) | AOF flush | (6)보다 먼저 해야 `appendfsync always`일 때 **"OK를 받았으면 AOF에 fsync까지 끝났다"**가 보장됩니다. 기본값 `everysec`에서는 백그라운드 fsync가 진행 중이면 AOF write 자체를 최대 2초까지 미루고, 그사이 응답은 나갑니다. |
| (6) | 응답 전송 | 이번 바퀴에 쌓인 응답을 한꺼번에 보냅니다. 응답마다 쓰기 이벤트를 등록하는 비용을 아낍니다. |
| (8) | 종료 예약 해제 | 루프에서 가장 안전한 지점입니다. 어떤 콜백도 실행 중이 아닙니다. |
| (10) | `aeSetDontWait` | TLS 버퍼에 데이터가 남는 등 할 일이 남았으면 ③에서 잠들지 않게 합니다. |

`beforeSleep`은 초당 수만 번 불릴 수 있습니다. 그래서 안의 함수들은 전부 "할 게 없으면 즉시 리턴"하도록 짜여 있습니다.

---

## 6. `serverCron`: 100ms마다, 조금씩

`aeCreateTimeEvent(1ms, serverCron)`으로 등록된, 기본 설정에서 메인 루프의 유일한 정기 타이머입니다(active defrag, 시간 예산을 넘긴 eviction, 모듈 타이머가 켜지면 더 생김). 반환값 `1000/server.hz`로 다음 주기를 스스로 정합니다(기본 hz=10이면 100ms).

### 주기별로 하는 일

`run_with_period(ms)` 매크로로 "serverCron은 100ms마다 돌지만, 이건 1초에 한 번만"을 표현합니다. hz=10 기준입니다.

| 주기 | 작업 |
|---|---|
| 매번 (100ms) | hz 조정, LRU 시계, SIGTERM 확인 후 안전 종료, `clientsCron`, `databasesCron`, BGSAVE/AOF rewrite 시작 조건 검사, 자식 프로세스 종료 확인 |
| 100ms | 초당 명령 수 등 통계 샘플링, `clusterCron` |
| 1000ms | `replicationCron` (failover 중이면 100ms) |
| 5000ms | DB 키 개수, 클라이언트 수 로그 |

### "조금씩" 원칙

| 작업 | 한 번에 다 하면 | Redis가 하는 방식 |
|---|---|---|
| 클라이언트 점검 (`clientsCron`) | 1만 명 순회 동안 모두 멈춤 | 한 번에 `클라이언트 수 / hz`명만. 리스트를 회전시켜 1초에 한 바퀴 |
| 만료 키 삭제 (`activeExpireCycle`) | 만료 키 100만 개 삭제 동안 멈춤 | 무작위 샘플링 + 시간 예산. beforeSleep에서도 짧게(FAST) |
| 해시 테이블 rehash | 큰 dict 재배치 동안 멈춤 | 1ms 예산 안에서만 (`INCREMENTAL_REHASHING_THRESHOLD_US`) |

`dynamic-hz`(기본 켜짐)가 이 원칙을 보강합니다. 클라이언트가 많아서 한 번에 200명 넘게 점검해야 하면 hz를 2배씩 올립니다(최대 500). "더 자주, 더 조금씩" 하려는 것입니다.

---

## 7. 핵심 원칙: 메인 스레드는 절대 기다리지 않는다

스레드가 하나라서, 기다리는 순간 모든 클라이언트가 멈춥니다. 그래서 "기다려야 하는" 상황마다 다른 방법을 씁니다.

| 기다려야 하는 상황 | Redis의 대응 | 어디서 |
|---|---|---|
| 클라이언트 데이터 도착 | epoll에 맡기고 다른 일 | ae.c |
| 응답을 보낼 소켓이 꽉 참 | 쓰기 이벤트 등록하고 다음 바퀴에 | networking.c |
| `BLPOP` (데이터가 올 때까지) | 클라이언트를 **블록 상태로 표시**하고 키별 대기 목록에 올린 뒤 다음 클라이언트로. 키를 바꾸는 명령(LPUSH 등)이 실행되면 **그 명령 직후** 대기자를 깨움. timeout은 매 바퀴 `blockedBeforeSleep`에서 확인 | blocked.c, server.c processCommand |
| `CLIENT PAUSE` | `blockPostponeClient`로 블록 상태로 표시 | server.c processCommand |
| `BGSAVE` (디스크에 전체 저장) | `fork()`로 자식 프로세스가 저장. 부모는 serverCron마다 "끝났나?"만 확인 | server.c serverCron |
| AOF fsync (everysec) | 백그라운드 스레드(bio)가 1초마다 | aof.c, bio.c |
| 큰 키 삭제 (`UNLINK`) | 백그라운드 스레드가 해제 | lazyfree.c |
| SIGTERM | 시그널 핸들러는 플래그만 세우고, 실제 종료는 serverCron에서 | server.c serverCron |
| RDB 로딩, 긴 Lua 스크립트 | 피할 수 없음. 대신 도중에 `processEventsWhileBlocked`로 루프를 잠깐 돌려 `-LOADING`, `-BUSY` 응답 | networking.c |

Kotlin 감각으로는 "코루틴에서 절대 `Thread.sleep`이나 블로킹 I/O를 부르지 않는다"는 규칙을 서버 전체가 지키는 것입니다. `BLPOP`은 `suspend` 함수와 같습니다. 스레드는 놀지 않고 다른 일을 하다가, 조건이 되면 이어서 실행합니다.

---

## 8. 시간 캐시

`afterSleep`이 깨어날 때마다 `updateCachedTime`으로 현재 시각을 전역 변수(`server.unixtime`, `server.mstime`)에 저장합니다. 명령 실행 중에 **객체에 접근할 때마다** `time()`을 부르는 대신 이 값을 읽으려는 것입니다.

다만 이 캐시가 "한 바퀴 동안 고정"되는 것은 아닙니다. `call()`이 최상위 명령을 실행할 때마다 `enterExecutionUnit()`으로 캐시를 다시 갱신합니다.
- 단조 시계가 하드웨어 시계(x86의 TSC)가 아니면: 명령마다 `ustime()`으로 갱신
- 하드웨어 시계면: 마지막 갱신 후 10µs가 지났거나 명령 25개마다 갱신 (시스템 콜을 아끼려고)

`call()`은 하드웨어 시계일 때 그걸로 명령 실행 시간도 잽니다.

---

## 9. `processCommand`와 `call`

### `processCommand`: 대부분 검사

```
명령 조회 ─▶ 존재?   ─ no ─▶ -ERR unknown command
          ─▶ 인자 수? ─ no ─▶ -ERR wrong number of arguments
          ─▶ 인증?   ─ no ─▶ -NOAUTH
          ─▶ ACL?    ─ no ─▶ -NOPERM
          ─▶ 클러스터 슬롯 맞나? ─ no ─▶ -MOVED
          ─▶ maxmemory? → 필요하면 eviction(키 삭제) → 그래도 부족 ─▶ -OOM
          ─▶ 디스크 에러? ─▶ -MISCONF / 읽기전용 레플리카? ─▶ -READONLY
          ─▶ 로딩 중? ─▶ -LOADING / 긴 스크립트 중? ─▶ -BUSY
          ─▶ CLIENT PAUSE? ─▶ 블록 상태로 (응답 없음, 나중에 재처리)
          ─▶ MULTI 중? ─▶ 큐에 쌓고 +QUEUED
          ─▶ ★ call(c) ★
```

거부도 `addReply`로 에러 응답을 쌓을 뿐이고, 응답은 다음 `beforeSleep`에서 나갑니다. 블로킹 I/O는 하나도 없습니다.

### `call`: 핵심은 한 줄

```c
c->cmd->proc(c);    // SET이면 setCommand(c), GET이면 getCommand(c)
```

그 앞뒤로 시간을 재서 슬로우로그와 통계에 남깁니다. `server.dirty`(변경 횟수)가 늘었으면 AOF와 레플리카로 전파할 목록에 넣습니다. 슬로우로그의 시간은 이 `proc(c)` 실행 시간뿐입니다. 네트워크 왕복이나 대기열 시간은 들어가지 않습니다. 여기가 1초 걸리면 그 1초 동안 루프 전체가 멈춥니다. `KEYS *`가 위험한 이유입니다.

---

## 10. I/O 스레드를 켜면: 루프가 여러 개

`io-threads 4`로 설정하면 `aeEventLoop`가 4개 돕니다(메인 1 + I/O 3). `io-threads`는 메인 스레드를 포함한 숫자라서 I/O 스레드는 N-1개만 만들어집니다. 모두 같은 ae.c 코드입니다.

```
메인 스레드 (server.el)                   I/O 스레드 1~3 (t->el)
┌──────────────────────────┐             ┌──────────────────────────┐
│ 리슨 소켓 accept          │             │ 맡은 클라이언트 소켓      │
│ serverCron               │             │   read + RESP 파싱        │
│ beforeSleep / afterSleep │             │   응답 write              │
│ ★ 명령 실행 (유일) ★      │             │ IOThreadCron (타이머)     │
│ 알림 fd ◀────────────────┼── 깨움 ─────┤                          │
│                          ├── 깨움 ────▶│ 알림 fd                   │
└──────────────────────────┘             └──────────────────────────┘
```

- 다른 스레드를 깨우는 방법은 `module_pipe`와 같습니다. 받는 쪽 루프에 알림용 fd(eventfd 또는 pipe)를 READABLE로 등록해 두고, 보내는 쪽이 거기에 씁니다.
- 명령 실행은 메인 스레드만 하므로 명령의 원자성은 그대로 유지됩니다.

---

## 11. Redis 8.0 vs unstable (server.c)

이벤트루프 관련 구조(초기화 순서, `beforeSleep` 순서, `serverCron` 구성)는 같습니다. 눈에 띄는 차이는 다음과 같습니다.

| 위치 | 8.0 | unstable |
|---|---|---|
| `beforeSleep` (5) I/O 스레드 클라이언트 처리 | 없음 (알림 fd로만 받음) | 있음. 알림 없이 넘어온 클라이언트도 잠들기 전에 마저 처리 |
| `beforeSleep` (1) 압축 데이터 처리 | 없음 | `clientCompressionProcessPendingData` |
| 미리 찾아둔 명령 재사용 | `iolookedcmd`: I/O 스레드가 미리 찾은 명령을 processCommand가 재사용 | `lookedcmd`: networking.c lookahead 단계에서 미리 찾은 명령 재사용 (스레드 무관) |
| `serverCron`의 `pendingCommandPoolCron`, `hashTemplatesCron`, `backupCron` 등 | 없음 | 있음 |

---

## 12. 지금까지 세 파일로 답할 수 있게 된 질문

| 질문 | 답 | 근거 |
|---|---|---|
| Redis는 왜 스레드 하나로 빠른가? | 기다리는 일을 epoll에 맡기고, 메모리 작업만 하며, 모든 일을 조금씩 나눠서 함 | ae.c, 7절 |
| 놀고 있을 때 CPU를 왜 안 쓰나? | epoll_wait가 다음 타이머까지 잠들기 때문 | ae.c `usUntilEarliestTimer` |
| 응답은 언제 실제로 나가나? | 명령 실행 후 다음 `beforeSleep`에서 | networking.c, 5절 |
| appendfsync always에서 OK는 디스크 기록 후인가? | 기본 설정에서는 그렇다. beforeSleep에서 AOF write·fsync가 응답 전송보다 먼저다. write barrier는 쓰기 핸들러가 설치된 경우를 막는 보조 장치. 단서: `no-appendfsync-on-rewrite yes`이고 자식 프로세스가 있으면 fsync를 건너뜀. io-threads > 1에서는 확인되지 않은 경로가 있음(검증_주제1.md 04:247) | 5절, 02번 6-2 |
| `KEYS *`는 왜 위험한가? | `call` 안의 실행 시간 동안 루프 전체가 멈춤 | 9절 |
| `BLPOP`은 스레드를 막지 않나? | 클라이언트를 블록 상태로 표시만 함. 키를 바꾸는 명령 직후 깨우고, timeout은 매 바퀴 확인 | 7절 |
| I/O 스레드를 켜면 명령이 동시에 실행되나? | 아니다. 실행은 메인 스레드만 함 | 10절 |
| 만료된 키는 언제 지워지나? | 읽을 때(lazy) + serverCron의 SLOW 사이클 + beforeSleep의 FAST 사이클 | 6절 |

---

## 13. 다음에 보면 좋은 것

- **iothread.c**: I/O 스레드와 메인 스레드가 클라이언트를 주고받는 방법(10절의 화살표 부분)
- **blocked.c**: BLPOP 같은 블로킹 명령의 구현
- **expire.c `activeExpireCycle`**: "조금씩" 원칙이 가장 잘 드러나는 함수(시간 예산, 무작위 샘플링)
- **aof.c `flushAppendOnlyFile`**: beforeSleep (4)의 실체, appendfsync 정책별 차이
