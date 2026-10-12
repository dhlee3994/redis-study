/* ======================================================================
 * [해설판] dict 주변 발췌 — dict를 부르는 쪽, 정책을 정하는 쪽
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/server.h
 *   https://github.com/redis/redis/blob/unstable/src/server.c
 *   https://github.com/redis/redis/blob/unstable/src/db.c
 *   https://github.com/redis/redis/blob/unstable/src/t_string.c
 *   https://github.com/redis/redis/blob/unstable/src/object.c
 *   https://github.com/redis/redis/blob/unstable/src/sds.h
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 여러 파일에서 dict와 직접 맞닿은 부분만 골라 모았습니다.
 *   - 각 조각 위에 "파일명 L시작~L끝"을 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *
 * ▶ dict.c는 "어떻게"만 압니다. "어떤 해시 함수로, 언제 resize를 허락하고, 언제 cron이 도와주나"는
 *   전부 여기 있는 코드가 정합니다.
 *
 * ▶ 목차
 *   [1] 상수 (server.h)               : 1ms 예산, cron당 16개, load factor 상한 1.618
 *   [2] 키 공간 만들기 (server.c)     : DB마다 kvstore + 해시 시드
 *   [3] 메인 키 공간의 dictType       : 해시 = SipHash(sds), 비교 = 길이 → memcmp
 *   [4] dictResizeAllowed             : maxmemory 근처에선 확장을 미룸
 *   [5] updateDictResizePolicy        : fork 자식이 있으면 AVOID
 *   [6] databasesCron                 : resize 검사 + 1ms rehash   ★
 *   [7] GET 한 번의 경로 (t_string.c, db.c)
 *   [8] sds 길이는 O(1) (sds.h)
 * ====================================================================== */


/* ######################################################################
 * [1] 상수 — server.h L140~L141, L164, L239~L240
 * ###################################################################### */

#define CRON_DBS_PER_CALL 16  /* ▶ cron 한 번에 resize 검사할 DB 수 */
#define CRON_DICTS_PER_DB 16  /* ▶ DB 하나에서 resize 검사할 dict 수 (클러스터면 16384개 중 16개씩) */

#define INCREMENTAL_REHASHING_THRESHOLD_US 1000  /* ▶ cron 한 번에 rehash에 쓸 수 있는 시간 = 1ms (모든 DB 합계) */

/* Hash table parameters */
#define HASHTABLE_MAX_LOAD_FACTOR 1.618   /* Maximum hash table load factor. */  /* ▶ 이 값 이하면 maxmemory를 넘을 때 확장을 미룸 [4] */


/* ######################################################################
 * [2] 키 공간 만들기
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] 해시 시드 — server.c L8328~L8330 (main 안)
 *   서버를 켤 때마다 시드가 달라집니다. 같은 키라도 프로세스마다 해시값이 다릅니다.
 *   그래서 바깥에서 "같은 버킷에 몰리는 키"를 계산해 보낼 수 없습니다 (hash flooding 방어).
 * ====================================================================== */
    uint8_t hashseed[16];
    getRandomBytes(hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);

/* ======================================================================
 * ▶ [2-2] DB마다 kvstore 만들기 — server.c L3118~L3129 (initServer 안)
 * ====================================================================== */
    /* Create the Redis databases, and initialize other internal state. */
    int slot_count_bits = 0;
    int flags = KVSTORE_ALLOCATE_DICTS_ON_DEMAND;
    if (server.cluster_enabled) {
        slot_count_bits = CLUSTER_SLOT_MASK_BITS;  /* ▶ 14 → dict 16384개 (슬롯마다 하나) */
        flags |= KVSTORE_FREE_EMPTY_DICTS;  /* ▶ 키가 다 빠진 슬롯의 dict는 해제 */
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].keys = kvstoreCreate(&kvstoreExType, &dbDictType, slot_count_bits, flags);  /* ▶ ★ 메인 키 공간. dictType = dbDictType [3] */
        server.db[j].expires = kvstoreCreate(&kvstoreBaseType, &dbExpiresDictType, slot_count_bits, flags);  /* ▶ TTL 있는 키만 따로 모은 공간. 같은 kvobj를 가리킴 */
        server.db[j].blessed_keys = blessedKvstoreCreate(slot_count_bits, flags);
        server.db[j].subexpires = estoreCreate(&subexpiresBucketsType, slot_count_bits);
        /* ▶ [생략] server.c L3130 이하: DB 초기화 계속 */


/* ######################################################################
 * [3] 메인 키 공간의 dictType — server.c L647~L661, L436~L438, L328~L345
 * ###################################################################### */

/* Db->dict, keys are of type kvobj, unification of key and value */
dictType dbDictType = {
    dictSdsHash,            /* hash function */  /* ▶ 해시 [3-1] */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    dictSdsCompareKV,       /* lookup key compare */  /* ▶ 비교 [3-2] */
    dictDestructorKV,       /* key destructor */
    NULL,                   /* val destructor */
    dictResizeAllowed,      /* allow to resize */  /* ▶ resize 허락 여부 [4] */
    .no_value = 1,          /* keys and values are unified (kvobj) */  /* ▶ 값은 kvobj 안에 → dict는 kvobj 포인터만 저장 */
    .keys_are_odd = 0,      /* simple kvobj (robj) struct */
    .keyFromStoredKey = kvGetKey,    /* get key from stored-key */
    .prefetchEntryKey = dbDictPrefetchEntryKey,
    .prefetchEntryValue = dbDictPrefetchEntryValue,
};

/* ======================================================================
 * ▶ [3-1] dictSdsHash — server.c L436~L438
 *   sdslen이 O(1)이라 [8], 해시 비용은 순수하게 SipHash가 키 바이트를 읽는 시간입니다.
 * ====================================================================== */
uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* ======================================================================
 * ▶ [3-2] kvGetKey / dictSdsCompareKV — server.c L328~L345
 *   체인을 따라가며 비교할 때:
 *     1) 찾는 키의 길이는 첫 비교 때 한 번만 구해서 cache에 저장
 *     2) 길이가 다르면 바로 "다름" (길이가 다른 충돌은 여기서 끝남. user:12345처럼
 *        같은 길이 키가 많으면 memcmp까지 감)
 *     3) 길이가 같을 때만 memcmp
 *   dict.c [2-2]에서 포인터가 같으면 이 함수조차 안 부릅니다.
 * ====================================================================== */
static const void *kvGetKey(const void *kv) {
    sds sdsKey = kvobjGetKey((kvobj *) kv);  /* ▶ kvobj 안에 박힌 sds 키 [7-3] */
    return sdsKey;
}

int dictSdsCompareKV(dictCmpCache *cache, const void *sdsKey1, const void *sdsKey2)
{
    /* is first cmp call of a new lookup */
    if (cache->useCache == 0) {
        cache->useCache = 1;
        cache->data[0].sz = sdslen((sds) sdsKey1);
    }

    size_t l1 = cache->data[0].sz;
    size_t l2 = sdslen((sds)sdsKey2);
    if (l1 != l2) return 0;
    return memcmp(sdsKey1, sdsKey2, l1) == 0;
}


/* ######################################################################
 * [4] dictResizeAllowed — server.c L562~L577
 *   dict.c dictTypeResizeAllowed가 확장(그리고 축소) 직전에 부릅니다.
 *   load factor ≤ 1.618 : 새 테이블을 잡으면 maxmemory를 넘는지 확인 → 넘으면 이번엔 확장 안 함
 *   load factor > 1.618 : 성능이 더 나빠지기 전에 무조건 확장
 *   즉 메모리가 빠듯하면 체인이 평균 1.6개까지 길어지는 것을 감수하고 확장을 미룹니다.
 *   확장했다가 maxmemory를 넘으면 키 eviction이 한꺼번에 일어나기 때문입니다.
 * ###################################################################### */

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead redis
 * rejects user's requests or evicts some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of redis, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
int dictResizeAllowed(size_t moreMem, double usedRatio) {
    /* for debug purposes: dict is not allowed to be resized. */
    if (!server.dict_resizing) return 0;

    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);  /* ▶ 새 테이블만큼 더 쓰면 maxmemory를 넘는가 */
    } else {
        return 1;
    }
}


/* ######################################################################
 * [5] updateDictResizePolicy — server.c L821~L834
 *   fork 직후와 자식 프로세스가 끝날 때 불립니다.
 *   자식이 메모리 스냅샷을 쓰는 동안 부모가 rehash로 페이지를 마구 건드리면, 건드린 페이지마다
 *   copy-on-write 복사가 일어나 메모리 사용량이 크게 늘어납니다. 그래서 resize를 최대한 미룹니다.
 * ###################################################################### */

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize or rehash the tables accordingly to the fact we have an
 * active fork child running. */
void updateDictResizePolicy(void) {
    if (server.in_fork_child != CHILD_TYPE_NONE)
        dictSetResizeEnabled(DICT_RESIZE_FORBID);  /* ▶ 자식 프로세스 자신: 금지 */
    else if (hasActiveChildProcess())
        dictSetResizeEnabled(DICT_RESIZE_AVOID);  /* ▶ 부모, 자식이 살아 있음: load factor 4 이상일 때만 */
    else
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);  /* ▶ 평소 */
}


/* ######################################################################
 * [6] databasesCron — server.c L1316~L1378   ★
 *   serverCron이 매번 부릅니다 (기본 hz=10 → 100ms마다). server 발췌에서 생략했던 부분입니다.
 *   resize "검사"와 rehash "진행"을 따로 합니다.
 *     - resize 검사 : DB 16개 × dict 16개씩 돌아가며 (kvstore 발췌 [5])
 *     - rehash 진행 : rehash 중인 dict만, 모든 DB 합쳐 최대 1ms (kvstore 발췌 [6])
 *   메인 스레드가 100ms마다 최대 약 1ms를 rehash에 씁니다. 그 1ms 동안 도착한 요청은 기다립니다.
 * ###################################################################### */

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {
    /* ▶ [생략] 원본 L1320~L1335: 만료 키 삭제(activeExpireCycle), active defrag, 클러스터 active-trim */

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    if (!hasActiveChildProcess()) {  /* ▶ ★ fork 자식이 있으면 cron은 resize도 rehash도 안 함 */
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        for (j = 0; j < dbs_per_call; j++) {
            redisDb *db = &server.db[resize_db % server.dbnum];
            kvstoreTryResizeDicts(db->keys, CRON_DICTS_PER_DB, resizeShouldSkip);  /* ▶ resize 시작 여부만 검사 */
            kvstoreTryResizeDicts(db->expires, CRON_DICTS_PER_DB, resizeShouldSkip);
            kvstoreTryResizeDicts(db->blessed_keys, CRON_DICTS_PER_DB, resizeShouldSkip);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {  /* ▶ activerehashing 설정 (기본 yes) */
            uint64_t elapsed_us = 0;
            for (j = 0; j < dbs_per_call; j++) {
                redisDb *db = &server.db[rehash_db % server.dbnum];
                elapsed_us += kvstoreIncrementallyRehash(db->keys, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);  /* ▶ 남은 예산만 넘김 */
                if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US)
                    break;
                elapsed_us += kvstoreIncrementallyRehash(db->expires, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
                if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US)
                    break;
                elapsed_us += kvstoreIncrementallyRehash(db->blessed_keys, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
                if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US)
                    break;
                rehash_db++;
            }
        }
    }
}


/* ######################################################################
 * [7] GET 한 번의 경로
 *   getGenericCommand → lookupKeyReadOrReply → lookupKeyRead → lookupKeyReadWithFlags
 *   → lookupKey → dbFindByLink → kvstoreDictFindLink → dictFindLink
 * ###################################################################### */

/* ======================================================================
 * ▶ [7-1] getGenericCommand / lookupKeyReadOrReply / lookupKeyRead* — t_string.c, db.c
 * ====================================================================== */
int getGenericCommand(client *c) {
    kvobj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL)
        return C_OK;
    /* ▶ [생략] t_string.c L461 이하: 타입 검사 후 응답 */

kvobj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    kvobj *kv = lookupKeyRead(c->db, key);
    if (!kv) addReplyOrErrorObject(c, reply);
    return kv;
}

kvobj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    serverAssert(!(flags & LOOKUP_WRITE));
    return lookupKey(db, key, flags, NULL);
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
kvobj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* ======================================================================
 * ▶ [7-2] lookupKey / dbFindByLink / getKeySlot — db.c L296~L298, L3132~L3150, L494~L510
 *   lookupKey는 찾은 뒤 만료 확인, LRU/LFU 갱신, 통계를 처리합니다. 조회 자체는 첫 줄이 전부입니다.
 * ====================================================================== */
kvobj *lookupKey(redisDb *db, robj *key, int flags, dictEntryLink *link) {

    kvobj *val = dbFindByLink(db, key->ptr, link);  /* ▶ ★ 조회 */
    /* ▶ [생략] db.c L299 이하: 만료 확인, 접근 시간 갱신, hit/miss 통계 */

/* Find a KV in the main db. Return also link to it.
 *
 * plink - If found, set to the link of the key in the dict.
 *         If not found, set to the bucket where the key should be added.
 *         If set to NULL, then HT of dict not allocated yet.
 */
kvobj *dbFindByLink(redisDb *db, sds key, dictEntryLink *plink) {
    int slot = getKeySlot(key);  /* ▶ 일반 모드는 0, 클러스터면 CRC16 슬롯 */
    dictEntryLink link, bucket;

    link = kvstoreDictFindLink(db->keys, slot, key, &bucket);  /* ▶ kvstore 발췌 [4] → dict.c [2-1] */
    if (link == NULL) {
        if (plink) *plink = bucket;
        return NULL;
    } else {
        if (plink) *plink = link;
        return dictGetKV(*link);
    }
}

/* Return slot-specific dictionary for key based on key's hash slot when cluster mode is enabled, else 0.*/
int getKeySlot(sds key) {
    if (!server.cluster_enabled) return 0;  /* ▶ 일반 모드: dict가 하나뿐이라 항상 0 */
    /* This is performance optimization that uses pre-set slot id from the current command,
     * in order to avoid calculation of the key hash.
     *
     * This optimization is only used when current_client flag `CLIENT_EXECUTING_COMMAND` is set.
     * It only gets set during the execution of command under `call` method. Other flows requesting
     * the key slot would fallback to calculateKeySlot.
     */
    if (server.current_client && server.current_client->slot >= 0 && server.current_client->flags & CLIENT_EXECUTING_COMMAND) {
        debugServerAssertWithInfo(server.current_client, NULL,
                                  (int)keyHashSlot(key, (int)sdslen(key)) == server.current_client->slot);
        return server.current_client->slot;
    }
    int slot = keyHashSlot(key, (int)sdslen(key));
    return slot;
}

/* ======================================================================
 * ▶ [7-3] kvobjGetKey — object.c L250~L259
 *   Redis 8은 키(sds)를 값 객체(kvobj) 안에 붙여서 할당합니다. 조회 때 키를 읽으려고
 *   포인터를 한 번 더 따라갈 필요가 없습니다 (메모리 접근 1번 절약).
 * ====================================================================== */
/* SDS header size of the embedded key, indexed by kvBits->key_sds_type. */
sds kvobjGetKey(const kvobj *kv) {
    static const uint8_t kvobjKeyHdrSize[4] = {
        sizeof(struct sdshdr5), sizeof(struct sdshdr8),
        sizeof(struct sdshdr16), sizeof(struct sdshdr32)
    };
    debugServerAssert(kv->iskvobj);
    kvBits *bits = kvobjBits(kv);
    return (sds) ((char *) (bits + 1) + kvobjKeyHdrSize[bits->key_sds_type]);
}


/* ######################################################################
 * [8] sds 길이는 O(1) — sds.h L32~L37, L64~L71, L98~L111
 *   sds는 문자열 바로 앞에 헤더(길이, 할당 크기, 타입)가 붙은 char*입니다.
 *   C 문자열처럼 '\0'을 찾아 strlen할 필요 없이 헤더에서 길이를 바로 읽습니다.
 *   해시 계산([3-1])과 키 비교([3-2])가 모두 sdslen을 부르므로, 이게 O(1)이라는 점만 알면 됩니다.
 *   sds.c의 나머지(문자열 이어 붙이기, 포맷 등)는 이번 주제와 관계가 거의 없습니다.
 *
 *   메모리 모양:  [len][alloc][flags][ h e l l o \0 ]
 *                                    ↑ sds 포인터는 여기를 가리킴. s[-1]이 flags
 * ###################################################################### */

struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))  /* ▶ 포인터를 헤더 크기만큼 뒤로 = 헤더 구조체 */
#define SDS_TYPE_5_LEN(s) (((unsigned char)(s[-1])) >> SDS_TYPE_BITS)

static inline unsigned char sdsType(sds s) {
    unsigned char flags = s[-1];
    return flags & SDS_TYPE_MASK;
}

static inline size_t sdslen(const sds s) {
    switch (sdsType(s)) {
        case SDS_TYPE_5: return SDS_TYPE_5_LEN(s);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}
