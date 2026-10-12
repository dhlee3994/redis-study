/* ======================================================================
 * [해설판] lazyfree.c 발췌 — Big Key를 지울 때의 비용과 인코딩
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/lazyfree.c (391줄)
 *   함께: db.c (DEL, UNLINK, 삭제 공통 함수), config.c (lazyfree 설정)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본에서 키 하나를 지우는 경로만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 파일과 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *
 * ▶ 왜 3주차에 lazyfree를 보나
 *   3주차 가설의 "Big Key 구간에서 latency가 달라진다"는 부분은 인코딩 변환보다 이쪽과 더 관련이
 *   깊습니다. 키를 지우는 비용은 "원소 수"가 아니라 "할당 개수"에 비례하고, 할당 개수는 인코딩이
 *   정합니다 (object_발췌.c [5]).
 *
 *   인코딩                 free 횟수       lazyfreeGetFreeEffort가 돌려주는 값
 *   listpack / intset      1번             1
 *   quicklist              노드 수만큼     노드 수 (원소 수가 아님. 8KB 노드 하나에 원소 수백 개)
 *   HT (Hash, Set)         원소 수만큼     원소 수
 *   skiplist (ZSet)        원소 수 이상    원소 수 (skiplist 노드 + dict)
 *
 *   그래서 원소 100만 개짜리 Hash(HT)를 DEL하면 메인 스레드에서 free가 100만 번 돌고, 그동안 다른
 *   클라이언트의 명령은 모두 기다립니다. 같은 100만 개라도 List는 노드 수천 개만 해제합니다.
 *
 * ▶ 세 가지 해제 경로
 *   DEL           : lazyfree-lazy-user-del (기본 no)   → 기본은 메인 스레드에서 바로 해제
 *   UNLINK        : 항상 async                        → 할당 64개 초과면 백그라운드 스레드로
 *   SET 덮어쓰기   : lazyfree-lazy-server-del (기본 no) → 기본은 메인 스레드에서 바로 해제
 *                   (t_string_발췌.c [3-2] dbSetValue L728~L731)
 *   만료, eviction, FLUSHALL도 각각 lazyfree-lazy-expire, -eviction, -user-flush 설정을 따릅니다.
 *
 * ▶ 목차
 *   [1] 삭제 명령        : DEL, UNLINK                 (db.c)
 *   [2] dbGenericDelete  : 키를 dict에서 떼고 값을 해제   (db.c)
 *   [3] freeObjAsync     : "64개"를 넘으면 백그라운드로    ★
 *   [4] lazyfreeGetFreeEffort : 비용을 인코딩으로 추정     ★
 *   [부록] lazyfree 설정 (config.c)
 * ====================================================================== */


/* ######################################################################
 * [1] 삭제 명령 — db.c
 *   DEL과 UNLINK는 같은 함수를 부르고, 값을 해제하는 방법만 다릅니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] delGenericCommand, delCommand — db.c L1437~L1459
 * ====================================================================== */
/* This command implements DEL and UNLINK. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (expireIfNeeded(c->db, c->argv[j], NULL, 0) == KEY_DELETED)
            continue;
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :  /* ▶ ★ lazy면 dbAsyncDelete, 아니면 dbSyncDelete */
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            keyModified(c,c->db,c->argv[j],NULL,1);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

void delCommand(client *c) {
    delGenericCommand(c,server.lazyfree_lazy_user_del);  /* ▶ ★ DEL: 기본(no)이면 동기 해제 */
}

/* ======================================================================
 * ▶ [1-2] unlinkCommand — db.c L1556~L1558
 * ====================================================================== */
void unlinkCommand(client *c) {
    delGenericCommand(c,1);  /* ▶ ★ UNLINK: 항상 async 경로 */
}

/* ======================================================================
 * ▶ [1-3] dbSyncDelete, dbAsyncDelete — db.c L936~L945
 * ====================================================================== */
/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbSyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 0, DB_FLAG_KEY_DELETED);
}

/* Delete a key, value, and associated expiration entry if any, from the DB. If
 * the value consists of many allocations, it may be freed asynchronously. */
int dbAsyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 1, DB_FLAG_KEY_DELETED);
}


/* ######################################################################
 * [2] dbGenericDelete — db.c
 *   키를 메인 dict에서 떼어 낸 뒤 값을 해제합니다. 동기면 마지막 dict 정리 단계에서 decrRefCount로
 *   바로 해제되고, async면 freeObjAsync에 넘깁니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] dbGenericDelete — db.c L876~L934
 * ====================================================================== */
/* Helper for sync and async delete. */
int dbGenericDelete(redisDb *db, robj *key, int async, int flags) {
    dictEntryLink link;
    int table;
    int slot = getKeySlot(key->ptr);
    link = kvstoreDictTwoPhaseUnlinkFind(db->keys, slot, key->ptr, &table);  /* ▶ 1단계: 키를 찾아 두기만 함 (아직 dict에서 빼지 않음) */

    if (link) {
        kvobj *kv = dictGetKV(*link);

        /* ▶ [생략] 원본 L886~L913: Hash 필드 TTL / stream 정리, 모듈 알림, 블로킹 클라이언트 깨우기 */
        /* if expirable, delete an entry from the expires dict is not decrRefCount of kvobj */
        if (kvobjGetExpire(kv) != -1)
            kvstoreDictDelete(db->expires, slot, key->ptr);

        if (async) {  /* ▶ ★ UNLINK, 또는 lazyfree 설정이 켜진 경우 */
            if (server.memory_tracking_enabled)
                updateSlotAllocSize(db, slot, kv, kvobjAllocSize(kv), -1);
            freeObjAsync(key, kv, db->id);  /* ▶ ★ 할당이 많으면 백그라운드 스레드로 ([3]) */
            /* Set the key to NULL in the main dictionary. */
            kvstoreDictSetAtLink(db->keys, slot, NULL, &link, 0);  /* ▶ 값을 넘겼으므로 dict 칸을 비워서 아래에서 다시 해제되지 않게 함 */
        }
        kvstoreDictTwoPhaseUnlinkFree(db->keys, slot, link, table);  /* ▶ ★ 2단계: dict에서 빼고 해제. 동기 경로면 여기서 decrRefCount → 큰 HT면 O(N) */

        /* remove key from histogram */
        if(!(flags & DB_FLAG_NO_UPDATE_KEYSIZES))
            updateKeysizesHist(db, type, oldlen, -1);
        return 1;
    } else {
        return 0;
    }
}


/* ######################################################################
 * [3] freeObjAsync — 기준은 할당 64개
 *   할당이 64개 이하면 그냥 메인 스레드에서 해제합니다. 백그라운드 작업을 만드는 비용이 더 크기 때문입니다.
 *   즉 listpack 인코딩 키(원소가 수백 개여도 할당 1개)는 UNLINK를 해도 동기로 해제되고, 그래도 빠릅니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] LAZYFREE_THRESHOLD, freeObjAsync — lazyfree.c L193~L213
 * ====================================================================== */
/* If there are enough allocations to free the value object asynchronously, it
 * may be put into a lazy free list instead of being freed synchronously. The
 * lazy free list will be reclaimed in a different bio.c thread. If the value is
 * composed of a few allocations, to free in a lazy way is actually just
 * slower... So under a certain limit we just free the object synchronously. */
#define LAZYFREE_THRESHOLD 64  /* ▶ ★ 할당 64개 */

/* Free an object, if the object is huge enough, free it in async way. */
void freeObjAsync(robj *key, robj *obj, int dbid) {
    size_t free_effort = lazyfreeGetFreeEffort(key,obj,dbid);  /* ▶ [4] 인코딩별로 계산 */
    /* Note that if the object is shared, to reclaim it now it is not
     * possible. This rarely happens, however sometimes the implementation
     * of parts of the Redis core may call incrRefCount() to protect
     * objects, and then call dbDelete(). */
    if (free_effort > LAZYFREE_THRESHOLD && obj->refcount == 1) {  /* ▶ 공유 중인 객체는 지금 해제할 수 없으므로 동기 경로(실제로는 refcount만 감소) */
        atomicIncr(lazyfree_objects,1);
        bioCreateLazyFreeJob(lazyfreeFreeObject,1,obj);  /* ▶ ★ bio 스레드의 작업 큐에 넣음. 메인 스레드는 바로 다음 명령으로 */
    } else {
        decrRefCount(obj);  /* ▶ 64개 이하: 그 자리에서 해제 */
    }
}

/* ======================================================================
 * ▶ [3-2] lazyfreeFreeObject — 백그라운드 스레드에서 실행 — lazyfree.c L11~L18
 * ====================================================================== */
/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
void lazyfreeFreeObject(void *args[]) {
    robj *o = (robj *) args[0];
    decrRefCount(o);  /* ▶ 같은 decrRefCount. 다만 메인 스레드가 아니라 bio 스레드에서 돎 */
    atomicDecr(lazyfree_objects,1);
    atomicIncr(lazyfreed_objects,1);
}


/* ######################################################################
 * [4] lazyfreeGetFreeEffort — 해제 비용을 인코딩으로 추정
 *   정확한 할당 수가 아니라 "할당 수에 비례하는 값"을 O(1)로 구합니다. 작은 인코딩은 전부 1입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [4-1] lazyfreeGetFreeEffort — lazyfree.c L125~L191
 * ====================================================================== */
/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is composed of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. */
size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid) {
    if (obj->type == OBJ_LIST && obj->encoding == OBJ_ENCODING_QUICKLIST) {  /* ▶ ★ List(quicklist): 원소 수가 아니라 노드 수 */
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {  /* ▶ Set(HT): 원소 수 */
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST){  /* ▶ ZSet(skiplist): 원소 수 */
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {  /* ▶ ★ Hash(HT): 필드 수 */
        dict *ht = obj->ptr;
        return dictSize(ht);
    /* ▶ [생략] 원본 L153~L187: TMPL_ARRAY Hash, Stream, 모듈, Array 타입 */
    } else {
        return 1; /* Everything else is a single allocation. */  /* ▶ ★ listpack, intset, String 등: 할당 1개 */
    }
}


/* ######################################################################
 * [부록] lazyfree 설정 — config.c (기본값은 전부 no)
 *   redis.conf 예제 파일에도 전부 no로 적혀 있습니다 (redis.conf L1389~L1406).
 *   운영 중인 서버에서 켜져 있는지는 CONFIG GET lazyfree* 로 확인하세요.
 * ###################################################################### */

/* ======================================================================
 * ▶ lazyfree-lazy-* — config.c L3457~L3461
 * ====================================================================== */
    createBoolConfig("lazyfree-lazy-eviction", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_eviction, 0, NULL, NULL),  /* ▶ maxmemory로 쫓겨나는 키 */
    createBoolConfig("lazyfree-lazy-expire", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_expire, 0, NULL, NULL),  /* ▶ TTL로 만료되는 키 */
    createBoolConfig("lazyfree-lazy-server-del", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_server_del, 0, NULL, NULL),  /* ▶ ★ 서버가 지우는 경우: SET 덮어쓰기, RENAME 대상 등 */
    createBoolConfig("lazyfree-lazy-user-del", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_user_del , 0, NULL, NULL),  /* ▶ ★ DEL 명령 */
    createBoolConfig("lazyfree-lazy-user-flush", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_user_flush , 0, NULL, NULL),  /* ▶ FLUSHDB / FLUSHALL (ASYNC/SYNC 옵션이 없을 때) */
