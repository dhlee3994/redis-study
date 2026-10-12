# dict.c 흐름 지도: 키가 늘어도 조회가 느려지지 않는 이유

> 기준 소스: `redis/redis` **unstable** 브랜치, 커밋 `b540ca4` (2026-09-30)
> 함께 볼 파일 (전부 원본 코드 + `▶` 한국어 주석):
> - [dict/dict.h](dict/dict.h): 원본 전체
> - [dict/dict_발췌.c](dict/dict_발췌.c): 원본 2397줄 중 조회, 확장/축소, 점진적 rehash 부분
> - [dict/kvstore_발췌.c](dict/kvstore_발췌.c): 원본 1176줄 중 rehash 목록과 cron 진입점
> - [dict/주변_발췌.c](dict/주변_발췌.c): server.h, server.c, db.c, t_string.c, object.c, sds.h에서 dict와 맞닿은 부분

---

## 1. 한 줄 요약

**키의 해시값 하위 비트로 버킷을 바로 찾고(배열 인덱싱), 키 수가 버킷 수와 같아지면 버킷을 2배로 늘려서 버킷당 평균 키 수를 1 이하로 묶어 둡니다. 늘리는 작업은 한 번에 하지 않고, dict 연산이 올 때마다 버킷 하나씩, 그리고 100ms마다(부하와 상관없이) 1ms씩 나눠서 합니다.**

앞부분이 "평균 O(1)"의 이유이고, 뒷부분이 "tail latency를 낮추는 방법"입니다. 뒷부분이 완벽하지는 않아서, 남는 비용이 8절의 tail latency 지점들입니다.

---

## 2. 어떤 파일이 무슨 일을 하나

| 파일 | 역할 | 이번에 볼 것 |
|---|---|---|
| dict.h | 자료구조 정의 | `struct dict`의 `ht_table[2]`, `rehashidx` |
| dict.c | 해시 테이블 엔진. 키가 무엇인지는 모름 | 조회, 확장 조건, `dictRehash` |
| kvstore.c | dict 여러 개를 묶은 키 공간. 클러스터면 슬롯마다 dict 하나 | rehash 중인 dict 목록, cron 진입점 |
| server.c | 정책: 해시 함수, resize 허락 여부, fork 중 정책, cron 예산 | `dbDictType`, `dictResizeAllowed`, `databasesCron` |
| db.c | 명령이 키를 찾는 입구 | `lookupKey` → `dbFindByLink` |
| sds.h | 문자열 헤더에 길이가 들어 있음 | `sdslen()`이 O(1)이라는 점 하나 |
| siphash.c | 해시 함수 본체 | 읽지 않아도 됨. "시드를 쓰는 해시"라는 사실만 필요 |

`sds.c`는 이번 주제와 거의 관계가 없습니다. 해시 계산과 키 비교가 부르는 `sdslen()`은 `sds.h`의 인라인 함수입니다.

---

## 3. 자료구조 그림

### 3-1. dict 하나 (평소 / rehash 중)

```
평소                                    rehash 중 (옛 크기 8 → 새 크기 16)

dict                                    dict
├── ht_table[0] ─▶ [0][1][2]...[7]      ├── ht_table[0] ─▶ [ ][ ][ ][3][4][5][6][7]   ← 0~2번 칸은 이미 비움
├── ht_table[1] = NULL                  ├── ht_table[1] ─▶ [0][1][2] ... [15]          ← 새 키는 여기로만
├── ht_used = {6, 0}                    ├── ht_used = {4, 5}
└── rehashidx = -1                      └── rehashidx = 3   ← 다음에 옮길 옛 칸
```

- 테이블은 **최대 2개**입니다. rehash가 끝나기 전에는 다음 resize를 시작하지 않습니다.
- 크기는 항상 2의 거듭제곱입니다. 버킷 번호 = `hash & (크기 - 1)`, 나눗셈 대신 AND 한 번입니다.
- 키 수(`ht_used`)를 버킷 수로 나눈 값이 **load factor**이고, 평균 체인 길이와 같습니다.

### 3-2. 메인 키 공간의 모양 (Redis 8)

```
server.db[0].keys  (kvstore)
└── dicts[]        일반 모드: 1칸 / 클러스터 모드: 16384칸 (슬롯마다)
     └── dict      dictType = dbDictType (no_value=1)
          └── ht_table[0][idx]
               ├── 키가 하나면 → kvobj 포인터를 칸에 직접   ┌──────────────────────────────┐
               │                                          │ kvobj: 타입, LRU, 값 + "key" │
               └── 충돌이면   → {next, key} → {next, key} → kvobj ...   └──────────────────────────────┘
```

- 값과 키가 kvobj 하나에 같이 들어 있어서 dict는 포인터 하나만 저장합니다(`no_value=1`).
- 버킷에 키가 하나뿐이면 엔트리 구조체 없이 칸에 kvobj 포인터를 바로 넣습니다. 포인터 하위 비트로 "엔트리인지 키인지"를 구분합니다(dict.h 맨 위 설명).
- 조회 한 번에 따라가는 포인터 수가 줄어서 캐시 미스가 적습니다. 이것은 O(1) 안의 상수를 줄이는 장치이고, O(1)이 되는 이유와는 별개입니다.

---

## 4. GET 한 번의 경로

```
getGenericCommand                          t_string.c:456
 └ lookupKeyReadOrReply                    db.c:402
    └ lookupKeyRead → lookupKeyReadWithFlags → lookupKey     db.c:373 → 366 → 296
       └ dbFindByLink                      db.c:3138
          ├ getKeySlot                     db.c:495   일반 모드는 0
          └ kvstoreDictFindLink            kvstore.c:861   dicts[slot] 고르기
             └ dictFindLink                dict.c:863
                └ dictFindLinkInternal     dict.c:766   ★ 여기가 전부
                   ① hash = SipHash(key)                    dictSdsHash (server.c:436)
                   ② idx  = hash & mask
                   ③ rehash 중이면 버킷 하나 옮김            _dictRehashStepIfNeeded (dict.c:1720)
                   ④ table 0 → (rehash 중이면) table 1 체인 순회
                      비교: 포인터 같음? → 길이 같음? → memcmp   dictSdsCompareKV (server.c:333)
```

`lookupKey`는 찾은 뒤에 만료 확인, LRU/LFU 갱신, 통계를 처리합니다. 해시 테이블 조회는 `dbFindByLink` 한 줄입니다.

---

## 5. 왜 평균 O(1)인가

### 5-1. 비용을 나눠 보면

| 단계 | 비용 | N(키 수)과의 관계 |
|---|---|---|
| ① 해시 계산 | 키 길이에 비례 | 무관 |
| ② 버킷 찾기 | AND 한 번 + 배열 인덱싱 | 무관 |
| ③ rehash 한 걸음 | 버킷 1개 (또는 빈 칸 최대 10개) | 무관 |
| ④ 체인 비교 | 체인 길이만큼 | **load factor에만 의존** |

①②③은 처음부터 N과 상관이 없습니다. 문제는 ④뿐이고, ④는 "버킷당 키가 몇 개냐"에 달려 있습니다.

### 5-2. load factor를 묶어 두는 규칙

`dictExpandIfNeeded` (dict.c:1649)의 규칙은 하나입니다. **키 수 ≥ 버킷 수가 되면, 키 수보다 큰 가장 작은 2의 거듭제곱으로 키운다.** 보통 2배가 됩니다.

그래서 키를 계속 넣어도 load factor는 대략 0.5와 1 사이를 오갑니다.

| 키 수 N | 버킷 수 | load factor α | 있는 키 찾을 때 평균 비교 ≈ 1 + α/2 |
|---|---|---|---|
| 1,000 | 1,024 | 0.98 | 1.49 |
| 1,000,000 | 1,048,576 | 0.95 | 1.48 |
| 100,000,000 | 134,217,728 | 0.75 | 1.37 |

키가 10만 배 늘어도 평균 비교 횟수는 1.5번 안팎입니다. 없는 키를 찾을 때는 체인 하나를 끝까지 보므로 평균 α번입니다.

삭제가 많아 키 수가 버킷 수의 1/8 이하로 떨어지면 축소합니다(`dictShrinkIfNeeded`, dict.c:1688). 버킷이 4개 이하면 축소하지 않습니다(dict.c:1693). 축소는 메모리 때문이고, load factor가 낮은 것은 조회 속도에는 오히려 유리합니다.

### 5-3. 전제: 해시가 고르게 퍼져야 한다

위 계산은 "키가 버킷에 고르게 흩어진다"는 가정 위에 있습니다. 누군가 같은 버킷에 떨어지는 키만 골라 보내면 체인 하나에 N개가 몰리고 조회는 O(N)이 됩니다(hash flooding).

Redis는 서버를 켤 때마다 16바이트 시드를 무작위로 만들고(server.c:8328), 그 시드로 SipHash를 돌립니다(dict.c:116). 시드를 모르는 바깥에서는 충돌하는 키를 미리 계산할 수 없습니다. 버킷 번호에 해시의 **하위 비트만** 쓰기 때문에 하위 비트까지 잘 섞어 주는 해시가 필요하다는 점도 같은 이유입니다.

### 5-4. O(1)이지만 상수는 커진다

평균 **비교 횟수**는 N과 무관하지만, 조회 1회의 **시간**은 N이 커지면 조금 늘어납니다. 키가 1억 개면 버킷 배열만 1GiB라서 CPU 캐시에 들어가지 않습니다. 조회 한 번에 "버킷 배열 → (엔트리) → kvobj"를 따라가며 매번 DRAM까지 갈 가능성이 큽니다. 이 증가는 N에 비례하지 않고, 버킷 배열이 캐시 계층(L1/L2/L3)과 TLB가 감당하는 범위를 하나씩 넘을 때마다 계단식으로 몇 번 늘어나는 정도입니다. 소스로는 확인할 수 없는 하드웨어 특성이라 측정으로 확인할 대상입니다. 3-2절의 엔트리 생략과 kvobj에 키를 붙여 넣는 설계가 이 상수를 줄이려는 것입니다.

---

## 6. resize: 언제, 얼마나

### 6-1. 조건

| 상황 | 확장 조건 | 축소 조건 | 근거 |
|---|---|---|---|
| 평소 (`DICT_RESIZE_ENABLE`) | 키 수 ≥ 버킷 수 | 키 수 × 8 ≤ 버킷 수 | dict.c:1665, 1700 |
| fork 자식이 살아 있음 (`AVOID`) | 키 수 ≥ 버킷 수 × 4 | 키 수 × 32 ≤ 버킷 수 | 같은 곳 |
| fork된 자식 프로세스 안 (`FORBID`) | 안 함 | 안 함 | 같은 곳 |
| maxmemory 근처 | load factor ≤ 1.618이고 새 테이블을 잡으면 maxmemory를 넘으면 **이번엔 안 함** | (같은 검사) | server.c:568 |

- 확장 크기는 `키 수 + 1` 이상인 가장 작은 2의 거듭제곱입니다(`_dictNextExp`). 평소엔 2배이고, fork 중에 load factor 4까지 밀렸다가 확장하면 8배가 됩니다.
- maxmemory 검사가 있는 이유는 server.c:562의 주석에 있습니다. 큰 버킷 배열을 잡는 순간 maxmemory를 넘으면 키 eviction이 한꺼번에 일어나거나 쓰기 요청이 거부될 수 있습니다. 그래서 체인이 평균 1.6개가 될 때까지는 확장을 미룹니다.

### 6-2. resize를 시작하는 곳 세 군데

| 어디서 | 함수 | 언제 |
|---|---|---|
| 삽입 | `dictSetKeyAtLink` (메인 키 공간), `dictFindLinkForInsert` (일반 dict) | 새 키를 넣기 직전 |
| 삭제 | `dictGenericDelete`, `dictTwoPhaseUnlinkFree` | 키를 뺀 직후 (축소만) |
| cron | `kvstoreTryResizeDicts` ← `databasesCron` | 100ms마다 DB 16개 × dict 16개씩 |

resize를 "시작"한다는 것은 빈 새 배열을 `ht_table[1]`에 걸고 `rehashidx = 0`으로 바꾸는 것까지입니다(`_dictResize`, dict.c:239). 키는 하나도 옮기지 않습니다. 단, 옛 테이블이 비어 있으면 바로 교체하고 끝납니다.

---

## 7. 점진적 rehash

### 7-1. 두 개의 엔진

| | (a) 요청이 낼 때 | (b) 주기적으로 (cron) |
|---|---|---|
| 누가 | 조회, 삽입, 삭제 모두 | `serverCron` → `databasesCron` → `kvstoreIncrementallyRehash` |
| 얼마나 | dict 조회·삽입·삭제 연산 1번마다 버킷 1개 (빈 칸은 최대 10개까지만 건너뜀). MGET·MSET·다중 키 DEL은 키마다 1걸음 | 100ms마다 최대 1ms (모든 DB 합계). 버킷 100개 단위로 시간 확인. 자식 프로세스가 없으면 **부하와 상관없이** 돎 |
| 어떤 버킷 | 이번 요청이 본 칸이 아직 안 옮겨졌으면 그 칸, 아니면 `rehashidx` 칸 | `rehashidx`부터 차례로 |
| 코드 | `_dictRehashStepIfNeeded` (dict.c:1720) | `dictRehashMicroseconds` (dict.c:449) |
| 끄는 법 | 끌 수 없음 | `activerehashing no` |

(a)만 있으면 요청이 끊긴 dict는 테이블 2개를 계속 들고 있게 됩니다. 메모리가 낭비되고 조회도 두 테이블을 봐야 할 수 있습니다. 그래서 (b)가 있습니다. 목적은 "요청이 끊긴 dict 마무리"지만, (b)는 바쁠 때도 똑같이 100ms마다 1ms를 씁니다. 8절 ⑤의 지연이 여기서 생깁니다.

메인 키 공간의 DEL은 (a) 중에서도 `dictTwoPhaseUnlinkFind` → `_dictRehashStep` 경로를 탑니다. 그래서 "이번 요청이 본 칸 먼저" 최적화 없이 항상 `rehashidx` 칸을 옮깁니다.

(a)에서 "이번 요청이 본 칸"을 먼저 옮기는 이유는 그 칸이 방금 CPU 캐시에 올라왔기 때문입니다. 이 칸은 `rehashidx`보다 뒤에 있을 수 있어서, `rehashidx` 뒤에도 이미 빈 칸이 생깁니다. 그래서 rehash 완료는 `rehashidx`가 끝에 닿았는지가 아니라 "옛 테이블 키 수가 0인지"로 판단합니다(`dictCheckRehashingCompleted`, dict.c:381).

클러스터 모드에서는 dict가 16384개라 cron이 다 훑으면 그것만으로 시간이 듭니다. kvstore는 rehash를 시작한 dict를 목록에 넣고(`rehashingStarted` 콜백), 끝나면 뺍니다(`rehashingCompleted` 콜백). cron은 이 목록의 맨 앞부터 처리합니다.

### 7-2. rehash 중의 규칙

| 동작 | 규칙 | 코드 |
|---|---|---|
| 조회 | 옛 테이블에서 이 칸 번호가 `rehashidx`보다 작으면 건너뛰고 새 테이블만 봄. 아니면 옛 → 새 순서로 | dict.c:788 |
| 삽입 | 항상 새 테이블. 옛 테이블은 줄어들기만 함 | dict.c:552, 1781 |
| 삭제 | 두 테이블 중 있는 쪽에서 | dict.c:644 |
| 다음 resize | 시작 안 함 | dict.c:1651 |

### 7-3. 삽입 속도를 rehash가 따라잡나

rehash 중에는 다음 확장을 못 하므로, 새 테이블이 꽉 차기 전에 이사가 끝나야 합니다. 삽입 1번마다 (a)가 최소 "키 1개 이상" 또는 "빈 칸 10개"만큼 진행합니다. 옛 테이블에 키 S개, 버킷 S개가 있으면 대략 S + S/10번의 연산이면 끝나고, 새 테이블(버킷 2S개)에는 그동안 새 키가 많아야 1.1S개 들어옵니다. 그래서 순수하게 삽입만 하는 경우에도 load factor가 1 근처일 때 rehash가 끝나고, cron이 돕는 만큼 더 일찍 끝납니다.

### 7-4. rehash가 멈추는 경우

| 원인 | 효과 | 코드 |
|---|---|---|
| safe iterator가 도는 중 (`KEYS` 등) | 그 dict의 rehash 전부 정지. 버킷이 순회 도중 옮겨지면 키를 빠뜨리거나 두 번 보기 때문 | dict.c:1190 |
| SCAN 한 번 처리 중 | 그 호출 동안만 정지 | dict.c:1583 |
| fork 자식이 살아 있음 (`AVOID`) | 요청 경로의 rehash도 멈춤. 단, 확장은 새 테이블이 옛 것의 4배 이상이면, 축소는 옛 테이블이 새 것의 32배 이상이면 계속. cron rehash는 아예 안 돔 | dict.c:416, server.c:1340 |
| 자식 프로세스 안 (`FORBID`) | 전부 금지 | dict.c:412 |

`AVOID`에서 멈추는 이유: 부모가 rehash로 메모리 페이지를 건드리면, 자식이 스냅샷을 위해 공유하던 페이지가 copy-on-write로 복사됩니다. 큰 테이블을 rehash하면 테이블 전체가 복사될 수 있습니다.

---

## 8. tail latency가 생기는 지점

평균은 위 규칙으로 지켜지지만, 특정 요청 하나가 비싸지는 순간들이 있습니다.

| # | 지점 | 누가 손해를 보나 | 크기 감 | 코드 |
|---|---|---|---|---|
| ① | 새 버킷 배열 할당 | resize를 일으킨 그 요청 + 메모리 | 버킷 수 × 8바이트. 키 2^26개(약 6,700만)를 넘는 순간 1GiB | dict.c:266 `zcalloc` |
| ② | 옛 버킷 배열 해제 | rehash 마지막 걸음을 밟은 요청 | 옛 배열 크기 | dict.c:387 `zfree` |
| ③ | 요청마다 rehash 한 걸음 | rehash 기간의 모든 요청 | 버킷 1개(키 1~2개 재해시) 또는 빈 칸 10개 | dict.c:1720 |
| ④ | 두 테이블 조회 | rehash 기간의 조회, 특히 없는 키 | 체인 최대 2개 | dict.c:786 |
| ⑤ | cron rehash | 그 1ms 동안 도착한 요청 | 100ms마다 최대 약 1ms + 100버킷 처리 시간 | server.c:1365 |
| ⑥ | fork 중 resize 보류 | BGSAVE/AOF rewrite 동안의 조회 | **fork 전에 rehash 중이 아니던 dict**: load factor 최대 약 4 (넘으면 ⑦). **fork 시작 때 이미 rehash 중이던 dict**: rehash가 멈추고, rehash 중이라 확장 검사도 건너뜀(dict.c:1651). 새 키는 전부 새 테이블에 쌓이므로 **자식이 끝날 때까지 load factor에 상한이 없음**. 조회는 두 테이블을 모두 봄 | dict.c:416, 1651, 1667 |
| ⑦ | fork 중 강제 확장 | 자식이 살아 있는 동안 load factor 4에 닿은 dict의 그 요청 | 8배 크기 배열을 한 번에 할당. 새 테이블이 4배 이상이라 이 rehash는 fork 중에도 진행됨(옛 배열 페이지는 CoW 대상). 자식이 끝난 직후에는 보류됐던 dict들이 2~4배로 확장을 시작함 | dict.c:416, 1667-1671 |
| ⑧ | maxmemory 근처 확장 보류 | 메모리가 빠듯할 때의 조회 | load factor 최대 1.618 | server.c:572 |
| ⑨ | 긴 체인 버킷 하나 옮기기 | 그 걸음을 밟은 요청 | 해시가 고르면 드묾 | dict.c:432 |

몇 가지 보충:

- **①의 실제 비용** (할당기·OS 동작에 대한 추정. 소스로는 `zcalloc`을 쓴다는 것만 확인됨): 큰 배열은 OS가 0으로 채운 새 페이지로 받아 오는 경우가 많아서, 할당 호출 자체는 생각보다 빠를 수 있습니다. 그 대신 rehash가 새 배열의 페이지를 처음 건드릴 때마다 page fault가 나고, 그 비용이 ③의 걸음들에 흩어집니다. 확실한 것은 rehash가 끝날 때까지 **옛 배열 + 새 배열**을 함께 들고 있다는 점입니다. 키 6,700만 개에서 확장하면 버킷 배열만 512MiB + 1GiB입니다.
- **③이 확장에서 더 비싼 이유**: dictEntry에 해시값을 저장하지 않아서(엔트리당 8바이트 절약), 확장할 때는 옮기는 키마다 SipHash를 다시 계산합니다. 축소는 옛 칸 번호에 마스크만 씌우면 되므로 해시를 다시 계산하지 않습니다(dict.c:345).
- **빈 칸 상한 10개**: 대량 삭제 뒤처럼 빈 칸이 길게 이어져도 한 걸음의 비용에 상한이 있습니다(dict.c:427). "끝날 때까지 빈 칸을 훑는" 일이 없도록 막는 장치입니다.
- **일반 모드 vs 클러스터 모드**: 일반 모드는 DB당 dict가 하나라서 ①②가 한 번에 크게 옵니다. 클러스터 모드는 노드 하나가 16384 슬롯을 다 가졌다고 쳐도, 같은 1억 키가 dict 하나에 평균 약 6,100개라서 배열 하나가 64KiB 정도이고, 각 dict가 서로 다른 시점에 조금씩 resize합니다.
- **cron 예산의 빈틈**: kvstore.c:686의 루프는 목록 맨 앞 dict가 `pauserehash` 상태면 진척 없이 같은 호출을 반복합니다. 코드상으로는 이때 1ms 예산을 다 쓰고 나옵니다. cron은 명령 실행 사이에 돌기 때문에 드문 상황입니다.

### Java HashMap과 비교 (Kotlin 개발자용)

| | Java `HashMap` | Redis dict |
|---|---|---|
| 확장 시점 | 크기 > 용량 × 0.75 | 키 수 ≥ 버킷 수 (load factor 1) |
| 확장 방식 | 그 `put()` 안에서 전체 엔트리를 한 번에 옮김 | 테이블 2개를 두고 요청마다 조금씩 |
| 확장 중 조회 | 해당 없음 (put이 끝나야 다음 연산) | 두 테이블을 봄 |
| 충돌 공격 대비 | 이미 8개인 체인에 추가될 때(9번째부터) 트리로 바꿈. 테이블이 64칸 미만이면 대신 resize. 키가 Comparable이면 O(log n) | 무작위 시드 SipHash로 충돌 자체를 예측 불가하게 |
| 축소 | 안 함 | load factor 1/8 이하면 축소 |

Java는 resize 한 번의 비용을 `put()` 하나가 떠안습니다. 스레드 하나로 모든 클라이언트를 처리하는 Redis에서는 그 한 번이 모든 클라이언트의 지연이 되므로 비용을 쪼갭니다.

---

## 9. 직접 확인해 보기

테스트용 로컬 인스턴스에서만 하세요. `DEBUG` 명령은 기본으로 꺼져 있어서 `enable-debug-command local`로 서버를 띄워야 합니다.

```bash
redis-server --port 6390 --enable-debug-command local --save "" --appendonly no
```

### 9-1. rehash 중인 두 테이블 보기

cron rehash를 끄고, 버킷 수 경계(1024)를 막 넘게 키를 넣은 뒤 테이블 상태를 봅니다.

```bash
redis-cli -p 6390 CONFIG SET activerehashing no
```

```bash
redis-cli -p 6390 EVAL "for i=1,1025 do redis.call('SET','k'..i,'v') end" 0
```

```bash
redis-cli -p 6390 DEBUG HTSTATS 0
```

`Hash table 0 stats (main hash table)`와 `Hash table 1 stats (rehashing target)` 두 개가 보이면 rehash 중입니다. 이 상태에서 조회를 여러 번 하면 (a) 경로가 이사를 마칩니다.

```bash
redis-cli -p 6390 EVAL "for i=1,2000 do redis.call('GET','k'..i) end" 0
```

다시 `DEBUG HTSTATS 0`을 보면 테이블이 하나로 돌아와 있습니다(키 1025개, 버킷 2048개). 그래도 두 개가 보이면 GET 횟수가 모자란 것이니 한 번 더 돌리면 됩니다. `INFO memory`의 `mem_overhead_db_hashtable_rehashing`은 rehash 동안 추가로 들고 있는 옛 배열 크기입니다(바이트 단위, keys·expires 등 키 공간 전체의 합).

`DEBUG POPULATE`는 넣기 전에 테이블을 필요한 크기로 미리 키우므로(debug.c:819) 이 실험에는 맞지 않습니다.

### 9-2. 확장이 없으면 O(N)이 되는지 보기

```bash
redis-cli -p 6390 FLUSHALL
```

```bash
redis-cli -p 6390 DEBUG DICT-RESIZING 0
```

```bash
redis-cli -p 6390 EVAL "for i=1,20000 do redis.call('SET','k'..i,'v') end" 0
```

```bash
redis-cli -p 6390 DEBUG HTSTATS 0 full
```

`DICT-RESIZING 0`이면 `dictResizeAllowed`가 항상 0을 돌려줍니다(server.c:570). 첫 테이블(버킷 4개)은 이 검사 없이 만들어지므로, 키 2만 개가 버킷 4개에 몰려 체인 길이가 5천 안팎이 됩니다. SET마다 체인을 훑으므로 위 EVAL 자체도 눈에 띄게 오래 걸립니다(그동안 서버 전체가 멈춥니다). `redis-benchmark -p 6390 -t get`이나 `redis-cli -p 6390 --latency`로 정상 상태와 비교해 보세요. 끝나면 `DEBUG DICT-RESIZING 1`로 되돌립니다.

---

## 10. Redis 8.0 vs unstable (dict.c, kvstore.c)

**점진적 rehash의 핵심 로직은 같습니다.** `_dictRehashStepIfNeeded`, `dictRehashMicroseconds`, `kvstoreIncrementallyRehash`는 두 브랜치에서 한 글자도 다르지 않습니다. `dictRehash`는 `dict_can_resize`를 `atomicGet`으로 읽는 줄만 다르고 로직은 같습니다(아래 표 첫 행). 확장/축소 조건과 숫자(1, 4, 1/8, 1/32, 1.618, 1ms)도 같습니다. 차이는 주로 API 정리입니다.

| 위치 | 8.0 | unstable |
|---|---|---|
| `dict_can_resize` | 일반 static 변수 | `redisAtomic` + `atomicGet`/`atomicSet` |
| `dictEntry` 필드 순서 | key, v, next | next, key, v (`dictEntryNoValue`와 앞부분을 맞춤) |
| no_value 체인 엔트리 | 포인터 하위 비트 `100`으로 표시 | 표시 없음 (레이아웃이 같아서 구분 불필요) |
| 키 비교 콜백 | `(dict*, k1, k2)` + `storedKeyCompare` | `(dictCmpCache*, k1, k2)` + `keyFromStoredKey` |
| `bucketChanged` 콜백, prefetch 훅 | 없음 | 있음 |

---

## 11. 이번에 답할 수 있게 된 질문

| 질문 | 답 | 근거 |
|---|---|---|
| 키가 1억 개여도 GET이 빠른 이유는? | 버킷은 배열 인덱싱으로 찾고, 버킷당 키 수(load factor)가 1 근처로 묶여 있어서 | 5절 |
| 그 "묶어 두기"는 누가 하나? | 삽입 직전 `dictExpandIfNeeded`가 키 수 ≥ 버킷 수면 2배로 | 6절 |
| 평균 O(1)이 깨지는 경우는? | 해시가 쏠릴 때(시드로 방어), `DEBUG DICT-RESIZING 0`, fork 시작 때 이미 rehash 중이던 dict(자식이 끝날 때까지). fork·maxmemory 보류는 상한(4, 1.618)이 있어 상수만 커짐 | 5-3, 8절 ⑥, 9-2절 |
| resize 때 Redis가 멈추지 않는 이유는? | 새 배열만 만들고, 키는 요청마다 1버킷 + cron 1ms씩 옮김 | 7절 |
| 그래도 느려지는 순간은? | 큰 배열 할당/해제, rehash 기간의 추가 걸음, fork 중 보류, cron 1ms | 8절 |
| BGSAVE 중에 왜 조회가 느려질 수 있나? | copy-on-write를 피하려고 resize와 rehash를 미뤄 체인이 길어짐. 이미 rehash 중이던 dict는 상한 없이 길어짐 | 7-4, 8절 ⑥ |
| `activerehashing no`로 하면? | cron rehash만 꺼짐. 요청 경로 rehash는 계속. 요청이 없는 dict는 두 테이블을 오래 들고 있음 | 7-1절 |
| sds.c를 봐야 하나? | 아니다. `sds.h`의 `sdslen()`이 O(1)이라는 것만 필요 | 2절 |

---

## 12. 읽는 순서

1. 이 문서 3절(그림)과 4절(GET 경로)
2. [dict/dict.h](dict/dict.h)의 `struct dict` (원본 L162, 해설판 L228)
3. [dict/dict_발췌.c](dict/dict_발췌.c) [2] 조회 → [4] 확장 결정 → [6] 점진적 rehash
4. [dict/kvstore_발췌.c](dict/kvstore_발췌.c) [3] [6]
5. [dict/주변_발췌.c](dict/주변_발췌.c) [4] [5] [6] (정책과 cron)
6. 이 문서 8절(tail latency)을 코드와 대조
7. 9절 실험

### 다음에 보면 좋은 것

- **t_hash.c, t_set.c**: [06_hash_set_인코딩_흐름지도.md](06_hash_set_인코딩_흐름지도.md)에 정리했습니다. 작을 때 listpack/intset을 쓰는 이유와, 큰 Hash/Set 안의 dict가 cron rehash와 maxmemory 검사를 받지 않는다는 점을 다룹니다.
- **memory_prefetch.c**: 여러 명령이 한꺼번에 파싱됐을 때(파이프라인이나 I/O 스레드) 조회를 모아 버킷과 kvobj를 미리 캐시에 올리는 기능. 5-4절 "상수"를 줄이는 또 다른 방법입니다.
- **evict.c**: 8절 ①과 이어지는 maxmemory eviction.
