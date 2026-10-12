# Blocking I/O와 Non-blocking I/O

> 필요한 주제: 1 Event Loop.

## 1. 한 줄 요약

- **Blocking**: 읽을 데이터가 없으면 `read`가 **데이터가 올 때까지 스레드를 재웁니다.**
- **Non-blocking**: 없으면 즉시 `-1`과 `errno = EAGAIN`("지금은 없으니 나중에 다시 해")을 돌려줍니다. 스레드는 멈추지 않습니다.

## 2. 왜 Redis는 반드시 non-blocking이어야 하나

```
스레드 1개, 클라이언트 1만 개

[blocking 이었다면]
  read(A) ── A가 아직 안 보냄 ── 스레드 잠듦 ... 나머지 9,999개 전부 멈춤

[non-blocking]
  read(A) → EAGAIN → 바로 다음 일로
```

Tomcat(요청마다 스레드 하나, blocking I/O)은 스레드 하나가 잠들어도 다른 스레드가 일하니 괜찮습니다. 대신 연결 수만큼 스레드가 필요하고, 스레드마다 메모리와 컨텍스트 스위칭 비용이 듭니다. Redis는 반대로 스레드 하나에 모든 소켓을 non-blocking으로 둡니다.

## 3. non-blocking만으로는 부족하다

non-blocking만 있으면 "어느 소켓에 데이터가 왔는지" 모릅니다. 1만 개를 계속 돌면서 `read`를 시도하면 대부분 EAGAIN이고 CPU만 낭비합니다(busy polling).

그래서 둘은 한 세트입니다.
- **non-blocking**: 절대 멈추지 않는다.
- **I/O 멀티플렉싱(epoll)**: 언제 시도하면 되는지 알려준다([05](05_IO멀티플렉싱_epoll_kqueue.md)).

## 4. 쓰기도 마찬가지

느린 클라이언트 때문에 커널 송신 버퍼([03](03_소켓.md) 3절)가 가득 차면 `write`는 일부만 쓰거나 EAGAIN을 돌려줍니다. 이때 Redis는 이렇게 합니다.
1. 못 보낸 나머지는 클라이언트 출력 버퍼에 남겨 둡니다.
2. 그 소켓에 "쓰기 가능해지면 알려 줘"(`AE_WRITABLE`)를 등록하고 다른 일을 합니다.

느린 클라이언트의 출력 버퍼가 끝없이 커지는 걸 막는 설정이 `client-output-buffer-limit`입니다.

또 한 클라이언트에 한 번에 64KB(`NET_MAX_WRITES_PER_EVENT`) 넘게 쓰지 않고 다음 바퀴로 넘깁니다. 큰 응답 하나가 루프를 오래 붙잡지 않게 하려는 것입니다(`networking.c:2975`).

## 5. 주의: Redis 안에도 blocking은 있다

non-blocking은 **네트워크 소켓** 이야기입니다. 아래는 여전히 메인 스레드를 멈출 수 있습니다.

| 멈추는 원인 | Redis의 대응 |
|---|---|
| 디스크 `fsync` | bio 스레드로 넘김(everysec) — [13](13_write와_fsync.md) |
| 큰 키 메모리 해제 | `UNLINK`로 bio 스레드에 넘김 — [09](09_백그라운드스레드_bio.md) |
| `fork` 자체 | 피할 수 없음, 시간만 측정 — [10](10_fork.md) |
| 명령 자체가 오래 걸림 (`KEYS`, 큰 `ZRANGE`) | 대응 없음. 주제 1 가설 "오래 걸리는 명령 하나가 p99를 올린다" |

`BLPOP` 같은 "블로킹 명령"은 이것과 다른 개념입니다. 응답만 미룰 뿐 스레드는 멈추지 않습니다(`blocked.c`).

## 6. Redis 소스에서 어디에 나오나

| 위치 | 내용 |
|---|---|
| `anet.c:82` `anetNonBlock` | 위의 `anetSetBlock`에서 `fcntl(fd, F_SETFL, flags \| O_NONBLOCK)`로 설정 |
| `socket.c:166-169` `connSocketRead` | `errno != EAGAIN`일 때만 에러로 처리. **EAGAIN은 정상입니다** |
| `socket.c:301` `connSocketAcceptHandler` | accept가 `EWOULDBLOCK`(= EAGAIN)이면 조용히 return |
| `networking.c:2935` `writeToClient` | 다 못 쓰면 남겨 두고 다음 기회에 |

## 7. 직접 확인

[README.md](README.md)의 strace에서 이 두 줄을 봅니다.

```
accept4(18, ..., SOCK_CLOEXEC|SOCK_NONBLOCK) = 24
accept4(18, ..., SOCK_CLOEXEC|SOCK_NONBLOCK) = -1 EAGAIN (Resource temporarily unavailable)
```

연결 하나를 받은 뒤 **혹시 또 있나 한 번 더** accept를 시도하고, 없으니 EAGAIN을 받고 바로 루프로 돌아갑니다. blocking이었다면 두 번째 accept에서 다음 클라이언트가 올 때까지 Redis 전체가 멈췄을 것입니다.

## 8. 여기서 멈춤

- **설명할 수 있으면 충분:** EAGAIN이 에러가 아닌 이유, non-blocking만으로 부족한 이유, 네트워크 말고 Redis를 멈추게 하는 것들.
- **몰라도 됨:** "동기/비동기 × 블로킹/논블로킹" 2×2 표 논쟁, POSIX AIO, io_uring.
