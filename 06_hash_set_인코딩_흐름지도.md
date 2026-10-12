# Hash와 Set의 인코딩 흐름 지도: 작을 때는 왜 해시 테이블을 안 쓰나

> 기준 소스: `redis/redis` **unstable** 브랜치, 커밋 `b540ca4` (2026-09-30)
> 먼저 볼 문서: [05_dict_흐름지도.md](05_dict_흐름지도.md) (dict의 조회, resize, 점진적 rehash)
> 함께 볼 파일 (전부 원본 코드 + `▶` 한국어 주석):
> - [hashset/t_set_발췌.c](hashset/t_set_발췌.c): t_set.c 2047줄 중 인코딩 선택, 변환, 조회 + intset.c 부록
> - [hashset/t_hash_발췌.c](hashset/t_hash_발췌.c): t_hash.c 6910줄 중 인코딩 선택, 변환, 조회

---

## 1. 한 줄 요약

**Hash와 Set은 원소가 적을 때 dict 대신 연속된 메모리 하나(listpack, intset)에 담고, 기준(128개, 512개, 64바이트)을 넘는 순간 한 번에 dict로 바꿉니다. 명령으로는 원소가 줄어도 돌아가지 않습니다.** (재시작·`DEBUG RELOAD` 같은 RDB 로딩 때만 크기 기준을 다시 적용합니다.)

05번 문서가 "dict는 왜 N이 커져도 O(1)인가"였다면, 이 문서는 "N이 작을 때는 O(1)을 포기하는 게 왜 이득인가"와 "Hash/Set 안의 dict는 메인 키 공간의 dict와 무엇이 다른가"를 다룹니다.

---

## 2. 인코딩 한눈에 보기

| 타입 | 인코딩 | 모양 | 조회 | 기본 기준값 |
|---|---|---|---|---|
| Set | intset | 정렬된 정수 배열. 원소 폭은 2/4/8바이트 중 하나로 통일 | 이진 탐색 O(log N) | 512개 이하, 전부 정수 |
| Set | listpack | `[원소][원소]...` 연속 메모리 | 처음부터 훑기 O(N) | 128개 이하, 원소 64바이트 이하 |
| Set | HT (dict) | `setDictType`, 멤버 sds 포인터 저장 | 평균 O(1) | 위를 넘으면 |
| Hash | listpack | `[필드][값][필드][값]...` 연속 메모리 | 필드만 골라 훑기 O(N) | 512개 이하, 필드/값 64바이트 이하 |
| Hash | HT (dict) | `entryHashDictType`, Entry(필드+값) 포인터 저장 | 평균 O(1) | 위를 넘으면 |

- 기준값은 `config.c`의 기본값이고 `CONFIG SET`으로 바꿀 수 있습니다 (`set-max-intset-entries`, `set-max-listpack-entries`, `set-max-listpack-value`, `hash-max-listpack-entries`, `hash-max-listpack-value`).
- Hash에는 필드 TTL용 `LISTPACK_EX`와 8.10부터 들어온 템플릿 인코딩(`TMPL_LP`, `TMPL_ARRAY`)도 있습니다. 이번 범위에서는 뺐습니다. 템플릿은 `hash-min-template-entries`가 기본 0이라 HSET에서는 자동으로 생기지 않습니다.
- 어떤 인코딩인지는 `OBJECT ENCODING key`로 볼 수 있습니다.

---

## 3. 작을 때 O(N)이 이기는 이유

### 3-1. 조회 한 번에 건드리는 메모리

```
listpack (원소 100개, 원소당 십여 바이트)
[hdr][a][b][c][d] ... [z]        ← 한 덩어리, 캐시 라인 수십 개를 순서대로 읽음
                                    CPU가 다음 줄을 미리 불러오기(prefetch) 좋은 패턴

dict (원소 100개)
버킷 배열 ──▶ [ ][•][ ][•]...     ← 1) 버킷 배열의 한 칸
                  │
                  ▼
                 sds "member"     ← 2) 멀리 떨어진 곳의 문자열 (충돌이면 엔트리를 하나 더 거침)
```

- dict는 비교 횟수가 적지만, 메모리 여기저기를 건너뛰며 읽습니다. DRAM까지 가는 캐시 미스 한 번은 대략 100ns 안팎입니다(일반적인 하드웨어 수치, 소스 밖 이야기).
- listpack은 비교 횟수가 많지만, 모든 원소가 붙어 있어서 대부분 이미 캐시에 있거나 순서대로 미리 불러와집니다.
- 그래서 N이 수백 이하이면 두 방식의 실제 시간 차이가 작고, listpack이 더 빠를 수도 있습니다. 차이가 작은 대신 **메모리는 listpack이 훨씬 적게** 씁니다. dict는 원소마다 버킷 칸, (충돌 시) 엔트리, 별도로 할당한 sds와 그 할당기 오버헤드가 붙고, listpack은 원소마다 길이 정보 몇 바이트만 붙습니다.

### 3-2. 그런데 왜 계속 listpack으로 두지 않나

listpack은 조회만 O(N)이 아니라 **삽입과 삭제도 O(N)** 입니다. 끝에 붙일 때도 메모리를 다시 할당(realloc)해서 통째로 복사할 수 있고, 중간을 지우면 뒤쪽을 당겨야 합니다. intset도 정렬을 유지하려고 삽입마다 뒤쪽을 `memmove`합니다(intset.c:233). N이 커지면 명령 하나가 N에 비례해서 느려지므로, 기준을 넘으면 dict로 갈아탑니다.

기준을 크게 올리면(예: `hash-max-listpack-entries 100000`) 메모리는 아끼지만, HGET/HSET 하나가 필드 수만큼 훑게 되어 그 키를 쓰는 모든 명령이 느려집니다. 스레드 하나로 도는 Redis에서는 다른 클라이언트의 지연으로 이어집니다.

---

## 4. 변환 흐름

### 4-1. Set

```
                 정수만, 512개 이하
            ┌──────────────────────┐
  SADD ───▶ │       intset          │──── 513개째 정수 ───────────────────────┐
            └──────────────────────┘                                          │
                 │ 정수 아닌 값이 들어옴                                       │
                 │  ├ 128개 미만, 64바이트 이하 ─▶ listpack                    ▼
                 │  └ 아니면 ─────────────────────────────────────────▶  HT (dict)
                 ▼                                                            ▲
            ┌──────────────────────┐                                          │
            │      listpack         │──── 129개째, 또는 64바이트 초과 원소 ────┘
            └──────────────────────┘
```

- 처음 만들 때는 SADD 인자 개수(`size_hint`)로 고릅니다. 한 번에 많이 넣으면 바로 dict로 만들고 그 크기로 미리 키웁니다(`setTypeCreate`, t_set.c:46).
- 기존 Set도 이번에 넣을 개수가 기준을 넘으면 미리 dict로 바꿉니다(`setTypeMaybeConvert`, t_set.c:61).

### 4-2. Hash

```
  HSET ───▶ listpack ──┬─ 이번 인자 중 필드 512개 초과 ──────────────┐
            (새 Hash는  ├─ 필드나 값 하나라도 64바이트 초과 ──────────┤  넣기 전 검사
             항상 여기)  ├─ listpack 전체 크기 상한 초과 예상 ─────────┤  hashTypeTryConversion
                       │                                            ▼
                       └─ 넣은 뒤 전체 필드 513개 ─────────────▶ HT (dict)
                          hashTypeSet                               │
                                                                    └─▶ 명령으로 돌아가는 길 없음
                                                                        (hashTypeConvert: "Not implemented")
                                                                        예외: RDB 로딩, 템플릿 설정
```

넣기 **전에** 인자를 보고 바꾸는 이유는, listpack에 넣었다가 곧바로 dict로 옮기는 낭비를 피하기 위해서입니다.

### 4-3. 거꾸로 가는 길

| 상황 | 작은 인코딩으로 돌아가나 | 근거 |
|---|---|---|
| SREM, HDEL로 원소를 지움 | 아니다. dict 안에서 dict 자체가 축소될 뿐 | t_set.c:263, t_hash.c:2708 |
| Hash가 dict가 된 뒤 명령을 실행 | 아니다 (hashTypeConvert에 HT에서 나가는 경로 없음) | t_hash.c:3432 |
| **RDB 로딩** (재시작, `DEBUG RELOAD`, `RESTORE`, 복제 full sync) | **그렇다.** 저장된 인코딩과 상관없이 크기 기준을 다시 적용. Hash는 512개 이하면 listpack, Set은 intset/listpack | rdb.c:3129-3133, 2951-2965 |
| SINTERSTORE 결과 | 전부 정수이고 512개 이하면 intset으로 저장 | t_set.c:1547 |
| SUNIONSTORE / SDIFFSTORE 결과 | 새 객체를 intset에서 시작해 보통 규칙으로 키움. SDIFFSTORE는 결과가 작아도 HT로 남을 수 있음 | t_set.c:1747, 1874-1878 |
| `SPOP key count`로 대부분을 꺼냄 | 남길 원소로 새 Set을 만들어서 작은 인코딩부터 다시 시작 | t_set.c:1007 |
| 키를 지우고 다시 만듦 | 새 객체이므로 처음부터 | |

---

## 5. 변환은 한 번에 일어난다

점진적 rehash와 달리 인코딩 변환은 **명령 하나 안에서 원소 전부를 옮깁니다**(`setTypeConvertAndExpand` t_set.c:522, `hashTypeConvertListpack` t_hash.c:3272).

그래도 tail latency 문제가 크지 않은 이유:

1. **N에 상한이 있습니다.** 변환은 기준값을 막 넘는 순간 일어나므로 옮기는 원소는 보통 129개나 513개 정도입니다.
2. **dict를 최종 크기로 먼저 키웁니다.** 빈 dict에 `dictExpand(cap)`을 하면 rehash 없이 바로 그 크기의 테이블이 됩니다(dict 발췌 [5-2]의 "옛 테이블이 비면 바로 교체"). 그래서 옮기는 동안 확장이나 rehash가 다시 일어나지 않습니다.

예외적으로 커지는 경우:

| 경우 | 무슨 일이 생기나 | 근거 |
|---|---|---|
| 새 키에 SADD로 원소 100만 개를 한 번에 | 처음부터 dict를 100만 개 크기로 한 번에 할당 | t_set.c:55 |
| HSET으로 필드 1만 개를 한 번에 | 현재 listpack을 dict로 바꾸고 1만 개 크기로 미리 키움 | t_hash.c:1954 |
| intset에 더 큰 정수가 들어옴 | 배열 전체를 넓은 폭으로 다시 씀 (최대 512개라 작음) | intset.c:178 |
| 기준값을 크게 올려 둠 | 변환 시점의 N 자체가 커져서 변환 한 번이 비싸짐. 그 전까지는 명령마다 O(N) | config.c |

명령 하나에 인자를 아주 많이 넣는 것 자체가 그 명령의 실행 시간을 늘리므로, 이 경우는 인코딩보다 "큰 명령"의 문제에 가깝습니다.

---

## 6. 큰 Hash/Set 안의 dict는 메인 키 공간과 다르다

이번 정리에서 가장 중요한 부분입니다. dict.c 코드는 같지만, 그 dict를 **누가 돌봐 주느냐**가 다릅니다.

| | 메인 키 공간 (`db->keys`) | 큰 Hash / Set 내부 dict |
|---|---|---|
| dictType | `dbDictType` (server.c:648) | `entryHashDictType` (t_hash.c:87) / `setDictType` (server.c:634) |
| 저장하는 것 | kvobj 포인터 | Entry(필드+값) 포인터 / 멤버 sds 포인터 |
| kvstore 소속 | 예 | 아니오. 객체 안에 dict 하나 |
| 요청 경로 rehash (조회/삽입/삭제마다 1버킷) | 예 | 예 |
| cron rehash (`databasesCron`, 100ms마다 1ms) | 예 | **아니오** |
| cron resize 검사 (`kvstoreTryResizeDicts`) | 예 | **아니오** (삽입/삭제 때만) |
| 확장 전 maxmemory 검사 (`resizeAllowed`) | 예 (`dictResizeAllowed`) | **아니오** (NULL) |
| fork 중 resize 보류 (`dict_can_resize`) | 적용 | 적용 (전역 변수 하나) |
| `INFO memory`의 `mem_overhead_db_hashtable_rehashing` | 포함 | 포함 안 됨 |

여기서 나오는 결과:

- **접근이 뜸한 큰 Hash는 rehash가 멈춰 있습니다.** 필드 수가 경계를 넘어서 rehash가 시작된 뒤 그 키에 아무도 접근하지 않으면, 옛 테이블과 새 테이블을 계속 함께 들고 있습니다. 메모리를 더 쓰고, 다음에 접근할 때 두 테이블을 볼 수 있습니다. 메인 키 공간이라면 cron이 1ms씩 마저 옮겨 줬을 상황입니다.
- **큰 Hash/Set의 확장은 maxmemory를 보지 않습니다.** 필드가 약 840만 개(2^23)를 넘는 순간 2^24 버킷 × 8바이트 = 128MiB 배열을 그 자리에서 할당합니다. 메인 키 공간처럼 "load factor 1.618까지 미루기"가 없습니다.
- **fork 중 정책은 똑같이 걸립니다.** BGSAVE 동안에는 큰 Hash도 load factor 4까지 확장이 미뤄지고, 진행 중이던 rehash도 멈춥니다.
- **HGET 한 번 = 해시 조회 두 번.** 키 공간에서 키를 찾고(`lookupKeyReadOrReply`), Hash 안에서 필드를 찾습니다(`hashTypeGetFromHashTable`). 두 dict가 각각 rehash 중이면 각각 한 걸음씩 진행합니다.

---

## 7. 직접 확인해 보기

05번 문서 9절처럼 테스트용 로컬 인스턴스(`enable-debug-command local`)에서만 하세요.

```bash
redis-server --port 6390 --enable-debug-command local --save "" --appendonly no
```

### 7-1. 인코딩이 바뀌는 순간 보기

```bash
redis-cli -p 6390 SADD s 1 2 3
```

```bash
redis-cli -p 6390 OBJECT ENCODING s
```

`intset`이 나옵니다. 정수가 아닌 값을 넣으면 `listpack`, 129개째를 넣으면 `hashtable`이 됩니다.

```bash
redis-cli -p 6390 SADD s a
```

```bash
redis-cli -p 6390 EVAL "for i=1,200 do redis.call('SADD',KEYS[1],'m'..i) end" 1 s
```

```bash
redis-cli -p 6390 SREM s a m1 m2 m3
```

SREM 뒤에도 `OBJECT ENCODING s`는 `hashtable`입니다. 각 단계에서 `MEMORY USAGE s`도 같이 보면 인코딩이 바뀔 때 메모리가 크게 뛰는 것을 볼 수 있습니다.

### 7-2. 큰 Hash의 rehash가 접근 없이는 멈춰 있는 것 보기

필드 1,025개를 넣으면, 513개째에서 dict(버킷 1,024개)로 바뀌고 1,025개째에서 2,048개로 확장이 시작됩니다.

```bash
redis-cli -p 6390 EVAL "for i=1,1025 do redis.call('HSET',KEYS[1],'f'..i,'v') end" 1 h
```

```bash
redis-cli -p 6390 DEBUG HTSTATS-KEY h
```

`Hash table 1 stats (rehashing target)`이 보이면 rehash 중입니다. 몇 초 기다렸다가 다시 봐도 그대로입니다. cron은 이 dict를 돌보지 않기 때문입니다. 이제 이 키에 접근하면 이사가 끝납니다.

```bash
redis-cli -p 6390 EVAL "for i=1,2000 do redis.call('HGET',KEYS[1],'f'..i) end" 1 h
```

같은 실험을 메인 키 공간에서 하면(05번 문서 9-1절, `activerehashing yes`) 접근하지 않아도 cron이 옮겨 줍니다.

---

## 8. Redis 8.0 vs unstable (t_set.c, t_hash.c)

| 위치 | 8.0 | unstable |
|---|---|---|
| 기준값 기본값 | 512 / 128 / 64, Hash 512 / 64 | 같음 |
| Set 인코딩 선택과 변환 (`setTypeCreate`, `setTypeAddAux`, `setTypeConvertAndExpand`) | 같은 로직 | 같은 로직. dict 삽입 API 이름(`dictFindPositionForInsert` → `dictFindLink`)과 메모리 추적 코드만 다름 |
| Hash dict 타입 | `mstrHashDictType`: 필드는 mstr 키, 값은 따로 저장 (`no_value=0`) | `entryHashDictType`: 필드+값을 Entry 하나에 (`no_value=1`) |
| Hash 템플릿 인코딩 (`TMPL_LP`, `TMPL_ARRAY`), HIMPORT | 없음 | 있음 (기본 꺼짐) |
| t_hash.c 길이 | 4026줄 | 6910줄. 템플릿 관련 줄(이름에 tmpl/template이 들어간 줄)만 850여 줄 |

이 문서의 결론(변환 조건, 한 방향 변환, 내부 dict는 cron 대상이 아님, maxmemory 검사 없음)은 두 버전에서 같습니다.

---

## 9. 이번에 답할 수 있게 된 질문

| 질문 | 답 | 근거 |
|---|---|---|
| 작은 Hash/Set은 왜 dict를 안 쓰나? | 연속 메모리가 캐시에 유리하고 메모리를 훨씬 적게 씀. N이 작으면 O(N)도 충분히 빠름 | 3절 |
| 언제 dict로 바뀌나? | Set: 128개(정수만이면 512개) 또는 64바이트 초과. Hash: 512개 또는 64바이트 초과 | 2, 4절 |
| 바뀌는 순간 멈추나? | 원소 전부를 한 번에 옮기지만, 보통 수백 개라 짧음. dict는 최종 크기로 미리 할당 | 5절 |
| 원소를 지우면 다시 작아지나? | 명령으로는 아니다 (SINTERSTORE 결과, SPOP count 예외만). RDB 로딩 때는 크기 기준을 다시 적용 | 4-3절 |
| 기준값을 크게 올리면? | 메모리는 줄지만 명령마다 O(N). 그 키를 쓰는 모든 명령이 느려짐 | 3-2절 |
| 큰 Hash 안의 dict도 cron이 rehash해 주나? | 아니다. 그 키에 접근할 때만 진행 | 6절, 7-2절 |
| 큰 Hash의 확장도 maxmemory를 보나? | 아니다. resizeAllowed가 없음 | 6절 |

---

## 10. 읽는 순서

1. 이 문서 2절(표)과 4절(변환 그림)
2. [hashset/t_set_발췌.c](hashset/t_set_발췌.c) [1] → [2] → [3] → [4]
3. 같은 파일 [부록 A] intset (이진 탐색과 memmove)
4. [hashset/t_hash_발췌.c](hashset/t_hash_발췌.c) [1] → [3] → [4] → [5] → [6]
5. 이 문서 6절을 05번 문서 7, 8절과 나란히 보기
6. 7절 실험

### 다음에 보면 좋은 것

- **listpack.c**: `lpFind`가 원소를 어떻게 건너뛰는지, 끝에 붙일 때 realloc이 어떻게 일어나는지
- **t_zset.c**: Sorted Set은 커지면 dict와 skiplist를 **함께** 씁니다. 조회는 dict(O(1)), 순위와 범위는 skiplist(O(log N))
- **lazyfree.c**: 큰 Hash/Set을 지울 때 원소 수에 비례하는 해제 비용과, 그것을 백그라운드 스레드로 넘기는 방법 (`UNLINK`, `lazyfree-lazy-user-del`)
