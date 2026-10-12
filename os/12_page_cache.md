# 페이지 캐시

> 필요한 주제: 7 Persistence (실제 4주차). [13](13_write와_fsync.md)과 같이 읽으세요.

## 1. 한 줄 요약

`write()`는 디스크에 쓰지 않습니다. 커널 메모리에 있는 **페이지 캐시**에 복사하고 바로 "성공"을 돌려줍니다. 실제 디스크 기록은 커널이 나중에 합니다. 읽을 때도 페이지 캐시에 있으면 디스크까지 가지 않습니다.

## 2. 그림

```
Redis (aof_buf, 유저 메모리)
   │ write()   ← 여기서 "성공". 수 µs
   ▼
페이지 캐시 (커널 메모리, "dirty" 표시)
   │ 커널이 나중에 알아서 (리눅스 기본: 대략 30초 안)
   │ 또는 fsync()를 부르면 지금 당장 (13)
   ▼
디스크
```

## 3. 그래서 무엇을 잃나

| 장애 종류 | 페이지 캐시에 있던 데이터 |
|---|---|
| **Redis 프로세스만 죽음** (`kill -9`, OOM killer, 크래시) | **살아남습니다.** 커널은 멀쩡하므로 결국 디스크에 씁니다 |
| **서버 자체가 죽음** (전원, 커널 패닉, 클라우드 인스턴스 강제 종료) | **잃습니다** |

주제 7 가설 "성공 응답을 받은 데이터도 일부 잃을 수 있다"의 **정확한 범위**가 이 표로 정해집니다. 예를 들어 `appendfsync no`여도 Redis 프로세스만 죽는 경우에는 거의 잃지 않습니다. 반대로 `everysec`이어도 서버 전원이 나가면 최대 1~2초를 잃습니다([13](13_write와_fsync.md)).

Redis 메모리에만 있고 아직 `write`하지 않은 데이터(`aof_buf`)는 프로세스가 죽으면 당연히 잃습니다. 다만 Redis는 보통 응답을 보내기 전에 `write`를 끝내도록 순서를 잡아 둡니다. 예외는 everysec에서 디스크가 느려 write를 미루는 경우입니다([13](13_write와_fsync.md) 3·4절).

## 4. Redis가 페이지 캐시를 신경 쓰는 부분

- **페이지 캐시도 RAM을 씁니다.** 다만 프로세스가 아니라 커널 몫이라 RSS([07](07_RSS와_used_memory.md))에는 안 들어갑니다. `free -m`의 `buff/cache` 칸이 이것입니다.
- 그래서 Redis는 RDB/AOF 파일을 다 쓴 뒤 **그 파일의 페이지 캐시를 비우라고 커널에 알립니다.** 다시 읽을 일이 없는 파일이 Redis 데이터와 RAM을 두고 경쟁하지 않게 하려는 것입니다.
  - `util.c:1330` `reclaimFilePageCache` → `posix_fadvise(..., POSIX_FADV_DONTNEED)`
  - `bio.c:311`, `:337`: bio 스레드가 close/fsync 뒤에 호출

## 5. 직접 확인

dirty 페이지(디스크에 아직 안 쓴 페이지 캐시)의 양을 봅니다.

```bash
docker exec redis-os grep -E "^(Dirty|Writeback):" /proc/meminfo
```

AOF를 켜고 쓰기를 퍼부으면서 다시 보면 `Dirty`가 늘었다가 줄어듭니다.

```bash
docker exec redis-os redis-cli CONFIG SET appendonly yes
```

```bash
docker exec redis-os redis-cli CONFIG SET appendfsync no
```

```bash
docker exec redis-os redis-benchmark -q -t set -n 500000 -d 100
```

`/proc/meminfo`는 컨테이너가 아니라 Docker VM 전체의 값입니다. 다른 컨테이너가 돌고 있으면 숫자가 섞입니다.

## 6. 여기서 멈춤

- **설명할 수 있으면 충분:** write 성공 ≠ 디스크에 기록됨, 프로세스 크래시와 서버 장애의 차이, 페이지 캐시는 RSS에 안 들어간다는 것.
- **몰라도 됨:** `dirty_ratio`/`dirty_expire_centisecs` 튜닝, writeback 스레드, `O_DIRECT`, 파일시스템 저널링.
