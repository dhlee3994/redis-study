/* ======================================================================
 * [해설판] t_string.c 발췌 — String 값의 인코딩이 정해지고 바뀌는 길
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/t_string.c (1678줄)
 *   함께: networking.c (명령 인자 객체 만들기), db.c (키스페이스에 넣기, 값 교체)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본에서 인코딩과 관련된 경로만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 파일과 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *   먼저 볼 파일: encoding/object_발췌.c [2], [3]
 *
 * ▶ SET k v 한 번에 값 객체가 거치는 길
 *
 *   [1] networking.c  인자를 robj로   44바이트 이하 → embstr, 그 이상 → raw (32KB 이상은 쿼리 버퍼째로 raw)
 *         │
 *   [2] setCommand    tryObjectEncoding   정수면 int, 44바이트 이하 raw는 embstr로
 *         │
 *   [3] db.c          dbAdd / dbSetValue → kvobjSet
 *                     키를 붙인 kvobj로 옮기면서 "키+값 ≤ 41바이트"면 embstr, 아니면 raw
 *                     (41 = CACHE_LINE_SIZE 64 기준. Apple Silicon macOS 직접 빌드는 128이라 105바이트.
 *                      Docker 같은 리눅스 환경은 64)
 *         │
 *       키스페이스에 저장. OBJECT ENCODING은 이 결과를 보여 줌
 *
 * ▶ 저장된 뒤에 인코딩이 바뀌는 경우
 *   INCR   : int이고 refcount 1이면 그 자리에서 숫자만 바꿈 (할당 없음)        [4]
 *   APPEND, SETRANGE : int/embstr이면 raw로 바꾼 뒤 고침. 한 번 raw면 짧아져도 raw로 남음   [5]
 *   SET으로 덮어쓰기 : 새 값 기준으로 처음부터 다시 정함                           [3-2]
 *
 * ▶ 목차
 *   [1] 명령 인자 객체 만들기    (networking.c)
 *   [2] SET 계열 명령            : 저장 전에 tryObjectEncoding
 *   [3] 키스페이스에 넣기         (db.c) dbAddInternal, dbSetValue      ★
 *   [4] INCR                     : int 인코딩의 이점
 *   [5] APPEND와 dbUnshareStringValue : 고쳐 쓰려면 raw여야 한다        ★
 * ====================================================================== */


/* ######################################################################
 * [1] 명령 인자 객체 만들기 — networking.c
 *   RESP로 받은 인자마다 robj를 만듭니다. 이 시점의 인코딩은 길이로만 정해집니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] processMultibulkBuffer 일부 — networking.c L3531~L3558
 * ====================================================================== */
            /* Optimization: if a non-master client's buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (!(c->flags & CLIENT_MASTER) &&
                c->qb_pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&  /* ▶ 32KB 이상 큰 인자이고 버퍼에 이 인자만 있으면 */
                querybuf_len == (size_t)(c->bulklen+2))
            {
                (pcmd->argv)[(pcmd->argc)++] = createObject(OBJ_STRING,c->querybuf);  /* ▶ ★ 쿼리 버퍼 sds를 복사 없이 그대로 값으로 씀 → raw */
                pcmd->argv_len_sum += c->bulklen;
                c->all_argv_len_sum += c->bulklen;
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one likely...
                 * But only if that fat argument is not too big compared to the memory limit. */
                if (!server.maxmemory || (size_t)c->bulklen < server.maxmemory / 32) {
                    c->querybuf = sdsnewlen(SDS_NOINIT,c->bulklen+2);  /* ▶ 다음 큰 인자를 위해 쿼리 버퍼를 새로 할당 */
                } else {
                    c->querybuf = sdsnewlen(SDS_NOINIT, PROTO_IOBUF_LEN);
                }
                sdsclear(c->querybuf);
                querybuf_len = sdslen(c->querybuf); /* Update cached length */
            } else {
                (pcmd->argv)[(pcmd->argc)++] =
                    createStringObject(c->querybuf+c->qb_pos,c->bulklen);  /* ▶ ★ 보통의 인자: 44바이트 이하면 embstr, 아니면 raw (object_발췌.c [2-2]) */
                pcmd->argv_len_sum += c->bulklen;
                c->all_argv_len_sum += c->bulklen;
                c->qb_pos += c->bulklen+2;
            }


/* ######################################################################
 * [2] SET 계열 명령 — 저장 전에 tryObjectEncoding
 *   값 인자를 키스페이스에 넣기 전에 가능한 한 작은 인코딩으로 바꿉니다 (object_발췌.c [3-1]).
 *   키 인자(argv[1])는 바꾸지 않습니다. 키는 kvobj 안에 문자열로 복사되기 때문입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] setCommand, setnxCommand, setexCommand, psetexCommand — t_string.c L426~L454
 * ====================================================================== */
/* SET key value [NX] [XX] [KEEPTTL] [GET] [EX <seconds>] [PX <milliseconds>]
 *     [EXAT <seconds-timestamp>][PXAT <milliseconds-timestamp>]
 *     [IFEQ <match-value>|IFNE <match-value>|IFDEQ <match-digest>|
 *      IFDNE <match-digest>]*/
void setCommand(client *c) {
    extendedStringArgs args;

    if (parseExtendedStringArgumentsOrReply(c, 3, &args, COMMAND_SET) != C_OK) {
        return;
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]);  /* ▶ ★ "12345" → int, 44바이트 이하 → embstr */
    setGenericCommand(c, args.flags, c->argv[1], &(c->argv[2]), args.expire, args.unit, args.match_value, NULL, NULL);  /* ▶ 값 객체를 주소(&argv[2])로 넘김. 저장하면서 다른 객체로 바뀔 수 있기 때문 */
}

void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c, OBJ_SET_NX, c->argv[1], &(c->argv[2]), NULL, 0, NULL, shared.cone, shared.czero);
}

void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_EX, c->argv[1], &(c->argv[3]), c->argv[2], UNIT_SECONDS, NULL, NULL, NULL);
}

void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_PX, c->argv[1], &(c->argv[3]), c->argv[2], UNIT_MILLISECONDS, NULL, NULL, NULL);
}

/* ======================================================================
 * ▶ [2-2] setGenericCommand의 저장 부분 — t_string.c L177~L188 (함수는 L229까지 이어짐)
 * ====================================================================== */
    /* When expire is not NULL, we avoid deleting the TTL so it can be updated later instead of being deleted and then created again. */
    setkey_flags |= ((flags & OBJ_KEEPTTL) || expire) ? SETKEY_KEEPTTL : 0;
    setkey_flags |= found ? SETKEY_ALREADY_EXIST : SETKEY_DOESNT_EXIST;

    setKeyByLink(c, c->db, key, valref, setkey_flags, &link);  /* ▶ ★ db.c setKeyByLink → 새 키면 dbAdd, 있으면 dbSetValue ([3]) */
    /* If there's an expiration, setExpireByLink may reallocate the object.
     * We must update valref to reflect the new object if that happens. */
    if (expire) *valref = setExpireByLink(c, c->db, key->ptr, milliseconds, link);  /* ▶ 만료 시간을 붙이면 앞쪽에 8바이트 메타데이터가 생기며 다시 할당될 수 있음 */
    /* The client still holds a reference to the original object via c->argv[i],
     * and will call decrRefCount() at the end of call(). We increment the refcount
     * from 1 to 2 to ensure both DB and client have valid references. */
    incrRefCount(*valref); /* 1->2 */  /* ▶ 이제 *valref는 kvobj. 클라이언트 argv도 같은 객체를 가리키므로 refcount 2 */
    /* ▶ [생략] 원본 L189~L229: dirty 증가, 키스페이스 알림, PXAT 전파 재작성, 응답 */


/* ######################################################################
 * [3] 키스페이스에 넣기 — db.c
 *   새 키는 dbAddInternal, 기존 키 덮어쓰기는 dbSetValue로 갑니다. 새 키와, 덮어쓰기 중 (나) 경로가
 *   kvobjSet을 불러 "키를 붙인 kvobj"를 만들고, 이때 embstr 여부가 키 길이를 포함해 다시 결정됩니다.
 *   embstr 값은 항상 (나) 경로로 갑니다. ((가) 경로는 옛 kvobj에 ptr과 encoding만 바꿔 낌, [3-2])
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] dbAddInternal — db.c L414~L471
 * ====================================================================== */
/* Add a key-value entry to the DB.
 *
 * A copy of 'key' is stored in the database. The caller must ensure the
 * `key` is properly freed by calling decrRefcount(key).
 *
 * The value may (if its reference counter == 1) be reallocated and become
 * invalid after a call to this function. The (possibly reallocated) value is
 * stored in the database and the 'valref' pointer is updated to point to the
 * new allocation.
 *
 * The reference counter of the value pointed to by valref is not incremented,
 * so the caller should not free the value using decrRefcount after calling this
 * function.
 *
 * link - Optional link to bucket where the key should be added.
 *          On return, get updated, by need, to the inserted key.
 *          
 * spec - Defines attributes and metadata of the new key, including NO-EVICT,
 *           optional expiration and module metadata (REQUIRED).
 */
kvobj *dbAddInternal(redisDb *db, robj *key, robj **valref, dictEntryLink *link, 
                     const kvSpec *spec)
{
    int slot = getKeySlot(key->ptr);
    dictEntryLink tmp = NULL;
    if (link == NULL) link = &tmp;
    robj *val = *valref;
    kvobj *kv = kvobjSet(key->ptr, val, spec->metabits);  /* ▶ ★ kvobjSet (object_발췌.c [2-4]). embstr 값은 여기서 embstr로 남거나 raw가 됨 */
    initObjectLRUOrLFU(kv);
    kvstoreDictSetAtLink(db->keys, slot, kv, link, 1);  /* ▶ 메인 dict에 kvobj 포인터 저장 */
    /* ▶ [생략] 원본 L444~L468: 만료 시간, 모듈 메타데이터, no-evict, 키스페이스 알림, 통계 */
    *valref = kv;
    return kv;
}

/* ======================================================================
 * ▶ [3-2] dbSetValue — 기존 키의 값 바꾸기 (SET 덮어쓰기 등) — db.c L597~L734
 *   두 가지 길이 있습니다.
 *     (가) 옛 값과 새 값이 둘 다 embstr이 아니고 refcount 1이면: 옛 kvobj를 그대로 두고 type/encoding/ptr만
 *          바꿔 낌. 키를 다시 복사하지 않아도 됨
 *     (나) 그 밖: kvobjSet으로 새 kvobj를 만들어 dict 칸을 갈아 끼움
 *   마지막에 옛 값을 해제합니다. lazyfree-lazy-server-del이 꺼져 있으면(기본) 메인 스레드에서 바로 해제하므로,
 *   큰 Hash를 SET으로 덮어쓰면 그 Hash의 필드 수만큼 free가 이 명령 안에서 돕니다 (lazyfree_발췌.c).
 * ====================================================================== */
static void dbSetValue(redisDb *db, robj *key, robj **valref, dictEntryLink link, 
                       int overwrite, int updateKeySizes, int keepTTL) {
    int freeModuleMeta = 0;
    robj *val = *valref;
    int slot = getKeySlot(key->ptr);
    size_t oldsize = 0;
    if (!link) {
        link = kvstoreDictFindLink(db->keys, slot, key->ptr, NULL);
        serverAssertWithInfo(NULL, key, link != NULL); /* expected to exist */
    }
    kvobj *old = dictGetKV(*link);
    kvobj *kvNew;

    int64_t oldlen = (int64_t) getObjectLength(old);
    int oldtype = old->type;

    /* ▶ [생략] 원본 L613~L651: HFE / stream 정리, 만료 시간 처리 준비, 덮어쓰기 알림 */
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(old);

    if ((old->refcount == 1 && old->encoding != OBJ_ENCODING_EMBSTR) &&  /* ▶ ★ (가) 둘 다 embstr이 아니면 옛 kvobj 껍데기를 재사용 */
        (val->refcount == 1 && val->encoding != OBJ_ENCODING_EMBSTR) && (!freeModuleMeta))
    {
        /* Keep old object in the database. Just swap it's ptr, type and
         * encoding with the content of val. */
        robj tmp = *old;
        old->type = val->type;
        old->encoding = val->encoding;  /* ▶ 인코딩도 새 값 것으로 */
        old->ptr = val->ptr;
        val->type = tmp.type;
        val->encoding = tmp.encoding;
        val->ptr = tmp.ptr;
        /* Set new to old to keep the old object. Set old to val to be freed below. */
        kvNew = old;
        old = val;  /* ▶ 이제 old가 가리키는 것은 "새 값의 껍데기 + 옛 데이터". 아래에서 해제 */

        /* Handle TTL in the optimization path */
        if ((!keepTTL) && (oldExpire >= 0))
            removeExpire(db, key);
    } else {
        /* Replace the old value at its location in the key space. */
        val->lru = old->lru;
        
        kvNew = kvobjSet(key->ptr, val, newKeyMetaBits);  /* ▶ ★ (나) embstr이 끼면 kvobjSet으로 새로 만듦 (키 복사 포함) */
        kvobjBits(kvNew)->no_evict = kvobjBits(old)->no_evict;
        kvstoreDictSetAtLink(db->keys, slot, kvNew, &link, 0);  /* ▶ dict 칸을 새 kvobj로 교체 */

        /* if expiry replace the old value at its location in the expire space. */
        if (oldExpire != -1) {
            if (keepTTL) {
                kvobjSetExpire(kvNew, oldExpire); /* kvNew not reallocated here */
                dictEntryLink exLink = kvstoreDictFindLink(db->expires, slot,
                                                           key->ptr, NULL);
                serverAssertWithInfo(NULL, key, exLink != NULL);
                kvstoreDictSetAtLink(db->expires, slot, kvNew, &exLink, 0);
            } else {
                kvstoreDictDelete(db->expires, slot, key->ptr);
            }
        }

        if (newKeyMetaBits & KEY_META_MASK_MODULES)
            keyMetaTransition(old, kvNew);
    }

    /* ▶ [생략] 원본 L699~L720: KEYSIZES 통계, 메모리 추적 */
    if (server.io_threads_num > 1 && old->encoding == OBJ_ENCODING_RAW && old->refcount == 1) {
        /* In multi-threaded mode, the OBJ_ENCODING_RAW string object usually is
         * allocated in the IO thread, so we defer the free to the IO thread.
         * Besides, we never free a string object in BIO threads, so, even with
         * lazyfree-lazy-server-del enabled, a fallback to main thread freeing
         * due to defer free failure doesn't go against the config intention. */
        tryDeferFreeClientObject(server.current_client, DEFERRED_OBJECT_TYPE_ROBJ, old);  /* ▶ I/O 스레드를 쓰면 raw 문자열은 I/O 스레드에서 해제 */
    } else if (server.lazyfree_lazy_server_del) {
        freeObjAsync(key, old, db->id);  /* ▶ lazyfree-lazy-server-del yes: 해제 비용(effort)이 64를 넘고 refcount 1인 값은 백그라운드 해제 (List는 노드 수 기준) */
    } else {
        decrRefCount(old);  /* ▶ ★ 기본값(no): 여기서 바로 해제. 옛 값이 큰 HT Hash면 O(N) */
    }
    *valref = kvNew;
}


/* ######################################################################
 * [4] INCR — int 인코딩의 이점
 *   값이 int이고 아무도 공유하지 않으면(refcount 1) ptr 자리의 숫자만 바꿉니다. 할당도 해제도 없습니다.
 *   그렇지 않으면 새 int 객체를 만들어 값을 교체합니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [4-1] incrDecrCommand — t_string.c L896~L934
 * ====================================================================== */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *new;
    dictEntryLink link;
    kvobj *o = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (checkType(c,o,OBJ_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;  /* ▶ 문자열이어도 정수로 읽히면 됨 ("10"이 embstr이든 raw든) */

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&  /* ▶ ★ 이미 int이고 공유 안 됨 */
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);  /* ▶ ★ 제자리 수정. 할당 0번 */
        updateKeysizesHist(c->db, OBJ_STRING,
                           (int64_t) sdigits10(oldvalue),
                           (int64_t) sdigits10(value));
    } else {
        new = createStringObjectFromLongLongForValue(value);  /* ▶ 그 밖: 0~9999면 공유 정수, 아니면 새 int 객체 */
        if (o) {
            /* replace value in db and also update keysizes hist */
            dbReplaceValueWithLink(c->db, c->argv[1], &new, link);  /* ▶ db.c dbSetValue ([3-2]) */
        } else {
            /* Add new key to db and also update keysizes hist */
            dbAddByLink(c->db, c->argv[1], &new, &link);
        }
    }
    addReplyLongLongFromStr(c,new);
    keyModified(c,c->db,c->argv[1],new,1);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
}


/* ######################################################################
 * [5] APPEND와 dbUnshareStringValue — 고쳐 쓰려면 raw여야 한다
 *   embstr은 robj와 한 덩어리라 늘릴 수 없고, int는 문자열이 아예 없습니다. 그래서 값을 고치는 명령
 *   (APPEND, SETRANGE, SETBIT ...)은 먼저 raw 사본으로 바꿉니다. 이후 값이 짧아져도 embstr로 돌아가지
 *   않습니다. 같은 "abc"라도 SET으로 넣으면 embstr, APPEND로 만들면 raw일 수 있는 이유입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [5-1] appendCommand — t_string.c L1364~L1405
 * ====================================================================== */
void appendCommand(client *c) {
    size_t totlen;
    robj *append;
    kvobj *o;
    size_t oldsize = 0;

    dictEntryLink link;
    o = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
    if (o == NULL) {
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]);  /* ▶ 새 키: SET과 같은 길 (int / embstr / raw) */
        o = dbAddByLink(c->db, c->argv[1], &c->argv[2], &link);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        size_t append_len = sdslen(append->ptr);
        if (checkStringLength(c,stringObjectLen(o),append_len) != C_OK)
            return;

        /* Append the value */
        o = dbUnshareStringValueByLink(c->db,c->argv[1],o,link);  /* ▶ ★ 기존 값이 int나 embstr이면 여기서 raw로 바뀜 */
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(o);
        o->ptr = sdscatlen(o->ptr,append->ptr,append_len);  /* ▶ raw sds 뒤에 이어 붙임. sds는 여유 공간을 두고 늘어나므로 반복 APPEND가 싸짐 */
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
        totlen = sdslen(o->ptr);
        int64_t oldlen = totlen - append_len;
        updateKeysizesHist(c->db, OBJ_STRING, oldlen, totlen);
    }
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;

    addReplyLongLong(c,totlen);
}

/* ======================================================================
 * ▶ [5-2] dbUnshareStringValue, dbUnshareStringValueByLink — db.c L964~L1006
 * ====================================================================== */
/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
kvobj *dbUnshareStringValue(redisDb *db, robj *key, kvobj *kv) {
    return dbUnshareStringValueByLink(db,key,kv,NULL);
}

/* Like dbUnshareStringValue(), but accepts a optional link,
 * which can be used if we already have one, thus saving the dbFind call. */
kvobj *dbUnshareStringValueByLink(redisDb *db, robj *key, kvobj *o, dictEntryLink link) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {  /* ▶ ★ 공유 중이거나 raw가 아니면 */
        robj *decoded = getDecodedObject(o);  /* ▶ int면 숫자를 문자열로 만듦 */
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));  /* ▶ ★ 항상 raw로 새로 만듦 (길이가 짧아도) */
        decrRefCount(decoded);
        dbReplaceValueWithLink(db, key, &o, link);  /* ▶ dbSetValue: 옛 값이 embstr이면 kvobjSet으로 새로 만들고, 아니면 옛 kvobj에 ptr만 바꿔 낌. 어느 쪽이든 결과는 raw */
    }
    return o;
}
