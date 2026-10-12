# write와 fsync

> 필요한 주제: 7 Persistence의 핵심 (실제 4주차). [12 페이지 캐시](12_page_cache.md)를 먼저 읽으세요.

## 1. 한 줄 요약

`fsync(fd)`는 "이 파일의 페이지 캐시 내용이 **디스크에 실제로 기록될 때까지** 기다려"라는 시스템 콜입니다. 디스크 응답 시간만큼 걸립니다. 보통 ms 단위이고, 디스크가 바쁘면 그보다 훨씬 깁니다. 명령 처리(µs 단위)보다 수백~수천 배 느립니다.

| 시스템 콜 | 하는 일 | 걸리는 시간 |
|---|---|---|
| `write` | 유저 메모리 → 페이지 캐시 복사 | µs |
| `fsync` | 페이지 캐시 → 디스크, 끝날 때까지 대기 | ms ~ 그 이상 |

## 2. appendfsync 세 가지 = "fsync를 누가, 언제 부르나"

| 설정 | 누가 | 언제 | 서버 장애 시 잃을 수 있는 양 | 비용 |
|---|---|---|---|---|
| `always` | **메인 스레드** | 이벤트루프 매 바퀴, **응답 보내기 전** | 거의 없음 | 매 바퀴 디스크 대기 → latency·처리량 크게 나빠짐 |
| `everysec` (기본) | **bio_aof 스레드** ([09](09_백그라운드스레드_bio.md)) | 1초마다 | 약 1초, 최악 약 2초 | 작음 |
| `no` | 아무도 안 부름 | 커널 마음대로 | 커널이 디스크에 쓰기 전까지 쌓인 양 | 없음 |

`always`도 정확히는 "명령마다"가 아닙니다. 한 바퀴 동안 쌓인 AOF 버퍼를 `write` 한 번, `fsync` 한 번으로 처리하고, 그다음에 그 바퀴의 응답을 보냅니다(묶어서 커밋). `beforeSleep`에서 `flushAppendOnlyFile`이 `handleClientsWithPendingWrites`보다 먼저 불리는 이유입니다(`server.c:2067-2070` 주석).

## 3. everysec의 "최악 2초"

`aof.c:1447-1466` `flushAppendOnlyFile`

```
매 바퀴 beforeSleep
 └ 이전 fsync가 bio에서 아직 안 끝났나?
     ├ 아니오 → write 하고 끝
     └ 예 → write도 미룸 (최대 2초)
            └ 2초가 넘어도 안 끝남
                 → 그냥 write 하고, aof_delayed_fsync++
                   로그: "Asynchronous AOF fsync is taking too long (disk is busy?)..."
```

- 디스크가 느리면 bio의 fsync가 밀리고, 그동안 쌓인 데이터는 디스크에 없습니다. 그래서 유실 범위가 1초가 아니라 **최악 약 2초**입니다.
- 2초를 넘겨 `write`할 때, 같은 파일에 fsync가 진행 중이면 리눅스에서 **`write` 자체가 메인 스레드를 막을 수 있습니다.** 로그 메시지의 "this may slow down Redis"가 이 뜻입니다. 운영에서 이 로그가 보이면 디스크부터 의심합니다.
- 반대로 write를 미루는 동안에도 클라이언트에는 응답이 나갑니다. 즉 **성공 응답을 받았는데 아직 `write`조차 안 된 데이터**가 잠깐 존재할 수 있습니다.

## 4. 리눅스 vs Mac: fsync도 다르다

`config.h:139-145`

```c
#if defined(__linux__)
#define redis_fsync(fd) fdatasync(fd)            // 메타데이터는 빼고 데이터만. 조금 빠름
#elif defined(__APPLE__)
#define redis_fsync(fd) fcntl(fd, F_FULLFSYNC)   // 디스크 내부 캐시까지 비움. 매우 느림
#else
#define redis_fsync(fd) fsync(fd)
#endif
```

**Mac 로컬에서 `appendfsync always`의 성능을 재면 리눅스보다 훨씬 나쁘게 나옵니다.** 실험은 Docker(리눅스)에서 하세요. 다만 Docker on Mac의 디스크도 가상 디스크라서 운영 서버(클라우드 볼륨 등)의 절대값과는 다릅니다. **정책 간 상대 비교**만 믿으세요.

## 5. Redis 소스에서 어디에 나오나

| 위치 | 내용 |
|---|---|
| `aof.c:1408` `flushAppendOnlyFile` | 전체 흐름 |
| `aof.c:1479` `aofWrite` | `write` |
| `aof.c:1591-1607` | `always`: 메인 스레드에서 `redis_fsync`. **실패하면 `exit(1)`** (안전을 약속했으니 차라리 죽음) |
| `aof.c:1608-1615` | `everysec`: 1초 지났고 진행 중인 fsync가 없으면 `aof_background_fsync` → `bio.c:252` `bioCreateFsyncJob` |
| latency monitor 이벤트 `aof-fsync-always` | `always`의 fsync 시간이 기록됨 (`LATENCY LATEST`) |

## 6. 직접 확인

AOF를 켜고 `always`로 바꿉니다.

```bash
docker exec redis-os redis-cli CONFIG SET appendonly yes
```

```bash
docker exec redis-os redis-cli CONFIG SET appendfsync always
```

[README.md](README.md)의 strace(`-f` 포함)를 띄운 상태에서 SET을 보내고, 다시 `everysec`으로 바꿔 SET을 보냅니다. 실제 결과:

```
[always]
[pid     1] fdatasync(25)               = 0     ← pid 1 = 메인 스레드가 직접 fsync

[everysec]
[pid     1] write(25, "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n", 27) = 27
[pid    27] fdatasync(25)               = 0     ← pid 27 = bio_aof 스레드
```

같은 `fdatasync`인데 **부르는 스레드가 다릅니다.** 이게 세 설정의 차이 전부입니다. 스레드 번호는 [09](09_백그라운드스레드_bio.md) 3절의 목록으로 확인합니다.

주제 7에서 해 볼 것: 세 설정 각각에서 처리량과 p99를 비교합니다.

```bash
docker exec redis-os redis-benchmark -q -t set -n 100000 -d 100
```

## 7. 여기서 멈춤

- **설명할 수 있으면 충분:** write와 fsync의 차이, 세 설정 = fsync를 부르는 주체와 주기, 각 설정의 유실 범위(최악 2초 포함), always가 느린 이유, Mac에서 재면 안 되는 이유.
- **몰라도 됨:** 디스크 쓰기 캐시와 FUA, 파일시스템 저널, fsync 실패 시 커널 동작(fsyncgate).
