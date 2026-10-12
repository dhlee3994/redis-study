/* ======================================================================
 * [해설판] kvstore.c 발췌 — dict 여러 개를 묶어서 키 공간 하나로
 * 원본: https://github.com/redis/redis/blob/unstable/src/kvstore.c (1176줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 1176줄 중 resize/rehash와 조회에 관련된 부분만
 *   골라 다시 배치했습니다.
 *   - 각 함수 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *
 * ▶ 왜 이 파일을 보나
 *   Redis 8에서 DB 하나의 키 공간(db->keys)은 dict 하나가 아니라 kvstore입니다.
 *     - 클러스터 모드 : 슬롯마다 dict 하나 → dict 16384개
 *     - 일반 모드     : dict 1개 (kvstore는 얇은 포장일 뿐)
 *   dict.c는 dict 하나만 압니다. "어느 dict가 지금 rehash 중인지" 아는 것은 kvstore입니다.
 *   cron이 16384개를 다 훑지 않고 rehash 중인 dict만 바로 꺼낼 수 있는 이유가 여기 있습니다.
 *
 * ▶ 목차
 *   [1] struct _kvstore                     : rehashing 목록, resize_cursor
 *   [2] kvstoreCreate                       : dict에 rehash 시작/종료 콜백을 꽂음
 *   [3] rehash 시작/종료 콜백               : 목록에 넣고 빼기 (O(1))
 *   [4] 조회와 삽입 포장                    : 슬롯 번호로 dict를 고르고 dict.c에 넘김
 *   [5] kvstoreTryResizeDicts               : cron이 resize 필요 여부를 조금씩 검사
 *   [6] kvstoreIncrementallyRehash          : cron이 1ms 예산 안에서 rehash  ★
 * ====================================================================== */


/* ######################################################################
 * [1] struct _kvstore — kvstore.c L28~L44
 * ###################################################################### */

struct _kvstore {
    int flags;
    kvstoreType *type;
    dictType dtype;
    dict **dicts;  /* ▶ dict 배열. 클러스터 모드면 16384칸, 아니면 1칸 */
    long long num_dicts;
    long long num_dicts_bits;
    list *rehashing;                       /* List of dictionaries in this kvstore that are currently rehashing. */  /* ▶ ★ 지금 rehash 중인 dict만 모은 목록 */
    int resize_cursor;                     /* Cron job uses this cursor to gradually resize dictionaries (only used if num_dicts > 1). */  /* ▶ [5]에서 resize 검사를 어디까지 했는지 */
    int allocated_dicts;                   /* The number of allocated dicts. */
    int non_empty_dicts;                   /* The number of non-empty dicts. */
    unsigned long long key_count;          /* Total number of keys in this kvstore. */
    unsigned long long bucket_count;       /* Total number of buckets in this kvstore across dictionaries. */
    fenwickTree *dict_sizes;               /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given dict-index. */
    size_t overhead_hashtable_rehashing;   /* The overhead of dictionaries rehashing. */  /* ▶ INFO memory의 mem_overhead_db_hashtable_rehashing 계산에 쓰임 */
    void *metadata[];                      /* conditionally allocated based on "flags" */
};


/* ######################################################################
 * [2] kvstoreCreate — kvstore.c L194~L243
 *   server.c initServer가 DB마다 keys / expires용으로 하나씩 만듭니다 (주변 발췌 [2]).
 *   dbDictType을 복사한 뒤 rehash 콜백을 kvstore 것으로 채워 넣습니다.
 * ###################################################################### */

/* Create an array of dictionaries
 * num_dicts_bits is the log2 of the amount of dictionaries needed (e.g. 0 for 1 dict,
 * 3 for 8 dicts, etc.) */
kvstore *kvstoreCreate(kvstoreType *type, dictType *dtype, int num_dicts_bits, int flags) {
    /* We can't support more than 2^16 dicts because we want to save 48 bits
     * for the dict cursor, see kvstoreScan */
    assert(num_dicts_bits <= 16);
    assert(!type->dictMetadataBytes || type->dictMetadataBytes(NULL) >= sizeof(kvstoreDictMetaBase));

    /* Calc kvstore size */   
    size_t kvsize = sizeof(kvstore);
    /* Conditionally calc also histogram size */
    if (type->kvstoreMetadataBytes)
        kvsize += type->kvstoreMetadataBytes(NULL);
    
    kvstore *kvs = zcalloc(kvsize);
    memcpy(&kvs->dtype, dtype, sizeof(kvs->dtype));
    kvs->flags = flags;
    kvs->type = type;

    /* kvstore must be the one to set these callbacks, so we make sure the
     * caller didn't do it */
    assert(!dtype->userdata);
    assert(!dtype->dictMetadataBytes);
    assert(!dtype->rehashingStarted);
    assert(!dtype->rehashingCompleted);
    kvs->dtype.userdata = kvs;  /* ▶ 콜백 안에서 d->type->userdata로 자기 kvstore를 찾음 */
    kvs->dtype.dictMetadataBytes = type->dictMetadataBytes ?
        type->dictMetadataBytes : kvstoreDictBaseMetaSize;
    kvs->dtype.rehashingStarted = kvstoreDictRehashingStarted;  /* ▶ ★ dict가 rehash를 시작하면 목록에 추가 [3] */
    kvs->dtype.rehashingCompleted = kvstoreDictRehashingCompleted;  /* ▶ ★ 끝나면 목록에서 제거 [3] */
    kvs->dtype.bucketChanged = kvstoreDictBucketChanged;

    kvs->num_dicts_bits = num_dicts_bits;
    kvs->num_dicts = 1 << kvs->num_dicts_bits;  /* ▶ 클러스터: 1<<14 = 16384, 일반: 1<<0 = 1 */
    kvs->dicts = zcalloc(sizeof(dict*) * kvs->num_dicts);
    if (!(kvs->flags & KVSTORE_ALLOCATE_DICTS_ON_DEMAND)) {  /* ▶ ON_DEMAND가 아니면 전부 미리 생성. 메인 키 공간은 ON_DEMAND라 그 슬롯에 첫 키가 들어올 때 생성 */
        for (int i = 0; i < kvs->num_dicts; i++)
            createDictIfNeeded(kvs, i);
    }

    kvs->rehashing = listCreate();
    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->dict_sizes = kvs->num_dicts > 1 ? fwTreeCreate(kvs->num_dicts_bits) : NULL;
    kvs->bucket_count = 0;
    kvs->overhead_hashtable_rehashing = 0;
    return kvs;
}


/* ######################################################################
 * [3] rehash 시작/종료 콜백 — kvstore.c L142~L174
 *   dict.c _dictResize가 rehashingStarted를, dictCheckRehashingCompleted가 rehashingCompleted를 부릅니다.
 *   첫 생성(빈 테이블)도 _dictResize를 거치므로 "추가 → 즉시 제거"가 한 번 일어납니다. 둘 다 O(1)입니다.
 * ###################################################################### */

/* Adds dictionary to the rehashing list, which allows us
 * to quickly find rehash targets during incremental rehashing.
 *
 * If there are multiple dicts, updates the bucket count for the given dictionary
 * in a DB, bucket count incremented with the new ht size during the rehashing phase.
 * If there's one dict, bucket count can be retrieved directly from single dict bucket. */
static void kvstoreDictRehashingStarted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
    listAddNodeTail(kvs->rehashing, d);  /* ▶ 목록 끝에 추가 O(1) */
    metadata->rehashing_node = listLast(kvs->rehashing);  /* ▶ 나중에 O(1)로 빼려고 노드를 dict 메타데이터에 기억 */

    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    kvs->overhead_hashtable_rehashing += from;  /* ▶ 옛 테이블 버킷 수를 더해 둠 (rehash 동안 추가로 쓰는 메모리) */
}

/* Remove dictionary from the rehashing list.
 *
 * Updates the bucket count for the given dictionary in a DB. It removes
 * the old ht size of the dictionary from the total sum of buckets for a DB.  */
static void kvstoreDictRehashingCompleted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
    if (metadata->rehashing_node) {
        listDelNode(kvs->rehashing, metadata->rehashing_node);  /* ▶ 목록에서 제거 O(1) */
        metadata->rehashing_node = NULL;
    }

    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    kvs->overhead_hashtable_rehashing -= from;
}


/* ######################################################################
 * [4] 조회와 삽입 포장 — kvstore.c L830~L835, L861~L866, L881~L891
 *   didx = 슬롯 번호 (일반 모드는 항상 0). 해당 dict가 없으면 키도 없습니다.
 *   kvstore는 dict를 고르기만 하고, 해시/체인/rehash는 전부 dict.c가 합니다.
 * ###################################################################### */

dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFind(d, key);
}

dictEntryLink kvstoreDictFindLink(kvstore *kvs, int didx, void *key, dictEntryLink *bucket) {
    if (bucket) *bucket = NULL;    
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d) return NULL;
    return dictFindLink(d, key, bucket);  /* ▶ dict.c [2-1]로 */
}

void kvstoreDictSetAtLink(kvstore *kvs, int didx, void *kv, dictEntryLink *link, int newItem) {
    dict *d;
    if (newItem) {
        d = createDictIfNeeded(kvs, didx);  /* ▶ 그 슬롯의 첫 키면 여기서 dict 생성 */
        dictSetKeyAtLink(d, kv, link, newItem);  /* ▶ dict.c [3-1]: 확장 검사 후 삽입 */
        cumulativeKeyCountAdd(kvs, didx, 1); /* must be called only after updating dict */
    } else {
        d = kvstoreGetDict(kvs, didx);
        dictSetKeyAtLink(d, kv, link, newItem);
    }
}


/* ######################################################################
 * [5] kvstoreTryResizeDicts — kvstore.c L651~L667
 *   server.c databasesCron이 DB마다 limit=16(CRON_DICTS_PER_DB)으로 부릅니다.
 *   삽입/삭제가 없는 dict는 dict.c의 자동 확장/축소 검사를 거치지 않으므로, cron이 대신 봐 줍니다.
 *   resize "시작"만 하고 키를 옮기지는 않습니다. 옮기는 건 [6]과 요청 경로입니다.
 * ###################################################################### */

/* This method traverses through kvstore dictionaries and triggers a resize,
 * unless skip_cb indicates otherwise. It first tries to shrink if needed, and
 * if it doesn't try to shrink, it tries to expand. */
void kvstoreTryResizeDicts(kvstore *kvs, int limit, kvstoreResizeShouldSkipDictIndex *skip_cb) {
    if (limit > kvs->num_dicts)  /* ▶ 한 번에 최대 limit개 dict만 검사 */
        limit = kvs->num_dicts;

    for (int i = 0; i < limit; i++) {
        int didx = kvs->resize_cursor;
        dict *d = kvstoreGetDict(kvs, didx);
        if (d && (!skip_cb || !skip_cb(didx))) {
            if (dictShrinkIfNeeded(d) == DICT_ERR)  /* ▶ 줄일 필요가 없으면 */
                dictExpandIfNeeded(d);  /* ▶ 키울 필요가 있는지 검사 */
        }
        kvs->resize_cursor = (didx + 1) % kvs->num_dicts;  /* ▶ 다음 cron은 여기서 이어서 */
    }
}


/* ######################################################################
 * [6] kvstoreIncrementallyRehash — kvstore.c L669~L695   ★
 *   server.c databasesCron이 남은 시간 예산(전체 1000us)을 넘겨서 부릅니다.
 *   rehashing 목록의 맨 앞 dict부터 dictRehashMicroseconds로 옮기고, 다 끝난 dict는 콜백 [3]이
 *   목록에서 빼므로 다음 바퀴엔 그다음 dict가 맨 앞에 옵니다.
 *
 *   참고: 맨 앞 dict가 pauserehash 상태면 dictRehashMicroseconds가 바로 0을 돌려주고, 같은 dict가
 *   계속 맨 앞에 남습니다. 코드상으로는 이 경우 예산(1ms)이 다 찰 때까지 같은 호출을 반복합니다.
 *   cron은 명령 실행 사이에 돌기 때문에 이런 상황은 드뭅니다.
 * ###################################################################### */

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use threshold_us
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise 0 is returned. */
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us) {
    if (listLength(kvs->rehashing) == 0)  /* ▶ rehash 중인 dict가 없으면 바로 끝. 16384개를 훑을 필요가 없음 */
        return 0;

    /* Our goal is to rehash as many dictionaries as we can before reaching threshold_us,
     * after each dictionary completes rehashing, it removes itself from the list. */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us = 0;
    elapsedStart(&timer);
    while ((node = listFirst(kvs->rehashing))) {
        dictRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);  /* ▶ 남은 예산만큼 rehash (dict.c [6-6]) */

        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break;  /* Reached the time limit. */
        }
    }
    return elapsed_us;
}
