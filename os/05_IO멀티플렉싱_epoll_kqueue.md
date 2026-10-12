# I/O 멀티플렉싱: select, epoll, kqueue

> 필요한 주제: 1 Event Loop. 핵심 질문 "싱글 스레드인데 왜 빠른가?"의 답입니다.

## 1. 한 줄 요약

스레드 하나가 커널에 "**이 fd들 중 읽기/쓰기 준비된 것만** 알려 줘. 없으면 최대 N ms까지 재워 줘"라고 부탁하는 시스템 콜입니다.

## 2. 세대별 차이

| 방식 | OS | 동작 | 비용 |
|---|---|---|---|
| `select` / `poll` | 거의 모든 OS | 매번 fd 목록 **전체**를 커널에 넘기고, 커널이 전부 훑음 | O(전체 연결 수) |
| `epoll` | Linux | 관심 목록을 커널에 **한 번 등록**하고, 이후에는 **준비된 것만** 받음 | O(준비된 수) |
| `kqueue` | macOS, BSD | epoll과 같은 생각 | O(준비된 수) |
| `evport` | Solaris | 같은 생각 | |

연결 1만 개 중 지금 데이터가 온 게 10개라면, select는 1만 개를 훑고 epoll은 10개만 돌려받습니다.

## 3. epoll의 세 동작과 ae.c의 대응

| epoll 시스템 콜 | ae의 래퍼 (`ae_epoll.c`) | 언제 |
|---|---|---|
| `epoll_create` | `aeApiCreate` | 서버 시작 시 한 번 |
| `epoll_ctl` (ADD/MOD/DEL) | `aeApiAddEvent` / `aeApiDelEvent` | 클라이언트 접속, 보낼 응답이 생김, 연결 종료 |
| `epoll_wait` | `aeApiPoll` (`ae_epoll.c:89`) | 루프 매 바퀴 |

`epoll_wait`의 timeout이 **다음 타이머까지 남은 시간**입니다. [02_이벤트루프_흐름지도.md](../02_이벤트루프_흐름지도.md) 1절의 "다음 타이머 시각까지만 기다릴게"가 바로 이 인자입니다.

## 4. Redis가 OS별로 고르는 방법

`ae.c:32-41`에서 컴파일할 때 하나를 고릅니다. 우선순위는 **evport > epoll > kqueue > select**이고, `config.h`가 OS별로 `HAVE_EPOLL`, `HAVE_KQUEUE` 등을 정의합니다.

```
Mac에서 brew로 설치한 Redis   → ae_kqueue.c
Docker / 리눅스 운영 서버     → ae_epoll.c
```

**실험할 때 주의할 점:** Mac 로컬에서 측정한 latency를 리눅스 운영 환경에 그대로 대입하지 마세요. 구조적인 결론(오래 걸리는 명령이 p99를 올린다 등)은 같지만 수치는 다를 수 있습니다. 어떤 게 쓰이는지는 `INFO server`의 `multiplexing_api`로 확인합니다.

## 5. 레벨 트리거 (짧게)

Redis는 epoll의 기본 모드인 **레벨 트리거**를 씁니다(`ae_epoll.c`에 `EPOLLET` 없음). 데이터를 다 못 읽어도 다음 `epoll_wait`이 "아직 남았다"고 또 알려 줍니다. 그래서 Redis는 한 클라이언트를 끝까지 다 처리하지 않고, 조금씩 처리하고 다음 클라이언트로 넘어갈 수 있습니다.

## 6. 직접 확인

```bash
docker exec redis-os redis-cli INFO server | grep multiplexing_api
```

실제 결과: `multiplexing_api:epoll`

[README.md](README.md)의 strace로 `SET k v` 하나를 보냈을 때의 실제 결과에 해설을 붙였습니다.

```
epoll_pwait(7, [], 10128, 42, NULL, 8) = 0
  ↑ fd 7(메인 epoll)에서 최대 42ms 대기. 아무 일 없이 깸 = 타이머(serverCron) 시각이 됨

epoll_pwait(7, [{events=EPOLLIN, data=0x12}], 10128, 100, NULL, 8) = 1
  ↑ 0x12 = 18 = 리슨 소켓이 "읽기 가능" = 새 연결이 왔음

accept4(18, ...) = 24
accept4(18, ...) = -1 EAGAIN

epoll_pwait(7, [{events=EPOLLIN, data=0x18}], 10128, 61, NULL, 8) = 1
  ↑ 0x18 = 24 = 클라이언트 소켓에 데이터가 왔음

read(24, "*3\r\n$3\r\nSET\r\n...", 20474) = 27
write(24, "+OK\r\n", 5) = 5
```

- `10128` = 한 번에 받을 수 있는 최대 이벤트 수 = `maxclients(10000) + CONFIG_FDSET_INCR(128)`([01](01_fd_모든것은파일.md) 4절).
- `100` = 기본 `hz 10`일 때 serverCron 간격 100ms. 다음 타이머까지 남은 시간이 그때그때 timeout이 되므로 42, 61처럼 바뀝니다.
- `data=0x12` = epoll에 등록할 때 같이 넣어 둔 fd 번호(16진수). 어느 소켓 이벤트인지 이걸로 압니다.

## 7. 여기서 멈춤

- **설명할 수 있으면 충분:** select 대비 epoll의 이점, ae.c 세 함수와의 대응, timeout이 타이머에서 온다는 것, Mac은 kqueue라는 것.
- **몰라도 됨:** epoll 내부의 레드블랙 트리와 ready list, `EPOLLONESHOT`, 엣지 트리거 세부, thundering herd.
