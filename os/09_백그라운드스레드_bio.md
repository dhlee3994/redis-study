# 스레드와 Redis의 백그라운드 스레드 (bio)

> 필요한 주제: **3·4 (지금, UNLINK)**, 7 (fsync).

## 1. 한 줄 요약

"Redis는 싱글 스레드"는 **명령 실행이 스레드 하나에서 일어난다**는 뜻입니다. 프로세스 안에는 스레드가 여러 개 있습니다. 오래 걸리면서 결과를 기다릴 필요가 없는 일은 백그라운드 스레드에 넘깁니다.

## 2. 프로세스 vs 스레드 (최소한만)

| | 메모리 | Redis에서 |
|---|---|---|
| 프로세스 | 따로 가짐 | `fork`로 만든 자식 = RDB 저장 ([10](10_fork.md)) |
| 스레드 | 같은 프로세스 안에서 **공유** | bio 스레드, I/O 스레드 |

스레드는 메모리를 공유하므로 일을 넘기기 쉽습니다. 대신 같은 데이터를 동시에 만지면 경쟁이 생깁니다. 그래서 Redis는 **메인 스레드가 더 이상 만지지 않는 것만** 넘깁니다. 예를 들어 키 공간에서 이미 떼어 낸 객체가 그렇습니다.

## 3. 실제 스레드 목록

```bash
docker exec redis-os sh -c 'for t in /proc/1/task/*; do echo "$(basename $t) $(cat $t/comm)"; done'
```

실제 결과:

```
1   redis-server       ← 메인 스레드. 이벤트루프 + 모든 명령 실행
26  bio_close_file
27  bio_aof
28  bio_lazy_free
29  jemalloc_bg_thd    ← jemalloc 자체 스레드 (06)
30  jemalloc_bg_thd
186 jemalloc_bg_thd
```

`io-threads`를 2 이상으로 켜면 `io_thd_1`, ... 같은 I/O 스레드가 더 생깁니다(`iothread.c:980`). 이건 소켓 읽기/쓰기만 나눠 맡고, 명령 실행은 여전히 메인 스레드가 합니다.

## 4. bio 스레드 3개

`bio.c:50-66`

| 스레드 | 맡는 일 | 왜 메인에서 하면 안 되나 |
|---|---|---|
| `bio_close_file` | 파일 `close` | AOF rewrite 뒤 옛 파일처럼 이미 지운 큰 파일을 close하면, 커널이 디스크 블록을 해제하느라 오래 걸림 |
| `bio_aof` | AOF `fsync` (everysec) | 디스크 대기 = ms 단위 ([13](13_write와_fsync.md)) |
| `bio_lazy_free` | `UNLINK`, `FLUSHALL ASYNC`, `lazyfree-*` 설정 | 큰 키 = `free` 수백만 번 |

메인 스레드는 일을 큐에 넣기만 하고(`bioCreateLazyFreeJob`, `bioCreateFsyncJob`) 바로 다음 명령으로 넘어갑니다.

## 5. UNLINK의 경로 (주제 3·4 실험)

```
UNLINK key
 └ db.c:1556 unlinkCommand
    └ delGenericCommand(c, lazy=1)
       └ 키를 키 공간에서 즉시 떼어 냄   ← 이 순간부터 다른 클라이언트에게 안 보임
          └ lazyfree.c:201 freeObjAsync
             ├ effort = lazyfreeGetFreeEffort(obj)    (lazyfree.c:140)
             ├ effort > 64 (LAZYFREE_THRESHOLD)  → bio_lazy_free에 넘김
             └ effort ≤ 64                        → 그냥 메인에서 바로 free
```

"해제 비용(effort)"은 대략 **할당이 몇 개냐**입니다.

| 객체 | effort |
|---|---|
| ZSET (skiplist 인코딩) | 원소 수 (`zs->zsl->length`) |
| Hash, Set (hashtable 인코딩) | 원소 수 |
| **listpack 등 나머지** | **1** ("한 덩어리 할당"이라서) |

여기서 주제 3·4의 인코딩과 이어집니다.
- **128개짜리 listpack ZSET은 UNLINK를 해도 메인 스레드에서 바로 지워집니다.** 한 덩어리라서 넘기는 비용이 더 비싸기 때문입니다.
- 300만 원소 skiplist ZSET은 effort가 300만이므로 bio로 넘어갑니다.

`DEL`은 `lazyfree-lazy-user-del` 설정(기본 no)을 따릅니다(`db.c:1458`). 기본값이면 300만 원소를 **메인 스레드가 직접** 지웁니다.

**예측(실험 전):** 300만 원소 ZSET에서 `DEL`은 수백 ms 단위로 응답이 늦고, 그동안 다른 클라이언트의 p99도 같이 튈 것입니다. `UNLINK`는 즉시 반환되고, RSS는 bio 스레드가 정리하는 동안 천천히 줄 것입니다.

## 6. 여기서 멈춤

- **설명할 수 있으면 충분:** "싱글 스레드"의 정확한 뜻, bio 3종이 하는 일, UNLINK가 bio에 넘기는 조건(effort > 64)과 listpack은 넘기지 않는다는 것.
- **몰라도 됨:** pthread API, mutex와 condition variable로 짠 bio 큐의 세부, 메모리 배리어.
