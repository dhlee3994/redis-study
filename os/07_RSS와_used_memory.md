# RSS와 used_memory: 메모리 숫자가 두 개인 이유

> 필요한 주제: **3 Object / Encoding, 4 ZSET (지금)**.

## 1. 한 줄 요약

- **`used_memory`**: Redis(zmalloc)가 "할당받았다"고 직접 센 바이트입니다.
- **`used_memory_rss`**: OS가 보기에 이 프로세스가 **실제 물리 메모리(RAM)에 올려 둔** 바이트입니다. RSS = Resident Set Size.

운영에서 "메모리 몇 GB 쓰냐"고 할 때, 모니터링 툴과 OOM killer가 보는 건 **RSS**입니다.

## 2. 가상 메모리와 물리 메모리 (최소한만)

- 프로세스가 보는 주소는 **가상 주소**입니다. OS가 이걸 실제 RAM 위치에 **페이지(보통 4KB) 단위**로 연결해 줍니다.
- 가상 주소를 받아만 두고 한 번도 안 쓴 페이지는 RAM에 없습니다. **실제로 RAM에 올라간 페이지의 합이 RSS**입니다.
- 페이지는 [11](11_copy_on_write.md)에서 다시 나옵니다.

## 3. 두 숫자가 다른 이유

```
RSS (OS가 본 것)
├── used_memory (Redis가 센 것)
│     └── 실제 데이터 + 크기 클래스 반올림 낭비 (06)
├── 페이지 안의 빈칸들        ← 단편화 (08). free 했지만 OS에 못 돌려준 것
├── jemalloc이 쥐고 있는 여유 페이지  ← 다시 쓸까 봐 바로 반납 안 함
└── 실행 코드, 라이브러리, 스택 등  ← 빈 Redis에서도 수 MB
```

반대로 **RSS < used_memory**이면 일부 페이지가 스왑(디스크)으로 나갔다는 뜻입니다. Redis에서는 latency가 크게 나빠지는 위험 신호입니다.

## 4. 비율 읽는 법

`mem_fragmentation_ratio` = RSS / used_memory

| 상황 | 해석 |
|---|---|
| 데이터가 거의 없음 | **의미 없음.** 고정 비용(코드, 라이브러리) 때문에 10배, 20배가 나옵니다 |
| 1.0 ~ 1.5 정도 | 흔히 정상 범위로 봅니다 |
| 1.5보다 꽤 큼 | 단편화를 의심합니다([08](08_메모리단편화.md)) |
| 1.0 미만 | 스왑을 의심합니다 |

할당자 내부 단편화만 따로 보려면 `allocator_frag_ratio`를 봅니다. 사람이 읽기 쉬운 진단은 `MEMORY DOCTOR`가 해 줍니다.

## 5. 주제 3·4 실험과 연결

DEL과 UNLINK를 비교하는 실험에서는 **`used_memory`와 `used_memory_rss`를 둘 다 기록하세요.** `used_memory`는 지우자마자 줄어도, RSS는 바로 안 줄거나 거의 안 줄 수 있습니다. 이유는 [08](08_메모리단편화.md)에 있습니다.

## 6. Redis 소스에서 어디에 나오나

| 위치 | 내용 |
|---|---|
| `zmalloc.c:658` `zmalloc_used_memory` | `used_memory` |
| `zmalloc.c:774` `zmalloc_get_rss` (리눅스) | `/proc/<pid>/stat`의 24번째 필드를 읽어서 RSS 계산. Mac 버전은 789줄(`task_info`) |
| `server.c:6730`, `:6732` | `INFO`의 `used_memory`, `used_memory_rss` 출력 |
| `server.c:6775` | `mem_fragmentation_ratio` |

`used_memory_rss`는 `server.cron_malloc_stats`에서 가져옵니다. 즉 serverCron이 주기적으로 갱신한 값이고, 실시간 값이 아닙니다. [README.md](README.md)의 strace에서 걸러 낸 "100ms마다 `/proc/self/stat` 읽기"가 바로 이 측정입니다.

## 7. 직접 확인

```bash
docker exec redis-os redis-cli INFO memory | grep -E "^(used_memory|used_memory_rss|mem_fragmentation_ratio):"
```

빈 상태에서 한 번, 100만 키를 넣고 한 번 봅니다.

```bash
docker exec redis-os redis-cli DEBUG POPULATE 1000000
```

실제 결과:

| 상태 | used_memory | used_memory_rss | mem_fragmentation_ratio |
|---|---|---|---|
| 빈 상태 | 1.0 MB | 19.1 MB | 19.13 ← 의미 없음 |
| 100만 키 | 81.5 MB | 100.5 MB | 1.23 ← 정상 |

키를 절반 지웠을 때 어떻게 되는지는 [08](08_메모리단편화.md)에서 이어서 봅니다.

## 8. 여기서 멈춤

- **설명할 수 있으면 충분:** 두 숫자의 정의, 비율 읽는 법, DEL 후에도 RSS가 안 줄 수 있다는 것.
- **몰라도 됨:** 페이지 테이블 구조, MMU, TLB, `/proc/<pid>/smaps` 필드 전체.
