# 메모리 할당자 (malloc, jemalloc)

> 필요한 주제: **3 Object / Encoding, 4 ZSET (지금)**.

## 1. 한 줄 요약

C에서 `malloc(100)`을 불러도 OS에 바로 가지 않습니다. **할당자(allocator)** 라는 라이브러리가 OS에서 큰 덩어리를 받아 와서 잘게 잘라 줍니다. Redis는 기본으로 **jemalloc**을 씁니다.

## 2. 그림

```
Redis 코드
  zmalloc(100)                  ← Redis의 래퍼. 할당량을 세서 used_memory로 보고
     │
     ▼
jemalloc
  "100바이트? 112바이트 칸 하나 줄게"   ← 크기 클래스로 반올림
  미리 받아 둔 페이지에서 잘라 줌
     │ (모자랄 때만, 가끔)
     ▼
OS (mmap 시스템 콜로 큰 덩어리)
```

### 크기 클래스 (size class)

jemalloc은 아무 크기나 잘라 주지 않고, 정해진 칸 크기 중 하나로 **올림**합니다. 작은 쪽은 대략 8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, ... 바이트입니다.
- 100바이트를 요청하면 112바이트 칸을 받습니다. 12바이트는 그냥 버려집니다.
- 같은 크기 칸끼리 한 페이지에 모아 두므로 관리가 빠르고, 단편화([08](08_메모리단편화.md))가 덜 생깁니다. Redis가 libc malloc 대신 jemalloc을 기본으로 쓰는 이유입니다.

Redis의 `used_memory`는 **반올림된 실제 칸 크기**로 셉니다(`zmalloc.c:249-262`. jemalloc이 알려 준 usable size나 `zmalloc_size(ptr)`로 `used_memory`를 올림). 즉 이 반올림 낭비는 `used_memory`에 이미 들어 있습니다.

## 3. 왜 주제 3·4에 중요한가

작은 데이터에 listpack을 쓰는 이유를 할당자 관점에서 다시 보면 이렇습니다.

```
ZSET 원소 100개

[listpack]  ┌──────────── 연속 메모리 1덩어리 ────────────┐
            할당 1번. 크기 클래스 낭비도 1번.

[skiplist + dict]
            skiplist 노드 100개   (각각 malloc, 레벨마다 크기 다름)
            member SDS 100개      (각각 malloc)
            dictEntry 100개       (각각 malloc)
            할당 수백 번. 칸마다 반올림 낭비 + 포인터 8바이트씩
```

- **128 → 129 인코딩 전환 때 메모리가 뛰는 이유**가 이것입니다. 한 덩어리였던 것이 수백 개의 작은 할당으로 쪼개집니다.
- **UNLINK가 의미 있는 이유**도 이것입니다. 300만 원소 ZSET을 지우려면 할당 수백만 개를 하나하나 `free` 해야 합니다([09](09_백그라운드스레드_bio.md)).
- Java의 `new`도 TLAB이라는 미리 받은 영역에서 잘라 주는 점은 비슷합니다. 하지만 C에는 GC와 압축(compaction)이 없어서 직접 `free` 해야 하고, 흩어진 빈칸이 그대로 남습니다([08](08_메모리단편화.md)).

## 4. Redis 소스에서 어디에 나오나

| 위치 | 내용 |
|---|---|
| `zmalloc.h:22`, `:32`, `:56` | 빌드에 따라 tcmalloc / jemalloc / libc 중 선택. `INFO memory`의 `mem_allocator` 값 |
| `zmalloc.c:246` `ztrymalloc_usable_internal` | malloc 호출 후 실제 칸 크기로 `used_memory` 증가 |
| `zmalloc.c:658` `zmalloc_used_memory` | `used_memory` 값 |
| `config.c:3477` `jemalloc-bg-thread` (기본 yes) | jemalloc 백그라운드 스레드. 안 쓰는 메모리를 OS에 천천히 돌려줌 |

## 5. 직접 확인

```bash
docker exec redis-os redis-cli INFO memory | grep mem_allocator
```

실제 결과: `mem_allocator:jemalloc-5.3.0`

스레드 목록([README.md](README.md))을 보면 `jemalloc_bg_thd`가 보입니다. jemalloc도 자기 스레드를 따로 돌리고 있다는 뜻입니다.

이번에는 문자열 길이를 1바이트씩 늘리면서 `MEMORY USAGE`(키 하나가 차지하는 바이트)를 재 봅니다.

```bash
docker exec redis-os sh -c 'for n in 1 8 9 16 17 40 44 45 48 49 64 65; do redis-cli SET k "$(head -c $n /dev/zero | tr "\0" a)" >/dev/null; printf "%s:%s " $n $(redis-cli MEMORY USAGE k); done; echo'
```

실제 결과 (값 길이:바이트):

```
1:56  8:64  9:64  16:72  17:72  40:96  44:96  45:104  48:104  49:104  64:128  65:128
```

- 1바이트씩 늘려도 값이 **계단처럼** 오릅니다. 여러 바이트 구간이 같은 칸에 들어가기 때문입니다. `MEMORY USAGE`는 키와 객체 헤더 등을 합친 추정치라 계단 폭이 jemalloc 크기 클래스와 1:1로 맞지는 않습니다. 요점은 "요청한 크기 ≠ 차지하는 크기"입니다.
- **44 → 45에서 한 번 더 뜁니다.** 44바이트까지는 객체 헤더와 문자열을 한 덩어리로 할당하는 `embstr`, 45부터는 따로 할당하는 `raw` 인코딩입니다(`object.c:346` `OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44`). 주제 3의 인코딩 전환을 문자열에서 본 것입니다.

## 6. 여기서 멈춤

- **설명할 수 있으면 충분:** 할당자가 무엇인지, 크기 클래스 때문에 요청 ≠ 실제라는 것, listpack이 할당 횟수를 줄인다는 것.
- **몰라도 됨:** jemalloc의 arena, tcache, extent, slab 내부 구조. 크기 클래스 표 전체.
