/* ======================================================================
 * [해설판] dict.c 발췌 — 조회, 확장/축소, 점진적 rehash
 * 원본: https://github.com/redis/redis/blob/unstable/src/dict.c (2397줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 2397줄 중 이번 주제에 필요한 함수만 골라
 *   "읽는 순서대로" 다시 배치했습니다.
 *   - 각 함수 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다. 뺀 것 외에는 원본과 같은지 확인했습니다.
 *   - 뺀 것: 이터레이터 대부분, 랜덤 키, SCAN, defrag, 통계, 테스트 코드
 *
 * ▶ 먼저 dict.h의 struct dict 그림을 보고 오세요. ht_table[2]와 rehashidx만 알면 됩니다.
 *
 * ▶ 목차
 *   [0] 전역 설정과 엔트리 구조
 *   [1] 해시 함수                 : SipHash + 무작위 시드
 *   [2] 조회                      : dictFind → dictFindLink → dictFindLinkInternal  ★
 *   [3] 삽입                      : 새 키는 어디에 들어가나, 확장 검사는 언제 하나
 *   [4] 확장 결정                 : load factor 1, fork 중엔 4  ★
 *   [5] 리사이즈 시작             : 새 테이블 할당 (tail latency 지점 ①)
 *   [6] 점진적 rehash             : 요청마다 1버킷 + cron에서 1ms  ★★
 *   [7] 삭제와 축소
 *   [8] rehash 일시 정지          : safe iterator
 *   [9] 정책 스위치               : fork 시작/종료 때 바뀜
 *
 * ▶ 처음 읽을 때는 [2] [4] [6]만 보세요.
 * ====================================================================== */


/* ######################################################################
 * [0] 전역 설정과 엔트리 구조 — dict.c L34~L66
 * ###################################################################### */

/* Using dictSetResizeEnabled() we make possible to disable
 * resizing and rehashing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to DICT_RESIZE_AVOID, not all
 * resizes are prevented:
 *  - A hash table is still allowed to expand if the ratio between the number
 *    of elements and the buckets >= dict_force_resize_ratio.
 *  - A hash table is still allowed to shrink if the ratio between the number
 *    of elements and the buckets <= 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio). */
static redisAtomic dictResizeEnable dict_can_resize = DICT_RESIZE_ENABLE;  /* ▶ 전역 하나. 모든 dict가 공유. server.c가 fork 상태에 따라 바꿈 [9] */
static const unsigned int dict_force_resize_ratio = 4;  /* ▶ AVOID(fork 중)여도 load factor가 4 이상이면 강제로 확장 */

/* -------------------------- types ----------------------------------------- */
/* ▶ 체인(연결 리스트)의 노드. next가 맨 앞이라 아래 dictEntryNoValue와 앞부분 모양이 같습니다.
 *   그래서 next/key를 다룰 때 두 타입을 구분하지 않아도 됩니다 (아래 static_assert가 보장).
 *   해시값은 저장하지 않습니다. 엔트리마다 8바이트를 아끼는 대신, 확장할 때 해시를 다시 계산합니다 [6-4]. */
struct dictEntry {
    struct dictEntry *next;  /* Must be first */
    void *key;               /* Must be second */
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
};

typedef struct dictEntryNoValue {
    dictEntry *next; /* Must be first */
    void *key;       /* Must be second */
} dictEntryNoValue;

static_assert(offsetof(dictEntry, next) == offsetof(dictEntryNoValue, next), "dictEntry & dictEntryNoValue next not aligned");
static_assert(offsetof(dictEntry, key) == offsetof(dictEntryNoValue, key), "dictEntry & dictEntryNoValue key not aligned");


/* ######################################################################
 * [1] 해시 함수 — dict.c L102~L118, L1807~L1810
 * ###################################################################### */

/* -------------------------- hash functions -------------------------------- */

/* ▶ 시드(16바이트)는 서버 시작 때 무작위로 만듭니다 (server.c main: getRandomBytes → dictSetHashFunctionSeed).
 *   시드를 모르면 "같은 버킷에 몰리는 키"를 미리 만들어 보낼 수 없습니다 (hash flooding 방어).
 *   해시가 고르게 퍼진다는 전제가 깨지면 체인 하나에 키가 몰려서 조회가 O(N)이 됩니다.
 *   평균 O(1)은 "좋은 해시 함수"를 전제로 한 이야기입니다. */
static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key, len, dict_hash_function_seed);  /* ▶ 비용은 키 길이에 비례. 키 개수 N과는 무관 */
}

/* Compiler inlines this for internal calls within dict.c (verified with -O3). */
uint64_t dictGetHash(dict *d, const void *key) {
    return d->type->hashFunction(key);  /* ▶ 메인 키 공간이면 server.c dictSdsHash → dictGenHashFunction */
}


/* ######################################################################
 * [2] 조회 — 평균 O(1)의 본체
 *   메인 키 공간 경로: GET → lookupKeyRead → lookupKey → dbFindByLink
 *                       → kvstoreDictFindLink → dictFindLink → dictFindLinkInternal
 *   (dictFind는 같은 일을 하고 엔트리를 돌려주는 얇은 포장입니다)
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] dictFind / dictFindLink — dict.c L805~L809, L863~L869
 * ====================================================================== */
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntryLink link = dictFindLink(d, key, NULL);
    return (link) ? *link : NULL;
}

dictEntryLink dictFindLink(dict *d, const void *key, dictEntryLink *bucket) {
    if (bucket) *bucket = NULL;
    if (unlikely(dictSize(d) == 0))  /* ▶ 빈 dict면 해시 계산도 안 함 */
        return NULL;
    
    return dictFindLinkInternal(d, key, bucket);
}

/* ======================================================================
 * ▶ [2-2] dictFindLinkInternal — dict.c L760~L803   ★ 조회의 전부 ★
 *
 *   ① 해시 계산          O(키 길이)
 *   ② 버킷 번호 = hash & mask   O(1), 배열 인덱싱
 *   ③ rehash 중이면 버킷 하나 옮겨 주기 (조회가 rehash 비용을 나눠 냄)
 *   ④ 체인 따라가며 비교  O(체인 길이) = 평균 O(load factor)
 *
 *   키가 N개로 늘어도 ①②③은 N과 무관하고, ④는 load factor에만 달려 있습니다.
 *   load factor는 확장 규칙([4]) 때문에 1을 크게 넘지 않으므로(확장 직후 약 0.5) ④도 상수입니다.
 *   이것이 "데이터가 늘어도 평균 조회 성능이 유지되는 이유"의 코드 버전입니다.
 * ====================================================================== */
/* Finds a given key. Like dictFindLink(), yet search bucket even if dict is empty. 
 * 
 * Returns dictEntryLink reference if found. Otherwise, return NULL.
 * 
 * bucket - return pointer to bucket that the key was mapped. unless dict is empty.
 */
static dictEntryLink dictFindLinkInternal(dict *d, const void *key, dictEntryLink *bucket) {
    dictCmpCache cmpCache = {0};
    dictEntryLink link;
    uint64_t idx;
    int table;
    
    if (bucket) {
        *bucket = NULL;
    } else {
        /* If dict is empty and no need to find bucket, return NULL */
        if (dictSize(d) == 0) return NULL; 
    }

    const uint64_t hash = dictGetHash(d, key);  /* ▶ ① 해시 계산 (SipHash, 키 길이만큼) */
    idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]);  /* ▶ ② 옛(0번) 테이블 기준 버킷 번호 */
    keyCmpFunc cmpFunc = dictGetCmpFunc(d);

    /* ▶ ③ rehash 중이면 여기서 버킷 하나를 옮깁니다. "조회하는 쪽이 이사 비용을 조금씩 나눠 낸다."
     *   방금 계산한 idx 칸을 옮기면 곧바로 그 칸을 읽으므로 CPU 캐시에도 유리합니다 [6-1]. */
    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d,idx);

    int tables = (dictIsRehashing(d)) ? 2 : 1;  /* ▶ rehash 중이면 두 테이블을 다 봐야 할 수 있음 */
    for (table = 0; table < tables; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;  /* ▶ 옛 테이블의 이 칸은 이미 새 테이블로 옮겨짐 → 건너뜀 */
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);  /* ▶ 테이블마다 크기가 달라서 버킷 번호를 다시 계산 */

        link = &(d->ht_table[table][idx]);
        if (bucket) *bucket = link;
        /* ▶ ④ 체인 따라가기. 비교 횟수 = 이 버킷의 체인 길이.
         *   있는 키를 찾을 때 평균 비교 ≈ 1 + load factor/2, 없는 키면 ≈ load factor (해시가 고르다는 전제). */
        while(link && *link) {
            const void *visitedKey = dictStoredKey2Key(d, dictGetKey(*link));

            if (key == visitedKey || cmpFunc( &cmpCache, key, visitedKey))  /* ▶ 포인터가 같으면 바로 일치, 아니면 길이 비교 → memcmp (server.c dictSdsCompareKV) */
                return link;

            link = dictGetNextLink(*link);  /* ▶ 키를 직접 가리키는 칸이면 next가 없으므로 NULL → 체인 끝 [2-3] */
        }
    }
    return NULL;  /* ▶ 못 찾음. 없는 키는 rehash 중이면 두 테이블을 모두 확인한 뒤에야 확정 */
}

/* ======================================================================
 * ▶ [2-3] 체인의 칸 해석하기 — dict.c L136~L140, L1033~L1039, L1077~L1081
 *   메인 키 공간(no_value=1)에서는 칸에 든 포인터가 엔트리일 수도, kvobj(키) 자체일 수도 있습니다.
 *   하위 비트를 보고 구분합니다 (dict.h 맨 위 pointer tagging 설명).
 * ====================================================================== */
/* Returns 1 if the entry pointer is a pointer to a key, rather than to an
 * allocated entry. Returns 0 otherwise. */
static inline int entryIsKey(const dictEntry *de) {
    return ((uintptr_t)de & (ENTRY_PTR_IS_ODD_KEY | ENTRY_PTR_IS_EVEN_KEY));
}

void *dictGetKey(const dictEntry *de) {
    /* if entryIsKey() */
    if ((uintptr_t)de & ENTRY_PTR_IS_ODD_KEY) return (void *) de;
    if ((uintptr_t)de & ENTRY_PTR_IS_EVEN_KEY) return decodeMaskedPtr(de);    
    /* Regular entry */
    return de->key;
}

static dictEntryLink dictGetNextLink(dictEntry *de) {
    if (entryIsKey(de)) return NULL;
    /* Must come after entryIsKey() check */
    return &de->next;
}


/* ######################################################################
 * [3] 삽입 — 새 키는 어디에 들어가나
 *   메인 키 공간의 SET(새 키): lookupKeyWriteWithLink → setKeyByLink → dbAddByLink → dbAddInternal
 *     → kvstoreDictSetAtLink → dictSetKeyAtLink (newItem=1)
 *     (dbAdd는 link=NULL을 넘겨서 "조회 때 받아 둔 버킷 재사용"이 없음. DEBUG POPULATE 등에서 사용)
 *   일반 dict: dictAdd → dictAddRaw → dictFindLinkForInsert + dictInsertKeyAtLink
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] dictSetKeyAtLink — dict.c L881~L920
 *   메인 키 공간은 먼저 조회(dbFindByLink)해서 "없으면 넣을 버킷"을 받아 두고, 여기서 넣습니다.
 *   넣기 직전에 확장 검사를 하고, 테이블이 바뀌었으면 버킷을 다시 찾습니다.
 * ====================================================================== */
void dictSetKeyAtLink(dict *d, void *key __stored_key, dictEntryLink *link, int newItem) {
    dictEntryLink dummy = NULL;
    if (link == NULL) link = &dummy;
    void *addedKey = (d->type->keyDup) ? d->type->keyDup(d, key) : key;
    
    if (newItem) {
        signed char snap[2] = {d->ht_size_exp[0], d->ht_size_exp[1] };

        /* Make room if needed for the new key */
        dictExpandIfNeeded(d);  /* ▶ ★ 확장 검사 [4]. 확장이 시작되면 새 키는 새 테이블로 가야 하므로 아래에서 버킷을 다시 찾음 */
        
        /* Lookup key's link if tables reallocated or if given link is set to NULL */
        if (snap[0] != d->ht_size_exp[0] || snap[1] != d->ht_size_exp[1] || *link == NULL) {
            dictEntryLink bucket;
            /* Bypass dictFindLink() to search bucket even if dict is empty!!! */
            *link = dictFindLinkInternal(d, dictStoredKey2Key(d, key), &bucket);
            assert(bucket != NULL);
            assert(*link == NULL);
            *link = bucket; /* On newItem the link should be the bucket */
        }
        dictInsertKeyAtLink(d, addedKey, *link);
        return;
    } 
    /* ▶ [생략] 원본 L904~L919: 기존 엔트리의 키만 바꾸는 경우(newItem=0) */
}

/* ======================================================================
 * ▶ [3-2] dictAddRaw / dictFindLinkForInsert — dict.c L531~L541, L1748~L1783
 *   일반 dict(Hash, Set 타입의 큰 객체 등)의 삽입 경로. 하는 일은 [3-1]과 같습니다:
 *   rehash 한 걸음 → 확장 검사 → 이미 있는 키인지 확인 → 넣을 버킷 반환
 * ====================================================================== */
dictEntry *dictAddRaw(dict *d, void *key __stored_key, dictEntry **existing)
{
    /* Get the position for the new key or NULL if the key already exists. */
    void *position = dictFindLinkForInsert(d, dictStoredKey2Key(d, key), existing);
    if (!position) return NULL;

    /* Dup the key if necessary. */
    if (d->type->keyDup) key = d->type->keyDup(d, key);

    return dictInsertKeyAtLink(d, key, position);
}

dictEntryLink dictFindLinkForInsert(dict *d, const void *key, dictEntry **existing) {
    unsigned long idx, table;
    dictCmpCache cmpCache = {0};
    dictEntry *he;
    uint64_t hash = dictGetHash(d, key);
    if (existing) *existing = NULL;
    idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d,idx);  /* ▶ 삽입도 rehash 한 걸음 */

    /* Expand the hash table if needed */
    _dictExpandIfNeeded(d);  /* ▶ ★ 확장 검사 [4] */
    keyCmpFunc cmpFunc = dictGetCmpFunc(d);

    for (table = 0; table <= 1; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;  /* ▶ 조회와 같은 건너뛰기 */
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Search if this slot does not already contain the given key */
        he = d->ht_table[table][idx];
        while(he) {
            const void *he_key = dictStoredKey2Key(d, dictGetKey(he));            
            if (key == he_key || cmpFunc(&cmpCache, key, he_key)) {
                if (existing) *existing = he;
                return NULL;
            }
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d)) break;
    }

    /* If we are in the process of rehashing the hash table, the bucket is
     * always returned in the context of the second (new) hash table. */
    dictEntry **bucket = &d->ht_table[dictIsRehashing(d) ? 1 : 0][idx];  /* ▶ rehash 중이면 새 키는 항상 새 테이블로 → 옛 테이블은 줄어들기만 함 */
    return bucket;
}

/* ======================================================================
 * ▶ [3-3] dictInsertKeyAtLink — dict.c L547~L580
 *   체인 맨 앞에 붙이므로 O(1). 체인 끝까지 갈 필요가 없습니다.
 * ====================================================================== */
dictEntry *dictInsertKeyAtLink(dict *d, void *key __stored_key, dictEntryLink link) {
    dictEntryLink bucket = link; /* It's a bucket, but the API hides that. */
    dictEntry *entry;
    /* If rehashing is ongoing, we insert in table 1, otherwise in table 0.
     * Assert that the provided bucket is the right table. */
    int htidx = dictIsRehashing(d) ? 1 : 0;  /* ▶ rehash 중이면 새 테이블(1) */
    assert(bucket >= &d->ht_table[htidx][0] &&
           bucket <= &d->ht_table[htidx][DICTHT_SIZE_MASK(d->ht_size_exp[htidx])]);
    if (d->type->no_value) {
        if (!*bucket) {
            /* We can store the key directly in the destination bucket without 
             * allocating dictEntry.
             */
            entry = encodeEntryKey(d, key);  /* ▶ 빈 버킷: 엔트리 없이 kvobj 포인터를 칸에 직접 */
            assert(entryIsKey(entry));
        } else {
            /* Allocate an entry without value. */
            entry = createEntryNoValue(key, *bucket);  /* ▶ 충돌: {next,key} 엔트리를 만들어 체인 맨 앞에 */
        }
    } else {
        /* Allocate the memory and store the new entry.
         * Insert the element in top, with the assumption that in a database
         * system it is more likely that recently added entries are accessed
         * more frequently. */
        entry = zmalloc(sizeof(*entry));
        assert(entryIsNormal(entry)); /* Check alignment of allocation */
        entry->key = key;
        entry->next = *bucket;
    }
    *bucket = entry;
    d->ht_used[htidx]++;  /* ▶ 키 수 증가 → 다음 삽입 때 load factor 검사에 반영 */

    return entry;
}


/* ######################################################################
 * [4] 확장 결정 — 언제, 얼마나 키우나
 *   "키 수 ≥ 버킷 수"가 되면 버킷을 2배로 → 평균 체인 길이가 다시 0.5로 내려감
 *   이 규칙 하나가 "N이 커져도 체인 길이는 상수"를 보장합니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [4-1] _dictExpandIfNeeded / dictExpandIfNeeded — dict.c L1677~L1683, L1646~L1675
 * ====================================================================== */
/* Expand the hash table if needed (OK=Expanded, ERR=Not expanded) */
static int _dictExpandIfNeeded(dict *d) {
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return DICT_ERR;
    
    return dictExpandIfNeeded(d);
}

/* Returning DICT_OK indicates a successful expand or the dictionary is undergoing rehashing, 
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that expand has not been triggered (may be try shrinking?)*/
int dictExpandIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;  /* ▶ rehash 중에는 다음 확장을 하지 않음 (테이블은 최대 2개) */

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) {
        dictExpand(d, DICT_HT_INITIAL_SIZE);  /* ▶ 첫 삽입: 버킷 4개로 시작 */
        return DICT_OK;
    }

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    /* ▶ 확장 조건
     *   ENABLE        : 키 수 ≥ 버킷 수        (load factor ≥ 1)
     *   AVOID(fork 중): 키 수 ≥ 버킷 수 × 4    (load factor ≥ 4. 체인이 꽤 길어진 뒤에야 확장)
     *   FORBID        : 확장 안 함 */
    if ((can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] >= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] >= dict_force_resize_ratio * DICTHT_SIZE(d->ht_size_exp[0])))
    {
        if (dictTypeResizeAllowed(d, d->ht_used[0] + 1))  /* ▶ 큰 메모리를 잡아도 되는지 (server.c dictResizeAllowed: maxmemory 검사) */
            dictExpand(d, d->ht_used[0] + 1);  /* ▶ used+1 이상인 2의 거듭제곱 → 보통 지금의 2배 (AVOID로 밀렸다면 8배) */
        return DICT_OK;
    }
    return DICT_ERR;
}

/* ======================================================================
 * ▶ [4-2] dictTypeResizeAllowed / _dictNextExp — dict.c L1636~L1644, L1735~L1742
 * ====================================================================== */
/* Because we may need to allocate huge memory chunk at once when dict
 * resizes, we will check this allocation is allowed or not if the dict
 * type has resizeAllowed member function. */
static int dictTypeResizeAllowed(dict *d, size_t size) {
    if (d->type->resizeAllowed == NULL) return 1;
    return d->type->resizeAllowed(
                    DICTHT_SIZE(_dictNextExp(size)) * sizeof(dictEntry*),  /* ▶ 새 테이블 바이트 수 = 버킷 수 × 포인터 8바이트 */
                    (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]));  /* ▶ 현재 load factor */
}

/* Our hash table capability is a power of two */
static signed char _dictNextExp(unsigned long size)
{
    if (size <= DICT_HT_INITIAL_SIZE) return DICT_HT_INITIAL_EXP;
    if (size >= LONG_MAX) return (8*sizeof(long)-1);

    return 8*sizeof(long) - __builtin_clzl(size-1);  /* ▶ size-1의 최상위 비트 위치 = size 이상인 가장 작은 2^exp의 지수 */
}


/* ######################################################################
 * [5] 리사이즈 시작 — 새 테이블을 만들고 rehash 상태로 전환
 * ###################################################################### */

/* ======================================================================
 * ▶ [5-1] dictExpand / _dictExpand — dict.c L314~L317, L306~L312
 * ====================================================================== */
/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

int _dictExpand(dict *d, unsigned long size, int* malloc_failed) {
    /* the size is invalid if it is smaller than the size of the hash table 
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size || DICTHT_SIZE(d->ht_size_exp[0]) >= size)
        return DICT_ERR;
    return _dictResize(d, size, malloc_failed);
}

/* ======================================================================
 * ▶ [5-2] _dictResize — dict.c L236~L304
 *   키는 한 개도 옮기지 않습니다. 빈 새 배열을 ht_table[1]에 걸고 rehashidx=0으로 바꾸는 것까지만.
 *   실제 이사는 [6]에서 조금씩 합니다.
 * ====================================================================== */
/* Resize or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if resize was performed, and DICT_ERR if skipped. */
int _dictResize(dict *d, unsigned long size, int* malloc_failed)
{
    if (malloc_failed) *malloc_failed = 0;

    /* We can't rehash twice if rehashing is ongoing. */
    assert(!dictIsRehashing(d));  /* ▶ 테이블은 최대 2개. rehash 중엔 새 resize 불가 */

    /* the new hash table */
    dictEntry **new_ht_table;
    unsigned long new_ht_used;
    signed char new_ht_size_exp = _dictNextExp(size);

    /* Detect overflows */
    size_t newsize = DICTHT_SIZE(new_ht_size_exp);
    if (newsize < size || newsize * sizeof(dictEntry*) < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp[0]) return DICT_ERR;

    /* ▶ ★ tail latency 지점 ①: 새 버킷 배열을 "한 번에" 할당 ★
     *   크기 × 8바이트. 예) 키가 2^26(약 6,700만) 개가 되는 순간 2^27 버킷 = 1GiB를 한 번에 요청합니다.
     *   이때부터 rehash가 끝날 때까지 옛 배열과 새 배열이 동시에 메모리에 있습니다.
     *   그래서 확장 전에 dictResizeAllowed로 maxmemory를 넘지 않는지 먼저 봅니다 [4-1].
     *   (넘으면 확장 대신 키 eviction이 몰려서 일어날 수 있기 때문) */
    /* Allocate the new hash table and initialize all pointers to NULL */
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize*sizeof(dictEntry*));
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed)
            return DICT_ERR;
    } else
        new_ht_table = zcalloc(newsize*sizeof(dictEntry*));

    new_ht_used = 0;

    /* Prepare a second hash table for incremental rehashing.
     * We do this even for the first initialization, so that we can trigger the
     * rehashingStarted more conveniently, we will clean it up right after. */
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;  /* ▶ rehash 시작. 옛 배열 0번 칸부터 옮김 */
    if (d->type->rehashingStarted) d->type->rehashingStarted(d);  /* ▶ kvstore가 이 dict를 "rehash 중" 목록에 넣음 (kvstore 발췌 [3]) */
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, DICTHT_SIZE(d->ht_size_exp[1]));

    /* ▶ 옛 테이블이 비어 있으면 옮길 게 없으니 바로 교체하고 끝 (첫 생성도 이 경로) */
    /* Is this the first initialization or is the first hash table empty? If so
     * it's not really a rehashing, we can just set the first hash table so that
     * it can accept keys. */
    if (d->ht_table[0] == NULL || d->ht_used[0] == 0) {
        if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);
        if (d->type->bucketChanged)
            d->type->bucketChanged(d, -(long long)DICTHT_SIZE(d->ht_size_exp[0]));
        if (d->ht_table[0]) zfree(d->ht_table[0]);
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        _dictReset(d, 1);
        d->rehashidx = -1;
        return DICT_OK;
    }

    /* ▶ 명령어 테이블 같은 작은 dict만 해당. 메인 키 공간은 점진적으로 옮깁니다. */
    /* Force a full rehashing of the dictionary */
    if (d->type->force_full_rehash) {
        while (dictRehash(d, 1000)) {
            /* Continue rehashing */
        }
    }
    return DICT_OK;
}


/* ######################################################################
 * [6] 점진적 rehash   ★★ 이번 주제의 핵심 ★★
 *
 *   옛 테이블 → 새 테이블 이사를 한 번에 하지 않고 두 경로로 조금씩 합니다.
 *     (a) 요청이 낼 때 : 조회/삽입/삭제마다 버킷 1개           [6-1] [6-2] [6-3]
 *     (b) 주기적으로   : serverCron(기본 100ms마다, 부하 무관)에서 최대 1ms     [6-6]
 *   (a)만 있으면 요청이 없는 dict는 영원히 테이블 2개를 들고 있게 되므로 (b)가 필요합니다.
 *
 *   Java HashMap은 put() 한 번에서 resize()가 전체 엔트리를 옮깁니다.
 *   Redis는 그 비용을 수많은 요청에 잘게 나눠서, 요청 하나가 떠안는 최악의 비용을 줄입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [6-1] _dictRehashStepIfNeeded — dict.c L1720~L1733
 *   조회/삽입/삭제가 부르는 진입점.
 * ====================================================================== */
static void _dictRehashStepIfNeeded(dict *d, uint64_t visitedIdx) {
    if ((!dictIsRehashing(d)) || (d->pauserehash != 0))  /* ▶ rehash 중이 아니거나 일시 정지면 아무것도 안 함 */
        return;
    /* rehashing not in progress if rehashidx == -1 */
    if ((long)visitedIdx >= d->rehashidx && d->ht_table[0][visitedIdx]) {  /* ▶ 이번 요청이 본 칸이 아직 안 옮겨졌고 비어 있지 않으면 */
        /* If we have a valid hash entry at `idx` in ht0, we perform
         * rehash on the bucket at `idx` (being more CPU cache friendly) */
        _dictBucketRehash(d, visitedIdx);  /* ▶ 그 칸을 옮김. rehashidx는 그대로 */
    } else {
        /* If the hash entry is not in ht0, we rehash the buckets based
         * on the rehashidx (not CPU cache friendly). */
        dictRehash(d,1);  /* ▶ 아니면 rehashidx부터 버킷 1개 (빈 칸은 최대 10개까지만 건너뜀) */
    }
}

/* ======================================================================
 * ▶ [6-2] _dictBucketRehash — dict.c L475~L495
 * ====================================================================== */
/* Performs rehashing on a single bucket. */
int _dictBucketRehash(dict *d, uint64_t idx) {
    if (d->pauserehash != 0) return 0;
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if (can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32. */
    if (can_resize == DICT_RESIZE_AVOID &&
        ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
         (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1)))
    {
        return 0;
    }
    rehashEntriesInBucketAtIndex(d, idx);  /* ▶ 이 칸의 키들만 옮김 */
    dictCheckRehashingCompleted(d);  /* ▶ 옛 테이블이 비었으면 마무리 [6-5] */
    return 1;
}

/* ======================================================================
 * ▶ [6-3] dictRehash — dict.c L397~L437   ★
 *   n = 옮길 버킷 수. 요청 경로는 n=1, cron 경로는 n=100.
 * ====================================================================== */
/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */  /* ▶ 빈 칸 방문 상한. n=1이면 10칸 */
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if (can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;  /* ▶ fork된 자식 안이면 금지 */
    /* ▶ fork 중(AVOID)이면 진행 중이던 rehash도 멈춥니다. 단, 새 테이블이 옛 테이블의 4배 이상이면
     *   (= load factor 4 이상에서 강제 확장된 경우) 계속합니다. 반대로 fork 전에 시작된 2배 확장은
     *   여기서 멈추고, rehash 중이라 다음 확장도 못 하므로 자식이 끝날 때까지 체인이 계속 길어집니다. [4-1]의 확장 조건과 짝을 이룹니다. */
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32. */
    if (can_resize == DICT_RESIZE_AVOID &&
        ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
         (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1)))
    {
        return 0;
    }

    while(n-- && d->ht_used[0] != 0) {  /* ▶ 버킷 n개를 옮기거나 옛 테이블이 빌 때까지 */
        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        /* ▶ ★ 빈 칸만 계속 나오면 일이 끝없이 늘어날 수 있으므로 n*10칸에서 멈춤 ★
         *   대량 삭제 뒤처럼 빈 칸이 많아도 호출 한 번의 비용에 상한이 생깁니다 = latency 보호 */
        while(d->ht_table[0][d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        /* Move all the keys in this bucket from the old to the new hash HT */
        rehashEntriesInBucketAtIndex(d, d->rehashidx);  /* ▶ 체인 전체를 옮김. 체인이 길면 이 한 걸음도 길어짐 */
        d->rehashidx++;
    }

    return !dictCheckRehashingCompleted(d);  /* ▶ 다 옮겼으면 옛 배열 해제하고 교체 [6-5] */
}

/* ======================================================================
 * ▶ [6-4] rehashEntriesInBucketAtIndex — dict.c L335~L378
 *   버킷 하나의 체인을 통째로 새 테이블로 옮깁니다.
 * ====================================================================== */
/* Helper function for `dictRehash` and `dictBucketRehash` which rehashes all the keys
 * in a bucket at index `idx` from the old to the new hash HT. */
static void rehashEntriesInBucketAtIndex(dict *d, uint64_t idx) {
    dictEntry *de = d->ht_table[0][idx];
    uint64_t h;
    dictEntry *nextde;
    while (de) {
        nextde = dictGetNext(de);
        void *storedKey = dictGetKey(de);
        /* ▶ 확장: 새 테이블은 버킷 번호에 쓰는 비트가 더 많아서 해시를 "다시 계산"합니다 (키마다 SipHash 1번).
         *   엔트리에 해시값을 저장하지 않기 때문입니다 [0].
         *   축소: 새 버킷 번호 = 옛 번호의 하위 비트. 해시 계산 없이 마스크만 씌웁니다. */
        /* Get the index in the new hash table */
        if (d->ht_size_exp[1] > d->ht_size_exp[0]) {
            const void *key = dictStoredKey2Key(d, storedKey);
            h = dictGetHash(d, key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
        } else {
            /* We're shrinking the table. The tables sizes are powers of
             * two, so we simply mask the bucket index in the larger table
             * to get the bucket index in the smaller table. */
            h = idx & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
        }
        if (d->type->no_value) {  /* ▶ 메인 키 공간(no_value=1)의 경우 */
            if (!d->ht_table[1][h]) {
                /* The destination bucket is empty, allowing the key to be stored 
                 * directly without allocating a dictEntry. If an old entry was 
                 * previously allocated, free its memory. */                
                if (!entryIsKey(de)) zfree(decodeMaskedPtr(de));
                
                de = encodeEntryKey(d, storedKey);  /* ▶ 목적지가 비었으면 엔트리를 버리고 kvobj 포인터만 */
                
            } else if (entryIsKey(de)) {
                /* We don't have an allocated entry but we need one. */
                de = createEntryNoValue(storedKey, d->ht_table[1][h]);  /* ▶ 목적지에 이미 있으면 엔트리를 새로 만들어 앞에 붙임 */
            } else {
                dictSetNext(de, d->ht_table[1][h]);
            }
        } else {
            dictSetNext(de, d->ht_table[1][h]);
        }
        d->ht_table[1][h] = de;  /* ▶ 새 테이블 체인 맨 앞에 붙임 (O(1)) */
        d->ht_used[0]--;  /* ▶ 키 수를 옛 → 새로 이동 */
        d->ht_used[1]++;
        de = nextde;
    }
    d->ht_table[0][idx] = NULL;  /* ▶ 옛 칸 비움 */
}

/* ======================================================================
 * ▶ [6-5] dictCheckRehashingCompleted — dict.c L380~L395
 * ====================================================================== */
/* This checks if we already rehashed the whole table and if more rehashing is required */
static int dictCheckRehashingCompleted(dict *d) {
    if (d->ht_used[0] != 0) return 0;  /* ▶ 완료 판단은 rehashidx가 아니라 "옛 테이블 키 수 = 0" */
    
    if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);  /* ▶ kvstore가 "rehash 중" 목록에서 뺌 */
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, -(long long)DICTHT_SIZE(d->ht_size_exp[0]));
    zfree(d->ht_table[0]);  /* ▶ ★ tail latency 지점 ②: 큰 옛 배열을 한 번에 해제. 크면 OS에 돌려주는 비용이 들 수 있음 */
    /* Copy the new ht onto the old one */
    d->ht_table[0] = d->ht_table[1];  /* ▶ 새 테이블이 0번이 됨 */
    d->ht_used[0] = d->ht_used[1];
    d->ht_size_exp[0] = d->ht_size_exp[1];
    _dictReset(d, 1);
    d->rehashidx = -1;  /* ▶ rehash 끝 */
    return 1;
}

/* ======================================================================
 * ▶ [6-6] dictRehashMicroseconds — dict.c L446~L461
 *   cron 경로. kvstoreIncrementallyRehash가 남은 시간 예산을 넘겨서 부릅니다.
 *   시간 확인은 100버킷마다 한 번이라 예산을 조금 넘길 수 있습니다 (주석의 "delta").
 * ====================================================================== */
/* Rehash in us+"delta" microseconds. The value of "delta" is larger
 * than 0, and is smaller than 1000 in most cases. The exact upper bound
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMicroseconds(dict *d, uint64_t us) {
    if (d->pauserehash > 0) return 0;  /* ▶ iterator/SCAN이 잡고 있으면 cron도 손대지 않음 */

    monotime timer;
    elapsedStart(&timer);
    int rehashes = 0;

    while(dictRehash(d,100)) {  /* ▶ 버킷 100개 단위로 옮기고 */
        rehashes += 100;
        if (elapsedUs(timer) >= us) break;  /* ▶ 시간이 넘었으면 멈춤 */
    }
    return rehashes;
}

/* ======================================================================
 * ▶ [6-7] _dictRehashStep — dict.c L463~L473
 *   [6-1]의 옛날 버전("본 칸 먼저" 최적화 없이 항상 rehashidx 칸을 옮김).
 *   메인 키 공간 DEL의 기본 경로인 dictTwoPhaseUnlinkFind와 dictGetRandomKey 등이 씁니다.
 *   위 영문 주석이 점진적 rehash의 의도를 가장 짧게 설명합니다.
 * ====================================================================== */
/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->pauserehash == 0) dictRehash(d,1);
}


/* ######################################################################
 * [7] 삭제와 축소
 *   대량 삭제 뒤 큰 배열을 그대로 두면 메모리 낭비이고, 빈 칸이 많아 rehash/SCAN도 느려집니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [7-1] dictGenericDelete — dict.c L627~L670
 *   메인 키 공간의 DEL은 dictTwoPhaseUnlinkFind/Free를 씁니다. 흐름(rehash 한 걸음 → 찾기 →
 *   떼어내기 → 축소 검사)은 같지만 두 가지가 다릅니다: rehash 걸음이 _dictRehashStep
 *   (rehashidx 기준, [6-7])이고, Find부터 Free까지 rehash를 멈춥니다.
 * ====================================================================== */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    dictCmpCache cmpCache = {0};
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    /* dict is empty */
    if (dictSize(d) == 0) return NULL;

    h = dictGetHash(d, key);
    idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d,idx);  /* ▶ 삭제도 rehash 한 걸음 */

    keyCmpFunc cmpFunc = dictGetCmpFunc(d);

    for (table = 0; table <= 1; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        prevHe = NULL;
        while(he) {
            const void *he_key = dictStoredKey2Key(d, dictGetKey(he));
            if (key == he_key || cmpFunc(&cmpCache, key, he_key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    dictSetNext(prevHe, dictGetNext(he));
                else
                    d->ht_table[table][idx] = dictGetNext(he);
                if (!nofree) {
                    dictFreeUnlinkedEntry(d, he);
                }
                d->ht_used[table]--;
                _dictShrinkIfNeeded(d);  /* ▶ 삭제할 때마다 축소 검사 */
                return he;
            }
            prevHe = he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* ======================================================================
 * ▶ [7-2] dictShrinkIfNeeded / _dictShrinkIfNeeded / dictShrink — dict.c L1685~L1718, L326~L333
 * ====================================================================== */
/* Returning DICT_OK indicates a successful shrinking or the dictionary is undergoing rehashing, 
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that shrinking has not been triggered (may be try expanding?)*/
int dictShrinkIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;
    
    /* If the size of hash table is DICT_HT_INITIAL_SIZE, don't shrink it. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) <= DICT_HT_INITIAL_SIZE) return DICT_OK;  /* ▶ 버킷 4개 이하는 안 줄임 */

    /* If we reached below 1:8 elements/buckets ratio, and we are allowed to resize
     * the hash table (global setting) or we should avoid it but the ratio is below 1:32,
     * we'll trigger a resize of the hash table. */
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    /* ▶ 축소 조건: ENABLE이면 키 수 ≤ 버킷 수/8, AVOID면 ≤ 버킷 수/32 */
    if ((can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] * HASHTABLE_MIN_FILL <= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] * HASHTABLE_MIN_FILL * dict_force_resize_ratio <= DICTHT_SIZE(d->ht_size_exp[0])))
    {
        if (dictTypeResizeAllowed(d, d->ht_used[0]))
            dictShrink(d, d->ht_used[0]);  /* ▶ 키 수 이상인 2의 거듭제곱으로. 축소도 [5][6]과 같은 점진적 rehash를 탐 */
        return DICT_OK;
    }
    return DICT_ERR;
}

static void _dictShrinkIfNeeded(dict *d) 
{
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return;

    dictShrinkIfNeeded(d);
}

/* return DICT_ERR if shrink was not performed */
int dictShrink(dict *d, unsigned long size) {
    /* the size is invalid if it is bigger than the size of the hash table
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size || DICTHT_SIZE(d->ht_size_exp[0]) <= size)
        return DICT_ERR;
    return _dictResize(d, size, NULL);
}


/* ######################################################################
 * [8] rehash 일시 정지 — safe iterator
 *   이터레이터가 버킷을 순서대로 도는 중에 키가 다른 테이블로 옮겨지면 빠뜨리거나 두 번 봅니다.
 *   그래서 safe iterator는 도는 동안 pauserehash를 올려 둡니다.
 *   SCAN(dictScanDefrag)도 콜백을 부르는 동안만 잠깐 멈춥니다 (원본 L1583, L1629).
 * ###################################################################### */

/* ======================================================================
 * ▶ [8-1] dictResetIterator / dictNext — dict.c L1160~L1168, L1184~L1220
 * ====================================================================== */
void dictResetIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);  /* ▶ 순회 끝: rehash 재개 */
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
}

dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        if (iter->entry == NULL) {
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    dictPauseRehashing(iter->d);  /* ▶ safe iterator: 도는 동안 rehash 정지 */
                else
                    iter->fingerprint = dictFingerprint(iter->d);  /* ▶ unsafe: 대신 전후 지문(fingerprint)이 다르면 assert로 죽임 */

                /* skip the rehashed slots in table[0] */
                if (dictIsRehashing(iter->d)) {
                    iter->index = iter->d->rehashidx - 1;  /* ▶ 이미 옮겨진 앞쪽 칸은 건너뜀 */
                }
            }
            iter->index++;
            if (iter->index >= (long) DICTHT_SIZE(iter->d->ht_size_exp[iter->table])) {
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;  /* ▶ 옛 테이블을 다 돌면 새 테이블로 */
                    iter->index = 0;
                } else {
                    break;
                }
            }
            iter->entry = iter->d->ht_table[iter->table][iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = dictGetNext(iter->entry);
            return iter->entry;
        }
    }
    return NULL;
}


/* ######################################################################
 * [9] 정책 스위치 — dict.c L1803~L1805
 *   server.c updateDictResizePolicy가 fork 시작/종료 때 부릅니다 (주변 발췌 [5]).
 * ###################################################################### */

void dictSetResizeEnabled(dictResizeEnable enable) {
    atomicSet(dict_can_resize, enable);
}
