# networking.c 흐름 지도 — 클라이언트 한 명의 일생

> 기준 소스: `redis/redis` **unstable** 브랜치, 커밋 `b540ca4` (2026-09-30)
> 함께 볼 파일: [networking/networking_발췌.c](networking/networking_발췌.c) (원본 6131줄 중 이벤트루프 경로만 실행 순서대로 발췌 + 주석)

---

## 1. 한 줄 요약

**읽을 때는 이벤트가 오면 바로 읽어서 실행하고, 응답은 메모리에 쌓아뒀다가 잠들기 직전에 한꺼번에 보냅니다. 한 번에 다 못 보낸 경우에만 쓰기 이벤트를 등록합니다.**

ae.c가 "언제 부를지"를 정했다면, networking.c는 "불렸을 때 무엇을 할지"를 정합니다.

---

## 2. 계층 구조: 누가 누구를 부르나

```
┌─────────────────────────────────────────────────────────────┐
│ ae.c          aeProcessEvents: epoll이 깨운 fd의 콜백 호출   │
├─────────────────────────────────────────────────────────────┤
│ socket.c      connSocketEventHandler: mask를 보고            │  ← connection 계층
│ (connection)  conn->read_handler / write_handler로 분기      │     TCP, TLS, Unix 소켓을
│               (BARRIER 처리도 여기서)                        │     같은 모양으로 감쌈
├─────────────────────────────────────────────────────────────┤
│ networking.c  readQueryFromClient / sendReplyToClient       │  ← 클라이언트 계층
│               querybuf 관리, RESP 파싱, 출력 버퍼 관리       │
├─────────────────────────────────────────────────────────────┤
│ server.c      processCommand → call → 명령 함수              │  ← 명령 실행
│ t_string.c 등 setCommand, getCommand ...                    │
└─────────────────────────────────────────────────────────────┘
```

위 그림은 TCP 기준입니다. connection 계층의 공통 모양은 connection.h가 정하고, 구현은 socket.c(TCP), tls.c(TLS), unix.c(Unix 소켓)로 나뉩니다. TLS에서 ae에 등록되는 콜백은 tls.c의 `tlsEventHandler`이고, Unix 소켓 핸들러는 TCP 핸들러를 다시 부릅니다.

TCP에서 ae에 등록되는 콜백은 networking.c 함수가 아니라 **socket.c의 `connSocketEventHandler`**입니다. 이 함수가 `connection` 객체에 달린 `read_handler`(= `readQueryFromClient`)나 `write_handler`(= `sendReplyToClient`)를 부릅니다. 01번 문서에서 본 "읽기·쓰기 콜백이 같은 함수"가 바로 이것입니다.

---

## 3. client 구조체: 이벤트루프와 관련된 필드만

`client`는 server.h에 정의돼 있고 필드가 100개 넘습니다. 흐름을 이해하는 데 필요한 건 이 정도입니다.

```
client
├── conn            소켓 (connection*). conn->fd가 ae의 events[fd] 인덱스
├── id              클라이언트 번호 (CLIENT LIST의 id)
│
├── [입력]
│   ├── querybuf     받은 바이트가 쌓이는 버퍼 (sds)
│   ├── qb_pos       querybuf에서 어디까지 파싱했나
│   ├── reqtype      INLINE / MULTIBULK
│   ├── multibulklen RESP 파서 상태: 남은 인자 수
│   ├── bulklen      RESP 파서 상태: 지금 읽는 인자 길이 (-1 = 모름)
│   └── argc, argv   파싱 완료된 명령. SET k v → argc=3, argv=[SET, k, v]
│
├── [출력]
│   ├── buf, bufpos  고정 16KB 응답 버퍼, 사용한 길이
│   ├── reply        넘친 응답을 담는 16KB 블록 리스트
│   └── sentlen      현재 블록에서 이미 보낸 바이트 수
│
├── flags            상태 비트들
│     CLIENT_PENDING_WRITE    쓰기 대기열에 올라가 있음
│     CLIENT_CLOSE_ASAP       종료 예약됨
│     CLIENT_CLOSE_AFTER_REPLY 응답 다 보내면 종료 (QUIT)
│     CLIENT_BLOCKED          BLPOP 등으로 대기 중
│
└── tid, running_tid  이 클라이언트를 맡은 스레드 (0 = 메인)
```

Kotlin 감각으로는 `class Client(val conn: Connection) { val input = StringBuilder(); var parsePos = 0; val output = ByteArrayOutputStream(); var flags = 0 }`입니다.

---

## 4. 전체 흐름 (`redis-cli SET k v`)

ae.c의 한 바퀴(①~⑥단계, 02번 문서 4절)에 겹쳐서 보면 이렇습니다.

```
━━━ 바퀴 N ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
③ epoll_wait → "리슨 소켓 읽기 가능"
⑤ connSocketAcceptHandler (socket.c)
     └ accept() → fd 9
     └ acceptCommonHandler
          ├ maxclients 검사
          ├ createClient
          │    └ connSetReadHandler(conn, readQueryFromClient)
          │         └ aeCreateFileEvent(fd 9, READABLE)   ★ 읽기 감시 시작
          └ connAccept → clientAcceptHandler
               └ (io-threads > 1이면) I/O 스레드에 배정

━━━ 바퀴 N+1 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
③ epoll_wait → "fd 9 읽기 가능"
⑤ connSocketEventHandler → readQueryFromClient
     ├ read(fd 9) 한 번 → querybuf = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
     └ processInputBuffer
          ├ processMultibulkBuffer → argv = [SET, k, v]
          └ processCommandAndResetClient
               └ processCommand (server.c) → call → setCommand
                    └ addReply(c, "+OK\r\n")
                         ├ _prepareClientToWrite
                         │    └ putClientInPendingWriteQueue   ★ 대기열에 이름만 올림
                         └ c->buf에 "+OK\r\n" 복사              ★ 아직 안 보냄!

━━━ 바퀴 N+2 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
① beforeSleep (server.c)
     ├ flushAppendOnlyFile     ← AOF 먼저 디스크에
     ├ handleClientsWithPendingWrites
     │    └ writeToClient(c, 0)
     │         └ write(fd 9, "+OK\r\n")   ★ 여기서 실제 전송
     │    └ 다 보냈음 → 쓰기 이벤트 등록 안 함
     └ freeClientsInAsyncFreeQueue
③ epoll_wait → 다음 명령까지 잠듦
```

응답은 바퀴 N+1에서 만들어지고 바퀴 N+2 시작 부분에서 나갑니다. 그 사이에 epoll 대기(잠)는 없습니다. 하지만 **다른 일은 낍니다.** write 전에 다음이 모두 먼저 실행됩니다.

- 바퀴 N+1에서 같이 깨어난 **다른 클라이언트들의 콜백(명령 실행)**
- ⑥ 타이머 (serverCron이 시각이 됐다면)
- 바퀴 N+2 `beforeSleep`의 앞 단계 (블록 클라이언트 처리, 만료 키 FAST 삭제, AOF write/fsync, I/O 스레드 클라이언트 처리)

그래서 같은 바퀴에 느린 명령 하나가 섞여 있으면, **이미 계산이 끝난 다른 클라이언트의 응답도 그만큼 늦게 나갑니다.**

### 응답이 커서 한 번에 못 보낼 때 (`KEYS *` 결과 10MB)

```
① beforeSleep → handleClientsWithPendingWrites → writeToClient
     └ write → 64KB까지 쓰고 중단 (또는 커널 송신 버퍼가 차서 EAGAIN)
     └ 남았음 → installClientWriteHandler
                 └ aeCreateFileEvent(fd 9, WRITABLE, ...)   ★ 이제서야 쓰기 감시

다음 바퀴들:
③ epoll_wait → "fd 9 쓰기 가능"
⑤ sendReplyToClient → writeToClient(c, 1) → 또 64KB
   ... 반복 ...
   다 보냄 → connSetWriteHandler(NULL) → aeDeleteFileEvent(fd 9, WRITABLE)  ★ 쓰기 감시 해제
```

---

## 5. 핵심 설계 6가지

### 5-1. 응답은 "모았다가" 보낸다

`addReply`는 소켓에 쓰지 않습니다. 대기열(`server.clients_pending_write`)에 이름을 올리고 메모리에 복사만 합니다.

쓰기 이벤트를 매번 등록하면 응답 하나에 `epoll_ctl`(등록) → 다음 바퀴에 epoll이 깨워줌 → `write` → `epoll_ctl`(해제)를 거칩니다. 시스템 콜이 3번입니다. 대기열 방식은 잠들기 직전 `write` 1번이면 끝납니다. 대부분의 응답은 작아서 이걸로 충분합니다.

### 5-2. 읽기는 이벤트당 `read()` 한 번

`readQueryFromClient`는 데이터가 더 남아 있어도 `read()`를 반복하지 않습니다. epoll이 level-triggered라서 남은 데이터는 다음 바퀴에 또 알려줍니다. 한 클라이언트가 계속 데이터를 보내도 다른 클라이언트에게 차례가 돌아갑니다.

### 5-3. 쓰기는 이벤트당 최대 64KB

`writeToClient`는 `NET_MAX_WRITES_PER_EVENT`(64KB)를 넘으면 멈춥니다. 응답이 거대한 클라이언트 하나가 루프를 붙잡지 못하게 하려는 것입니다. 예외로 maxmemory를 넘었을 때는 메모리를 빨리 비우려고 한도 없이 보냅니다. 레플리카와 MONITOR 클라이언트에도 이 한도가 적용되지 않습니다.

접속도 같은 원리입니다. `connSocketAcceptHandler`는 한 번에 최대 10명(`max-new-connections-per-cycle`)만 받습니다.

> **공정성 원칙**: 스레드가 하나뿐이라서, 어떤 작업이든 "한 번에 조금씩만" 하고 루프에 양보합니다. 남은 일은 level-triggered epoll이 다시 불러줍니다.

### 5-4. 파서는 "이어서 할 수 있게" 상태를 client에 저장

TCP는 메시지 경계를 보장하지 않습니다. `SET k v`가 `*3\r\n$3\r\nSE`와 `T\r\n$1\r\nk...` 두 번에 나눠 올 수 있습니다. 그래서 RESP 파서 상태(`qb_pos`, `multibulklen`, `bulklen`)를 지역 변수가 아니라 client에 저장해 두고, 다음 read 후에 이어서 파싱합니다.

반대로 한 번의 read에 명령이 여러 개 들어 있을 수도 있습니다(파이프라인). 그래서 `processInputBuffer`는 while 루프로 버퍼가 빌 때까지 반복합니다.

### 5-5. 종료도 "예약"했다가 나중에

`freeClientAsync`는 `CLIENT_CLOSE_ASAP`을 켜고 리스트에 넣기만 합니다. 실제 해제는 `readQueryFromClient` 끝의 `beforeNextClient`나 `beforeSleep`의 `freeClientsInAsyncFreeQueue`에서 합니다. 콜백 깊은 곳에서 바로 해제하면 위쪽 함수들이 해제된 메모리를 만지게 되기 때문입니다. ae.c의 타이머 삭제(`id = -1` 표시)와 같은 아이디어입니다.

### 5-6. 명령 실행은 항상 메인 스레드 하나

I/O 스레드(`io-threads > 1`)를 켜도 I/O 스레드가 하는 일은 **읽기, 파싱, 쓰기**뿐입니다. `processInputBuffer`에서 I/O 스레드는 명령을 완성하면 `CLIENT_IO_PENDING_COMMAND`를 켜고 클라이언트를 메인 스레드로 보냅니다. 실행(`processCommand`)은 메인 스레드만 합니다. 그래서 I/O 스레드를 켜도 Redis 명령은 여전히 원자적이고, 락이 필요 없습니다.

```
I/O 스레드 1: [read → parse] ──┐                       ┌──▶ [write]
I/O 스레드 2: [read → parse] ──┼──▶ 메인: [execute] ───┼──▶ [write]
I/O 스레드 3: [read → parse] ──┘                       └──▶ [write]
```

각 I/O 스레드는 자기 `aeEventLoop`를 가지고 `aeMain`을 돌립니다(iothread.c). 이 부분은 server.c 다음 단계에서 iothread.c와 함께 보면 좋습니다.

---

## 6. 출력 버퍼 2단 구조

```
c->buf (고정 16KB, client 생성 시 할당)
┌──────────────────────────────────────────┐
│+OK\r\n$5\r\nhello\r\n:42\r\n·····(빈칸)···│  bufpos까지 사용
└──────────────────────────────────────────┘
        │ 꽉 차면
        ▼
c->reply (연결 리스트는 client 생성 시 만들어 두고, 블록은 필요할 때만 할당)
[블록 16KB+] → [블록 16KB+] → [블록 16KB+] → ...
```

- 작은 응답은 `buf`에서 끝납니다. 메모리를 새로 할당하지 않습니다.
- `reply` 리스트에 블록이 남아 있는 동안에는 이후 응답도 `reply`로 갑니다. 순서를 지키기 위해서입니다. 리스트를 다 보내서 비우면 다시 `buf`를 씁니다.
- `reply`가 있으면 `writev`로 `buf`와 블록 여러 개를 시스템 콜 한 번에 보냅니다.
- `reply`가 `client-output-buffer-limit`을 넘으면(응답을 안 읽어가는 느린 클라이언트) 종료 예약합니다. 서버 메모리를 지키기 위해서입니다.

---

## 7. 읽기 쪽 메모리 트릭: 스레드 공용 query buffer

클라이언트 1만 명이 각자 16KB `querybuf`를 들고 있으면 160MB입니다. 그런데 어느 순간이든 대부분의 클라이언트는 받은 데이터를 다 처리해서 버퍼가 비어 있습니다.

그래서 `querybuf`가 비어 있는 클라이언트는 **스레드당 하나뿐인 공용 버퍼**(`thread_reusable_qb`)를 빌려서 읽습니다. 처리가 끝나서 비면 반납합니다. 덜 온 명령 조각이 남으면 **공용 버퍼를 통째로 그 클라이언트 소유로 넘기고**(복사 없음), 스레드는 다음에 새 공용 버퍼를 만듭니다. 한 스레드는 한 번에 한 클라이언트만 처리하니까 가능한 트릭입니다. 8.0에도 있습니다.

---

## 8. 놓치기 쉬운 지점

| 지점 | 설명 |
|---|---|
| `nread == 0` | 에러가 아니라 **상대방이 연결을 닫았다**는 뜻입니다(EOF). |
| `nread == -1`인데 연결 상태 정상 | EAGAIN, 즉 "지금은 읽을 게 없음"입니다. 그냥 돌아갑니다. |
| `server.current_client == NULL` | 명령 실행 중 이 클라이언트가 해제됐다는 신호입니다. `unlinkClient`가 NULL로 바꿉니다. |
| `c = NULL` (readQueryFromClient) | 해제된 client를 더 만지지 않으려고 포인터를 지워둡니다. |
| 인증 전 제한 | 인증 전에는 인자 10개, 인자당 16KB, querybuf 1MB까지만 받습니다. 메모리 공격을 막기 위해서입니다. |
| 큰 인자(32KB 이상) 최적화 | 그 인자만큼만 read해서 querybuf를 복사 없이 값 객체로 넘깁니다. `readQueryFromClient`의 `big_arg`와 `processMultibulkBuffer`의 Optimization이 짝입니다. |
| `processEventsWhileBlocked` | RDB 로딩이나 긴 Lua 스크립트 도중에 루프를 4바퀴까지 돌려서 `-LOADING`, `-BUSY` 응답이라도 보냅니다. |

---

## 9. Kotlin 의사코드 (비유용)

```kotlin
// 읽기 이벤트 콜백
fun readQueryFromClient(c: Client) {
    val n = c.conn.read(c.querybuf, 16 * 1024)      // 한 번만
    when {
        n == 0  -> { freeClientAsync(c); return }   // 상대가 끊음
        n < 0   -> { if (!c.conn.connected) freeClientAsync(c); return }
    }
    while (c.qbPos < c.querybuf.length) {
        val argv = c.parser.tryParse(c.querybuf, c) ?: break   // 덜 왔으면 다음에
        processCommand(c, argv)                                 // 안에서 addReply 호출
    }
    c.querybuf.delete(0, c.qbPos); c.qbPos = 0
}

// 명령 구현이 부름
fun addReply(c: Client, bytes: ByteArray) {
    if (!c.hasPendingReplies()) server.clientsPendingWrite.add(c)   // 첫 응답이면 대기열에
    c.output.write(bytes)                                           // 메모리에만 씀
}

// beforeSleep에서 부름
fun handleClientsWithPendingWrites() {
    for (c in server.clientsPendingWrite.drain()) {
        writeToClient(c)                                            // 바로 write
        if (c.hasPendingReplies()) loop.addWritable(c.fd) { writeToClient(c) }  // 남았을 때만
    }
}

fun writeToClient(c: Client) {
    var total = 0
    while (c.hasPendingReplies() && total < 64 * 1024) {
        val n = c.conn.write(c.output)
        if (n <= 0) break                                           // 소켓이 꽉 참
        total += n
    }
    if (!c.hasPendingReplies()) loop.removeWritable(c.fd)
}
```

---

## 10. Redis 8.0 vs unstable (networking.c)

unstable은 8.0보다 약 1500줄 깁니다. 대부분 성능 최적화이고, 이벤트루프 흐름(읽기 → 파싱 → 실행 → 응답 쌓기 → beforeSleep에서 쓰기)은 같습니다.

| 기능 | 8.0 | unstable | 발췌 파일에서 보이는 곳 |
|---|---|---|---|
| I/O 스레드 (`running_tid`, `enqueuePendingClientsToMainThread`) | 있음 | 있음 | 곳곳의 `IOTHREAD_MAIN_THREAD_ID` 검사 |
| 스레드 공용 query buffer | 있음 | 있음 | readQueryFromClient ③ |
| `writev` 묶어 쓰기 | 있음 | 있음 | _writeToClientNonSlave |
| **파이프라인 lookahead** (`pendingCommand`, 최대 16개 미리 파싱 + prefetch) | 없음 (1개 파싱 → 1개 실행) | 있음 | processInputBuffer의 안쪽 while |
| **큰 값 복사 회피** (`buf_encoded`, `BULK_STR_REF`) | 없음 | 있음 | _addReplyPayloadToBuffer의 `buf_encoded` 분기 |
| **클라이언트 압축** (`compression_state`) | 없음 | 있음 | readQueryFromClient 맨 앞 |

8.0 기준으로 공부하려면 위 표의 굵은 세 줄은 건너뛰고 읽으면 됩니다.

---

## 11. 읽는 순서

1. 이 문서 4절의 흐름도를 옆에 띄워두기
2. 발췌 파일 [2] `readQueryFromClient` → [3-1] `processInputBuffer` (파싱 부분은 대충)
3. [5-1] `addReply` → [5-3] `putClientInPendingWriteQueue` (짧지만 핵심)
4. [6-1] `handleClientsWithPendingWrites` → [6-2] `writeToClient`
5. [7] 종료, [1] 접속
6. 여유가 있으면 [3-2] `processMultibulkBuffer`로 RESP 파싱 세부 사항
