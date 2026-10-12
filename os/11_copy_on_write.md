# Copy-on-Write (COW)와 페이지

> 필요한 주제: 7 Persistence (실제 4주차). 2부 [05_dict_흐름지도.md](../05_dict_흐름지도.md) 7-4절에서 이미 한 번 나왔습니다.

## 1. 한 줄 요약

fork 직후 부모와 자식은 **같은 물리 메모리를 공유**합니다. 둘 중 하나가 어떤 페이지에 **쓰려는 순간**, 커널이 그 페이지(4KB) 하나만 복사해서 나눠 줍니다.

## 2. 그림

```
fork 직후
  부모 ─┐
        ├──▶ [페이지 1][페이지 2][페이지 3]   (공유, 읽기 전용 표시)
  자식 ─┘

부모가 페이지 2의 키를 SET
  부모 ──▶ [페이지 1][페이지 2'][페이지 3]    ← 페이지 2만 복사됨 (4KB)
  자식 ──▶ [페이지 1][페이지 2 ][페이지 3]    ← 자식은 fork 순간의 원본을 그대로 봄
```

- Redis에서 자식(RDB 저장)은 **읽기만** 합니다. 그래서 복사는 **부모가 명령을 처리하면서 쓴 페이지**에서만 생깁니다.
- 자식이 fork 순간의 메모리를 그대로 볼 수 있는 이유가 이것입니다([10](10_fork.md) 2절).

## 3. 결과: 쓰기가 많으면 메모리가 는다

- 최악의 경우: 저장하는 동안 쓰기가 메모리 전체에 고르게 퍼지면 **거의 모든 페이지가 복사되어 메모리가 2배**가 됩니다. "Redis 서버는 데이터 크기의 2배 가까운 여유 메모리를 두라"는 말이 여기서 나옵니다.
- **1바이트만 바꿔도 4KB를 복사합니다.** 키가 메모리에 흩어져 있으면, 적은 수의 쓰기로도 많은 페이지가 복사됩니다.

### Transparent Huge Pages (THP)

리눅스는 페이지를 2MB짜리로 크게 쓰는 THP 기능이 있습니다. 이게 켜져 있으면 **1바이트 쓰기에 2MB를 복사**하게 되어 COW 비용과 latency가 크게 나빠집니다. 그래서 Redis는 시작할 때 THP를 확인하고, 경고를 남기거나 자기 프로세스에서 끕니다.
- `server.c:7412` `THPDisable`, `server.c:7425` `linuxMemoryWarnings`
- 실험 환경 값: `cat /sys/kernel/mm/transparent_hugepage/enabled` → `always [madvise] never` (madvise = 요청한 곳에만 적용)

## 4. Redis가 COW를 줄이려고 하는 것들

| 무엇 | 어디 | 내용 |
|---|---|---|
| fork 중에는 dict resize 자제 | `server.c:827` `updateDictResizePolicy` | 자식이 있으면 `DICT_RESIZE_AVOID`. 부하율이 4배(`dict.c:46` `dict_force_resize_ratio`)를 넘을 때만 강제로 함. rehash는 테이블 전체에 쓰므로 페이지를 대량으로 복사시킴 |
| 자식은 resize 금지 | 같은 함수 | 자식 프로세스에서는 `DICT_RESIZE_FORBID` |
| 자식이 필요 없는 버퍼를 바로 버림 | `server.c:7845` `dismissMemoryInChild` | 부모가 곧 바꿀 버퍼(복제 버퍼, 클라이언트 버퍼 등)를 자식이 미리 반납해서 COW를 줄임. THP가 켜져 있으면 동작하지 않음 |

## 5. 직접 확인

`INFO persistence`의 `rdb_last_cow_size`(마지막 저장 중 복사된 바이트)를 봅니다. 저장 중이라면 `current_cow_size`를 봅니다(`server.c:6814`, `:6829`).

쓰기 없이 저장하는 경우:

```bash
docker exec redis-os redis-cli DEBUG POPULATE 1000000
```

```bash
docker exec redis-os redis-cli BGSAVE
```

```bash
docker exec redis-os redis-cli INFO persistence | grep rdb_last_cow_size
```

저장하는 동안 쓰기를 퍼붓는 경우(BGSAVE 직후 바로 실행):

```bash
docker exec redis-os sh -c 'redis-cli BGSAVE; redis-benchmark -q -t set -n 300000 -r 1000000 -d 100'
```

실제 결과 (100만 키, RSS 약 100MB, 저장에 약 1초):

| 조건 | rdb_last_cow_size |
|---|---|
| 저장 중 쓰기 없음 | 0.9 MB |
| 저장 중 SET 30만 번 | **9.5 MB** (10배 이상) |

주제 7에서 해 볼 것: 쓰기 부하를 늘리거나 키 수를 늘려서 COW가 RSS의 몇 %까지 가는지 봅니다. 가설 7 "보존 수준을 높이면 비용이 든다"의 메모리 쪽 비용입니다.

## 6. 여기서 멈춤

- **설명할 수 있으면 충분:** COW가 왜 생기는지, 저장 중 쓰기가 많으면 메모리가 왜 느는지, THP가 왜 나쁜지, fork 중에 dict resize를 왜 미루는지.
- **몰라도 됨:** 페이지 폴트 처리 과정, 다단계 페이지 테이블, KSM.
