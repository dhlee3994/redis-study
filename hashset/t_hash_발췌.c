/* ======================================================================
 * [해설판] t_hash.c 발췌 — Hash의 listpack → dict 변환과 조회
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/t_hash.c (6910줄)
 *   부록: object.c, config.c
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 6910줄 중 인코딩 선택, 변환, 조회만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *
 * ▶ t_hash.c가 긴 이유와 이번에 뺀 것
 *   - 필드 TTL (HEXPIRE 등, "HFE"): OBJ_ENCODING_LISTPACK_EX 인코딩과 ebuckets 자료구조
 *   - 해시 템플릿 (OBJ_ENCODING_TMPL_LP / TMPL_ARRAY): 8.10부터 들어온 기능.
 *     필드 이름이 같은 Hash 여러 개가 필드 이름을 한 벌만 공유합니다. HIMPORT 명령과 RDB 로드에서
 *     쓰이고, HSET의 자동 변환은 hash-min-template-entries(기본 0 = 꺼짐)를 켜야 동작합니다.
 *   두 기능 모두 코드 곳곳에 분기로 끼어 있어서, 아래에서는 그 분기를 [생략]으로 표시했습니다.
 *
 * ▶ 이번에 볼 인코딩 2종
 *   OBJ_ENCODING_LISTPACK : [필드1][값1][필드2][값2]... 연속 메모리.  조회 O(N) 필드만 골라 훑기
 *   OBJ_ENCODING_HT       : dict (entryHashDictType).                  조회 평균 O(1)
 *   기본 기준값 (config.c): hash-max-listpack-entries 512, hash-max-listpack-value 64바이트
 *   명령 실행 중의 변환은 listpack → HT 한 방향뿐입니다. hashTypeConvert에 HT → listpack은 구현돼 있지
 *   않습니다([5]). 예외: RDB 로딩(재시작, DEBUG RELOAD, RESTORE, 복제 full sync)은 크기 기준을 다시
 *   적용해서 512개 이하면 listpack으로 만들고, hash-min-template-entries를 켜면 HT가 템플릿으로 바뀔 수 있습니다.
 *
 * ▶ 목차
 *   [1] entryHashDictType       : 큰 Hash 안의 dict는 어떤 모양인가
 *   [2] HSET 경로               : hsetCommand → hashTypeTryConversion → hashTypeSet
 *   [3] hashTypeTryConversion   : 넣기 "전에" 인자 크기로 변환   ★
 *   [4] hashTypeSet             : 넣은 "후에" 개수로 변환        ★
 *   [5] hashTypeConvert / hashTypeConvertListpack : 한 번에 옮기기, dict는 미리 최종 크기로
 *   [6] HGET 경로               : 인코딩별 조회 비용             ★
 *   [7] hashTypeDelete          : 지워도 listpack으로 돌아가지 않음
 *   [부록] 객체 생성 함수, 기준값 설정
 * ====================================================================== */


/* ######################################################################
 * [1] entryHashDictType — t_hash.c L77~L97
 *   필드와 값을 Entry 객체 하나에 같이 담고, dict는 Entry 포인터만 저장합니다(no_value=1).
 *   단 값까지 Entry 안에 넣는 것은 필드+값 할당이 128바이트 이하일 때뿐입니다. 그보다 크면 Entry에는
 *   값 포인터만 두고 값 sds를 따로 할당합니다(이 경우 필드 하나에 할당 2개, 해제도 2번).
 *   메인 키 공간이 kvobj 하나에 키와 값을 담는 것과 같은 발상입니다.
 *
 *   메인 키 공간의 dbDictType과 다른 점:
 *     - resizeAllowed 콜백이 없습니다 → 확장할 때 maxmemory 검사를 하지 않음
 *     - kvstore에 속하지 않습니다 → databasesCron의 1ms rehash도, resize 검사도 받지 않음
 *       이 dict의 rehash는 이 Hash에 HGET/HSET/HDEL 같은 접근이 있을 때만 한 걸음씩 진행됩니다.
 *     - 전역 resize 정책(dict_can_resize, fork 중 AVOID)은 똑같이 적용됩니다.
 *   (server.c에 있는 hashDictType은 이름만 비슷하고, Hash 객체에는 이 entryHashDictType을 씁니다.)
 * ###################################################################### */

/*-----------------------------------------------------------------------------
 * Define dictType of hash
 *
 * - Stores fields as entry objects (field-value pairs) with optional expiration
 * - Note that small hashes are represented with listpacks
 * - Once expiration is set for a field, the dict instance and corresponding
 *   dictType are replaced with a dict containing metadata for Hash Field
 *   Expiration (HFE) and using dictType `entryHashDictTypeWithHFE`
 * - Dict uses no_value=1 since entry contains both field and value
 *----------------------------------------------------------------------------*/
dictType entryHashDictType = {
    dictSdsHash,                                /* lookup hash function */  /* ▶ 필드 이름(sds)으로 SipHash */
    NULL,                                       /* key dup */
    NULL,                                       /* val dup */
    dictSdsKeyCompare,                          /* lookup key compare */  /* ▶ 길이 비교 → memcmp */
    dictEntryDestructor,                       /* key destructor */
    NULL,                                       /* val destructor (value is in entry) */
    .dictMetadataBytes = hashDictMetadataBytes,
    .no_value = 1,                              /* entry contains both field and value */  /* ▶ Entry 포인터 하나만 저장 */
    .keys_are_odd = 1,                          /* entry pointers (SDS) are always odd */  /* ▶ Entry 포인터는 필드 sds를 가리키므로 홀수 주소 → 버킷 칸에 직접 저장 가능 */
};


/* ######################################################################
 * [2] HSET 경로 — t_hash.c L3129~L3139, L4262~L4325
 *   새 Hash는 언제나 빈 listpack으로 시작합니다. 그다음 두 번 검사합니다:
 *     넣기 전: 이번 인자들의 개수와 길이 [3]
 *     넣은 후: 전체 필드 수 [4]
 * ###################################################################### */

static kvobj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, key, &link);  /* ▶ 키를 찾으면서 "없으면 넣을 자리"도 받음 */
    if (checkType(c, kv, OBJ_HASH)) return NULL;

    if (kv == NULL) {
        robj *o = createHashObject();  /* ▶ 새 Hash = 빈 listpack (부록 createHashObject) */
        kv = dbAddByLink(c->db, key, &o, &link);
    }
    return kv;
}

void hsetCommand(client *c) {
    int i, created = 0;
    size_t oldsize = 0;
    kvobj *kv;

    if ((c->argc % 2) == 1) {
        addReplyErrorArity(c);
        return;
    }

    if ((kv = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(kv);
    hashTypeTryConversion(c->db, kv, c->argv, 2, c->argc-1);  /* ▶ ★ 넣기 전에 인자 크기로 변환 검사 [3] */

    int numfields = (c->argc - 2) / 2;
    /* The fresh-build fast path pays for a transient dedup dict, which only pays
     * off once the field count is large enough; 5 fields is a reasonable
     * threshold, below it the per-field loop is cheaper. */
    if (kv->encoding == OBJ_ENCODING_LISTPACK && numfields >= 5 && lpLength(kv->ptr) == 0) {  /* ▶ 빈 listpack에 5개 이상을 한 번에 넣을 때의 일괄 추가 경로 */
        /* Fresh wide build: single dict pass (last-wins) + one batch append. */
        created = hashTypeBuildFreshListpack(kv, c->argv + 2, numfields);
    } else if (hashTypeIsTemplate(kv)) {
        /* Template hash: existing fields are overwritten in place and the
         * missing ones are added in a single template switch. */
        created = hashTypeTmplSetFields(c->db, kv, c->argv + 2, numfields);
    } else {
        for (i = 2; i < c->argc; i += 2)
            created += !hashTypeSet(c->db, kv, c->argv[i]->ptr, c->argv[i+1]->ptr, HASH_SET_COPY | HASH_SET_NO_TEMPLATE_CONVERT);  /* ▶ 필드 하나씩 [4] */
    }

    /* Convert to template once after all fields are set, rather than per-field,
     * to avoid a template lookup for each field set. */
    hashTypeTryConvertToTemplate(kv, server.hash_min_template_entries,  /* ▶ 템플릿 변환. hash-min-template-entries 기본 0이면 아무것도 안 함 */
                                 server.hash_max_template_entries, NULL);


    /* ▶ [생략] 원본 L4300~L4324: 응답, 키스페이스 통계, 알림 */
}


/* ######################################################################
 * [3] hashTypeTryConversion — t_hash.c L1934~L1978   ★
 *   인자를 실제로 넣기 전에 "넣으면 기준을 넘을지"를 봅니다. 넘으면 지금 바로 dict로 바꿉니다.
 *   listpack에 넣었다가 곧바로 다시 옮기는 낭비를 피하려는 것입니다.
 * ###################################################################### */

/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(redisDb *db, kvobj *o, robj **argv, int start, int end) {
    int tmpl_lp = (o->encoding == OBJ_ENCODING_TMPL_LP);
    int target_enc = tmpl_lp ? OBJ_ENCODING_TMPL_ARRAY : OBJ_ENCODING_HT;

    /* TMPL_ARRAY and HT don't need conversion checks. */
    if (o->encoding != OBJ_ENCODING_LISTPACK &&
        o->encoding != OBJ_ENCODING_LISTPACK_EX &&
        o->encoding != OBJ_ENCODING_TMPL_LP)
    {
        return;  /* ▶ 이미 dict면 검사할 것 없음 */
    }

    /* Determine target encoding for conversion. */
    if (!tmpl_lp) {
        /* Check field count limit for regular listpack. */
        size_t new_fields = (end - start + 1) / 2;
        if (new_fields > server.hash_max_listpack_entries) {  /* ▶ 이번에 넣을 필드가 512개를 넘으면 */
            hashTypeConvert(db, o, OBJ_ENCODING_HT);  /* ▶ 지금 것을 dict로 바꾸고 [5] */
            dictExpand(o->ptr, new_fields);  /* ▶ 넣을 개수만큼 미리 키워 둠 */
            return;
        }
    }

    /* Check value sizes (and field sizes for non-TMPL_LP). */
    size_t sum = 0;
    for (int i = start; i <= end; i++) {
        if (tmpl_lp && ((i - start) % 2 == 0))
            continue;
        if (!sdsEncodedObject(argv[i]))
            continue;  /* ▶ 정수로 인코딩된 인자는 짧으므로 건너뜀 */
        size_t len = sdslen(argv[i]->ptr);
        if (len > server.hash_max_listpack_value) {  /* ▶ 필드나 값 하나라도 64바이트를 넘으면 */
            hashTypeConvert(db, o, target_enc);  /* ▶ dict로 */
            return;
        }
        sum += len;
    }
    unsigned char *lp = (o->encoding == OBJ_ENCODING_LISTPACK_EX) ?
                        ((listpackEx*)o->ptr)->lp : o->ptr;
    if (!lpSafeToAdd(lp, sum))
        hashTypeConvert(db, o, target_enc);  /* ▶ listpack 전체 크기 상한을 넘을 것 같아도 dict로 */
}


/* ######################################################################
 * [4] hashTypeSet — t_hash.c L2275~L2451   ★
 *   listpack : 필드를 처음부터 찾고(O(N)), 없으면 끝에 붙인 뒤 "512개를 넘었나" 확인
 *   HT       : dict.c의 평범한 조회 + 삽입 (rehash 한 걸음, 확장 검사 포함)
 * ###################################################################### */

int hashTypeSet(redisDb *db, kvobj *o, sds field, sds value, int flags) {
    int update = 0;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);  /* ▶ skip=1: 값 칸은 건너뛰고 필드 칸만 비교. O(N) */
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char*)value, sdslen(value));  /* ▶ 이미 있으면 값만 교체 */
                update = 1;
            }
        }

        if (!update) {
            listpackEntry entries[2] = {
                {.sval = (unsigned char*) field, .slen = sdslen(field)},
                {.sval = (unsigned char*) value, .slen = sdslen(value)},
            };

            /* Push new field/value pair onto the tail of the listpack */
            zl = lpBatchAppend(zl, entries, 2);  /* ▶ 없으면 끝에 붙임. realloc 때문에 크기에 비례한 복사가 생길 수 있음 */
        }
        o->ptr = zl;

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o, 0) > server.hash_max_listpack_entries)  /* ▶ ★ 512개를 넘었으면 */
            hashTypeConvert(db, o, OBJ_ENCODING_HT);  /* ▶ dict로 [5] */
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        /* ▶ [생략] 원본 L2311~L2349: LISTPACK_EX(필드 TTL이 있는 listpack) 처리. 구조는 위 LISTPACK과 같음 */
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dict *ht = o->ptr;
        /* check if field already exists */
        dictEntryLink bucket, link = dictFindLink(ht, field, &bucket);  /* ▶ dict 발췌 [2-1]. 조회하면서 rehash 한 걸음 */
        size_t *alloc_size = htGetMetadataSize(ht);

        /* take ownership of value if requested */
        uint32_t newEntryFlags = flags & HASH_SET_TAKE_VALUE;
        flags &= ~HASH_SET_TAKE_VALUE;

        if (link == NULL) {
            /* Create entry and transfer value ownership if possible */
            size_t usable;
            Entry *newEntry = entryCreate(field, value, newEntryFlags, &usable);  /* ▶ 필드 + 값을 Entry 하나로 */

            dictSetKeyAtLink(ht, newEntry, &bucket, 1);  /* ▶ dict 발췌 [3-1]. 넣기 직전 확장 검사 */
            *alloc_size += usable;
        } else {
            /* ▶ [생략] 원본 L2368~L2404: 기존 필드의 값 바꾸기 (필드 TTL 처리 포함) */
        }
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        /* ▶ [생략] 원본 L2409~L2429: 템플릿 인코딩 처리 */
    } else {
        serverPanic("Unknown hash encoding");
    }

cleanup:
    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);

    /* Auto-convert to template if threshold met and not already template.
     * Skipped when HASH_SET_NO_TEMPLATE_CONVERT is set: multi-field callers defer
     * this to a single post-loop conversion to avoid per-field template lookup. */
    if (!(flags & HASH_SET_NO_TEMPLATE_CONVERT) &&  /* ▶ 템플릿 자동 변환. 기본 설정(0)에서는 들어가지 않음 */
        server.hash_min_template_entries > 0)
    {
        hashTypeTryConvertToTemplate(o, server.hash_min_template_entries,
                                    server.hash_max_template_entries, NULL);
    }

    return update;
}


/* ######################################################################
 * [5] hashTypeConvert / hashTypeConvertListpack — t_hash.c L3422~L3436, L3272~L3326
 *   변환은 점진적이지 않습니다. 필드 전부를 한 번에 dict로 옮깁니다.
 *   변환 시점의 필드 수는 보통 기준값(512) 근처라 한 번의 비용은 작습니다.
 *   dict를 최종 크기로 먼저 키워 두므로(빈 dict라 즉시 교체) 옮기는 동안 rehash가 생기지 않습니다.
 * ###################################################################### */

void hashTypeConvert(redisDb *db, robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);  /* ▶ 일반 listpack → [아래] */
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        hashTypeConvertListpackEx(db, o, enc);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP) {
        hashTypeConvertTmplLp(o, enc);
    } else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTypeConvertTmplArray(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        serverPanic("Not implemented");  /* ▶ ★ hashTypeConvert에는 HT에서 나가는 경로가 없음. 명령 실행 중에는 dict로 남음.
                                            *   예외: RDB 로딩(512개 이하면 listpack으로 다시 만듦), 템플릿 설정 */
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeConvertListpack(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_LISTPACK_EX) {
        /* ▶ [생략] 원본 L3279~L3294: LISTPACK → LISTPACK_EX (필드 TTL을 처음 걸 때). 각 필드 뒤에 TTL 칸을 끼워 넣음 */
    } else if (enc == OBJ_ENCODING_HT) {
        hashTypeIterator hi;
        dict *dict;
        int ret;

        hashTypeInitIterator(&hi, o);
        dict = dictCreate(&entryHashDictType);  /* ▶ 필드 TTL이 없는 Hash용 dictType [1] */

        /* Presize the dict to avoid rehashing */
        dictExpand(dict,hashTypeLength(o, 0));  /* ▶ 최종 크기로 미리 → 옮기는 동안 확장/rehash 없음 */

        size_t usable, *alloc_size = htGetMetadataSize(dict);
        while (hashTypeNext(&hi, 0) != C_ERR) {
            Entry *entry = hashTypeCurrentObjectNewEntry(&hi, &usable);
            ret = dictAdd(dict, entry, NULL);  /* ▶ 하나씩 넣기 */
            if (ret != DICT_OK) {
                entryFree(entry, NULL); /* Needed for gcc ASAN */
                hashTypeResetIterator(&hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                    o->ptr,lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
            *alloc_size += usable;
        }
        hashTypeResetIterator(&hi);
        zfree(o->ptr);  /* ▶ 옛 listpack 해제 */
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
        serverPanic("Unknown hash encoding");
    }
}


/* ######################################################################
 * [6] HGET 경로 — t_hash.c L5184~L5191, L5161~L5182, L2068~L2175, L1980~L2050
 *   hgetCommand → lookupKeyReadOrReply(키 찾기: dict 발췌 [2])
 *              → addHashFieldToReply → hashTypeGetValue
 *                 ├ listpack : hashTypeGetFromListpack  O(N), N ≤ 512
 *                 └ HT       : hashTypeGetFromHashTable 평균 O(1)
 *   즉 HGET 한 번에 해시 조회가 두 번 일어납니다: 키 공간에서 키를 찾고, Hash 안에서 필드를 찾습니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [6-1] hgetCommand / addHashFieldToReply — t_hash.c L5184~L5191, L5161~L5182
 * ====================================================================== */
void hgetCommand(client *c) {
    kvobj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL ||  /* ▶ ① 키 공간에서 키 찾기 */
        checkType(c,o,OBJ_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]->ptr, HFE_LAZY_EXPIRE);  /* ▶ ② Hash 안에서 필드 찾기 */
}

static GetFieldRes addHashFieldToReply(client *c, kvobj *o, sds field, int hfeFlags) {
    if (o == NULL) {
        addReplyNull(c);
        return GETF_NOT_FOUND;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    GetFieldRes res = hashTypeGetValue(c->db, o, field, &vstr, &vlen, &vll, hfeFlags, NULL);  /* ▶ 인코딩별 조회 [6-2] */
    if (res == GETF_OK) {
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }
    } else {
        addReplyNull(c);
    }
    return res;
}

/* ======================================================================
 * ▶ [6-2] hashTypeGetValue — t_hash.c L2052~L2175
 * ====================================================================== */
/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field.
 * Arguments:
 * hfeFlags      - Lookup for HFE_LAZY_* flags
 *
 * Returned:
 * GetFieldRes  - Result of get operation
 * vstr, vlen   - if string, ref in either *vstr and *vlen if it's
 *                returned in string form,
 * vll          - or stored in *vll if it's returned as a number.
 *                If *vll is populated *vstr is set to NULL, so the caller can
 *                always check the function return by checking the return value
 *                for GETF_OK and checking if vll (or vstr) is NULL.
 * expiredAt    - if the field has an expiration time, it will be set to the expiration 
 *                time of the field. Otherwise, will be set to EB_EXPIRE_TIME_INVALID.
 */
GetFieldRes hashTypeGetValue(redisDb *db, kvobj *o, sds field, unsigned char **vstr,
                                   unsigned int *vlen, long long *vll, 
                                   int hfeFlags, uint64_t *expiredAt)
{
    sds key = kvobjGetKey(o);
    GetFieldRes res;
    uint64_t dummy;
    size_t oldsize = 0;
    if (expiredAt == NULL) expiredAt = &dummy;
    if (o->encoding == OBJ_ENCODING_LISTPACK ||
        o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        *vstr = NULL;
        res = hashTypeGetFromListpack(o, field, vstr, vlen, vll, expiredAt);  /* ▶ listpack: 선형 탐색 [6-3] */

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value = NULL;
        if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
            oldsize = kvobjAllocSize(o);
        res = hashTypeGetFromHashTable(o, field, &value, expiredAt);  /* ▶ dict: dictFind [6-4] */
        if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
            updateSlotAllocSize(db, getKeySlot(key), o, oldsize, kvobjAllocSize(o));

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

        *vstr = (unsigned char*) value;
        *vlen = sdslen(value);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        /* ▶ [생략] 원본 L2101~L2113: 템플릿 인코딩 조회 (필드 위치를 이진 탐색으로 찾아 값 배열에서 읽음) */
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* ▶ [생략] 원본 L2118~L2174: TTL이 안 지났으면 GETF_OK. 지났으면 필드를 지우고(마지막 필드면 키도 삭제) 만료로 처리 */
}

/* ======================================================================
 * ▶ [6-3] hashTypeGetFromListpack — t_hash.c L1980~L2029
 * ====================================================================== */
/* Get the value from a listpack encoded hash, identified by field. */
GetFieldRes hashTypeGetFromListpack(robj *o, sds field,
                            unsigned char **vstr,
                            unsigned int *vlen,
                            long long *vll,
                            uint64_t *expiredAt)
{
    *expiredAt = EB_EXPIRE_TIME_INVALID;
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);  /* ▶ 필드 칸만 비교하며 처음부터 끝까지. O(N) */
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);  /* ▶ 필드 바로 다음 칸이 값 */
                serverAssert(vptr != NULL);
            }
        }
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        /* ▶ [생략] 원본 L2002~L2018: LISTPACK_EX: [필드][값][TTL] 3칸 단위로 같은 방식 */
    } else {
        serverPanic("Unknown hash encoding: %d", o->encoding);
    }

    if (vptr != NULL) {
        *vstr = lpGetValue(vptr, vlen, vll);
        return GETF_OK;
    }

    return GETF_NOT_FOUND;
}

/* ======================================================================
 * ▶ [6-4] hashTypeGetFromHashTable — t_hash.c L2031~L2050
 * ====================================================================== */
/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
GetFieldRes hashTypeGetFromHashTable(robj *o, sds field, sds *value, uint64_t *expiredAt) {
    dictEntry *de;

    *expiredAt = EB_EXPIRE_TIME_INVALID;

    serverAssert(o->encoding == OBJ_ENCODING_HT);

    de = dictFind(o->ptr, field);  /* ▶ dict 발췌 [2-1]. 평균 O(1) + rehash 중이면 한 걸음 */

    if (de == NULL)
        return GETF_NOT_FOUND;

    Entry *entry = dictGetKey(de);
    *expiredAt = entryGetExpiry(entry);
    *value = entryGetValue(entry);  /* ▶ 작은 값(필드+값 ≤ 128B)은 Entry 안에 있어 바로 읽고, 큰 값은 포인터를 한 번 더 따라감 */
    return GETF_OK;
}


/* ######################################################################
 * [7] hashTypeDelete — t_hash.c L2677~L2726
 *   어떤 인코딩이든 지우기만 합니다. 필드가 몇 개 안 남아도 dict는 dict로 남습니다.
 *   (dict 자체의 축소는 dict.c dictGenericDelete → _dictShrinkIfNeeded가 합니다.)
 * ###################################################################### */

int hashTypeDelete(robj *o, void *field) {
    int deleted = 0;
    int fieldLen = sdslen((sds)field);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, fieldLen, 1);  /* ▶ listpack: 찾기 O(N) + 필드/값 2칸 삭제 (뒤쪽을 당기는 복사) */
            if (fptr != NULL) {
                /* Delete both of the key and the value. */
                zl = lpDeleteRangeWithEntry(zl,&fptr,2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        unsigned char *fptr;
        listpackEx *lpt = o->ptr;

        fptr = lpFirst(lpt->lp);
        if (fptr != NULL) {
            fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, fieldLen, 2);
            if (fptr != NULL) {
                /* Delete field, value and ttl */
                lpt->lp = lpDeleteRangeWithEntry(lpt->lp, &fptr, 3);
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* dictDelete() will call dictEntryDestructor() */
        if (dictDelete((dict*)o->ptr, field) == C_OK) {  /* ▶ dict 발췌 [7-1]. rehash 한 걸음 + 축소 검사 */
            deleted = 1;
        }
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        /* ▶ [생략] 원본 L2716~L2721: 템플릿 인코딩 처리 */
    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}


/* ######################################################################
 * [부록] 객체 생성 함수, 기준값 설정
 * ###################################################################### */

/* ======================================================================
 * ▶ createHashObject — object.c L494~L499
 * ====================================================================== */
robj *createHashObject(void) {
    unsigned char *zl = lpNew(0);
    robj *o = createObject(OBJ_HASH, zl);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

/* ======================================================================
 * ▶ 기준값 — config.c L3640, L3652 (512, 64가 기본값)
 *   CONFIG SET으로 바꿀 수 있습니다. 크게 올리면 메모리는 아끼지만 필드마다 O(N) 탐색이 길어집니다.
 * ====================================================================== */
    createSizeTConfig("hash-max-listpack-entries", "hash-max-ziplist-entries", MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_listpack_entries, 512, INTEGER_CONFIG, NULL, NULL),

    createSizeTConfig("hash-max-listpack-value", "hash-max-ziplist-value", MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_listpack_value, 64, MEMORY_CONFIG, NULL, NULL),
