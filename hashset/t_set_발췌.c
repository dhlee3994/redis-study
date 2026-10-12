/* ======================================================================
 * [해설판] t_set.c 발췌 — Set의 인코딩 3종과 변환
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/t_set.c (2047줄)
 *   부록: intset.h, intset.c, object.c, server.c, config.c
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본에서 인코딩 선택, 변환, 조회 부분만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 뺀 것: SPOP/SRANDMEMBER의 무작위 추출, SINTER/SUNION/SDIFF 같은 집합 연산, SSCAN
 *
 * ▶ Set의 인코딩 3종 (o->encoding)
 *   OBJ_ENCODING_INTSET   : 정수만, 정렬된 배열.          조회 O(log N) 이진 탐색
 *   OBJ_ENCODING_LISTPACK : 아무 문자열, 연속 메모리.     조회 O(N) 처음부터 훑기
 *   OBJ_ENCODING_HT       : dict (setDictType).           조회 평균 O(1)
 *   기본 기준값 (config.c): set-max-intset-entries 512, set-max-listpack-entries 128,
 *                          set-max-listpack-value 64바이트
 *
 * ▶ 변환 방향
 *   intset ──(정수 아닌 값, 작음)──▶ listpack ──(128개 초과 / 64바이트 초과)──▶ HT
 *   intset ──(512개 초과, 또는 정수 아닌 값인데 listpack 조건도 넘음)──────────────▶ HT
 *   거꾸로 가는 길은 SREM에는 없습니다. 예외는 [6](SINTERSTORE 결과, SPOP count)과
 *   RDB 로딩(재시작, DEBUG RELOAD, RESTORE: 크기 기준을 다시 적용해 intset/listpack으로 만듦)입니다.
 *
 * ▶ 목차
 *   [1] setTypeCreate / setTypeMaybeConvert / saddCommand : 처음 만들 때 인코딩 고르기
 *   [2] setTypeAddAux      : 넣다가 기준을 넘으면 변환   ★
 *   [3] setTypeIsMemberAux : 인코딩별 조회 비용         ★
 *   [4] setTypeConvertAndExpand : 변환은 한 번에, dict는 미리 최종 크기로
 *   [5] setTypeRemoveAux   : 지워도 다시 작은 인코딩으로 돌아가지 않음
 *   [6] 작은 인코딩으로 돌아가는 예외 두 곳
 *   [부록 A] intset : 이진 탐색, 끼워 넣기(memmove), 인코딩 업그레이드
 *   [부록 B] setDictType, 객체 생성 함수, 기준값 설정
 * ====================================================================== */


/* ######################################################################
 * [1] 처음 만들 때 인코딩 고르기 — t_set.c L40~L67, L626~L661
 *   size_hint = 이번 명령으로 넣을 원소 수 (SADD key a b c → 3).
 *   처음부터 크면 dict를 바로 만들고 최종 크기로 미리 키웁니다(빈 테이블이라 rehash 없이 즉시).
 * ###################################################################### */

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value, size_t size_hint) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK && size_hint <= server.set_max_intset_entries)  /* ▶ 첫 값이 정수이고 작으면 intset */
        return createIntsetObject();
    if (size_hint <= server.set_max_listpack_entries)  /* ▶ 작으면 listpack */
        return createSetListpackObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    dictExpand(o->ptr, size_hint);  /* ▶ 크면 dict를 만들고 size_hint 크기로 한 번에 할당 */
    return o;
}

/* Check if the existing set should be converted to another encoding based off the
 * the size hint. */
void setTypeMaybeConvert(robj *set, size_t size_hint) {
    if ((set->encoding == OBJ_ENCODING_LISTPACK && size_hint > server.set_max_listpack_entries)
        || (set->encoding == OBJ_ENCODING_INTSET && size_hint > server.set_max_intset_entries))
    {
        setTypeConvertAndExpand(set, OBJ_ENCODING_HT, size_hint, 1);  /* ▶ 기존 set도 이번에 넣을 개수가 기준을 넘으면 미리 dict로 */
    }
}

void saddCommand(client *c) {
    kvobj *set;
    int j, added = 0;
    dictEntryLink link;
    size_t oldsize = 0;

    set = lookupKeyWriteWithLink(c->db,c->argv[1], &link);  /* ▶ 키를 찾으면서 "없으면 넣을 자리"도 받음 (dict 발췌 [3-1]) */
    if (checkType(c,set,OBJ_SET)) return;
    
    if (set == NULL) {
        robj *o = setTypeCreate(c->argv[2]->ptr, c->argc - 2);  /* ▶ 새 키: 인자 개수를 size_hint로 */
        set = dbAddByLink(c->db, c->argv[1], &o, &link);
    } else {
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);
        setTypeMaybeConvert(set, c->argc - 2);  /* ▶ 기존 키: 인자 개수로 변환 필요 여부 검사 */
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    }

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    for (j = 2; j < c->argc; j++) {
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;  /* ▶ 원소 하나씩 [2] */
    }
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    /* ▶ [생략] 원본 L653~L658: 키스페이스 통계, 알림 */
    server.dirty += added;
    addReplyLongLong(c,added);
}


/* ######################################################################
 * [2] setTypeAddAux — t_set.c L120~L236   ★
 *   인코딩마다 넣는 방법이 다르고, 넣다가 기준을 넘으면 그 자리에서 변환합니다.
 *   HT로 한 번 바뀌면 이후 SADD는 dict.c의 평범한 삽입입니다 (rehash 한 걸음 + 확장 검사 포함).
 * ###################################################################### */

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, llval, &success);  /* ▶ intset에 정수 넣기: 이진 탐색 + memmove [부록 A] */
            if (success) maybeConvertIntset(set);  /* ▶ 512개를 넘었으면 HT로 */
            return success;
        }
        /* Convert int to string. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    serverAssert(str);
    if (set->encoding == OBJ_ENCODING_HT) {
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        dict *ht = set->ptr;
        dictEntryLink bucket, link = dictFindLink(ht, sdsval, &bucket);  /* ▶ dict 발췌 [2-1]. 조회하면서 rehash 한 걸음 */
        if (link == NULL) {
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);
            dictSetKeyAtLink(ht, sdsval, &bucket, 1);  /* ▶ dict 발췌 [3-1]. 넣기 직전 확장 검사 */
            *htGetMetadataSize(ht) += sdsAllocSize(sdsval);
            return 1;
        } else if (sdsval != str) {
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
            return 0;
        }
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        if (p != NULL)
            p = lpFind(lp, p, (unsigned char*)str, len, 0);  /* ▶ listpack은 처음부터 훑어서 찾음 O(N) */
        if (p == NULL) {
            /* Not found.  */
            /* ▶ 개수 128개 미만, 길이 64바이트 이하, listpack 전체 크기 제한 안쪽이면 listpack에 그대로 추가 */
            if (lpLength(lp) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                lpSafeToAdd(lp, len))
            {
                if (str == tmpbuf) {
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {
                    lp = lpAppend(lp, (unsigned char*)str, len);  /* ▶ 끝에 붙이기. 메모리를 다시 할당(realloc)하므로 크기에 비례한 복사가 생길 수 있음 */
                }
                set->ptr = lp;
            } else {
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT, lpLength(lp) + 1, 1);  /* ▶ ★ 기준 초과: 지금 원소 전부를 dict로 옮김 [4] */
                sds newval = sdsnewlen(str,len);
                serverAssert(dictAdd(set->ptr,newval,NULL) == DICT_OK);
                *htGetMetadataSize(set->ptr) += sdsAllocSize(newval);
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long value;
        if (string2ll(str, len, &value)) {  /* ▶ 정수로 바뀌면 intset에 */
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr,value,&success);
            if (success) {
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            /* ▶ 정수가 아닌 값이 intset에 들어옴 → intset으로는 못 담음.
             *   listpack 조건(개수, 길이, 전체 크기)을 만족하면 listpack, 아니면 바로 HT */
            /* Check if listpack encoding is safe not to cross any threshold. */
            size_t maxelelen = 0, totsize = 0;
            unsigned long n = intsetLen(set->ptr);
            if (n != 0) {
                size_t elelen1 = sdigits10(intsetMax(set->ptr));
                size_t elelen2 = sdigits10(intsetMin(set->ptr));
                maxelelen = max(elelen1, elelen2);
                size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
                size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
                totsize = max(s1, s2);
            }
            if (intsetLen((const intset*)set->ptr) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                maxelelen <= server.set_max_listpack_value &&
                lpSafeToAdd(NULL, totsize + len))
            {
                /* In the "safe to add" check above we assumed all elements in
                 * the intset are of size maxelelen. This is an upper bound. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK,  /* ▶ intset → listpack */
                                        intsetLen(set->ptr) + 1, 1);
                unsigned char *lp = set->ptr;
                lp = lpAppend(lp, (unsigned char *)str, len);
                lp = lpShrinkToFit(lp);
                set->ptr = lp;
                return 1;
            } else {
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT,  /* ▶ intset → HT */
                                        intsetLen(set->ptr) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so dictAdd should always work. */
                sds newval = sdsnewlen(str,len);
                serverAssert(dictAdd(set->ptr,newval,NULL) == DICT_OK);
                *htGetMetadataSize(set->ptr) += sdsAllocSize(newval);
                return 1;
            }
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}


/* ######################################################################
 * [3] setTypeIsMemberAux — t_set.c L297~L330   ★
 *   SISMEMBER의 본체. 같은 질문("있나?")인데 인코딩마다 비용이 다릅니다.
 *     listpack : lpFind로 처음부터 비교            O(N), N ≤ 128
 *     intset   : 이진 탐색                         O(log N), N ≤ 512
 *     HT       : dictFind                          평균 O(1)
 *   N이 작으면 listpack의 O(N)이 dict의 O(1)보다 느리지 않을 수 있습니다. 원소들이 연속된 메모리에
 *   있어서 캐시 라인 몇 개만 읽으면 되고, dict는 버킷 배열 → sds(충돌이면 엔트리를 하나 더)로 메모리를 여기저기 읽기 때문입니다.
 * ###################################################################### */

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET)
            return intsetFind(set->ptr, llval);  /* ▶ 정수로 물어보면 intset은 바로 이진 탐색 */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        return p && lpFind(lp, p, (unsigned char*)str, len, 0);  /* ▶ 선형 탐색 */
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(set->ptr, llval);  /* ▶ 정수로 바꿀 수 없으면 intset에는 없음 */
    } else if (set->encoding == OBJ_ENCODING_HT && str_is_sds) {
        return dictFind(set->ptr, (sds)str) != NULL;  /* ▶ dict.c [2] 조회 */
    } else if (set->encoding == OBJ_ENCODING_HT) {
        sds sdsval = sdsnewlen(str, len);
        int result = dictFind(set->ptr, sdsval) != NULL;
        sdsfree(sdsval);
        return result;
    } else {
        serverPanic("Unknown set encoding");
    }
}


/* ######################################################################
 * [4] setTypeConvert / setTypeConvertAndExpand — t_set.c L511~L580
 *   변환은 점진적이지 않습니다. 원소 전부를 한 번에 새 인코딩으로 옮깁니다.
 *   그래도 변환 시점의 원소 수가 기준값(128, 512) 근처라 한 번의 비용은 작습니다.
 *   dict로 바꿀 때는 먼저 최종 크기로 키워 두므로(빈 dict라 즉시 교체) 옮기는 동안 rehash가 생기지 않습니다.
 * ###################################################################### */

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    setTypeIterator si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(&setDictType);
        sds element;

        /* Presize the dict to avoid rehashing */
        if (panic) {
            dictExpand(d, cap);  /* ▶ 빈 dict를 cap 크기로. dict.c _dictResize의 "옛 테이블이 비면 바로 교체" 경로 */
        } else if (dictTryExpand(d, cap) != DICT_OK) {
            dictRelease(d);
            return C_ERR;
        }

        /* To add the elements we extract integers and create redis objects */
        size_t *alloc_size = htGetMetadataSize(d);
        setTypeInitIterator(&si, setobj);
        while ((element = setTypeNextObject(&si)) != NULL) {  /* ▶ 원소 전부 순회 */
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);  /* ▶ 하나씩 dict에 넣음. 테이블이 이미 충분히 커서 확장이 일어나지 않음 */
            *alloc_size += sdsAllocSize(element);
        }
        setTypeResetIterator(&si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */  /* ▶ 옛 intset/listpack 해제 */
        setobj->encoding = OBJ_ENCODING_HT;
        setobj->ptr = d;
    } else if (enc == OBJ_ENCODING_LISTPACK) {
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(setobj->ptr), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(setobj->ptr), cap);
            estcap = max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        setTypeInitIterator(&si, setobj);
        while (setTypeNext(&si, &str, &len, &llele) != -1) {
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *)str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeResetIterator(&si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        setobj->ptr = lp;
    } else {
        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}


/* ######################################################################
 * [5] setTypeRemoveAux — t_set.c L244~L289
 *   어떤 인코딩이든 지우기만 합니다. HT에서 원소가 몇 개 안 남아도 listpack으로 돌아가지 않습니다.
 *   (HT 안에서 dict 자체의 축소는 dict.c dictGenericDelete → _dictShrinkIfNeeded가 합니다.)
 * ###################################################################### */

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);  /* ▶ intset: 이진 탐색 후 뒤쪽을 앞으로 memmove */
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HT) {
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        int deleted = (dictDelete(setobj->ptr, sdsval) == DICT_OK);  /* ▶ dict.c [7-1]. rehash 한 걸음 + 축소 검사 */
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p != NULL) {
            lp = lpDelete(lp, p, NULL);  /* ▶ listpack: 찾아서 지우기 (뒤쪽을 당기는 복사) */
            setobj->ptr = lp;
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}


/* ######################################################################
 * [6] 작은 인코딩으로 돌아가는 예외 두 곳
 * ###################################################################### */

/* ======================================================================
 * ▶ [6-1] maybeConvertIntset / maybeConvertToIntset — t_set.c L69~L110
 *   maybeConvertIntset    : intset이 512개를 넘으면 HT로 (위쪽 변환)
 *   maybeConvertToIntset  : 전부 정수이고 512개 이하면 intset으로 (아래쪽 변환)
 *                           SINTERSTORE 결과가 전부 정수일 때만 씀 (원본 L1547, 호출은 이 한 곳뿐).
 *                           SUNIONSTORE/SDIFFSTORE는 결과를 intset으로 시작해 보통 규칙으로 키움
 *                           (L1747-1748). SDIFFSTORE는 결과가 작아도 HT로 남을 수 있음 (L1874-1878)
 * ====================================================================== */
/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1<<30) max_entries = 1<<30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())
        setTypeConvert(subject,OBJ_ENCODING_HT);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {
    if (set->encoding == OBJ_ENCODING_INTSET) return; /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */
    intset *is = intsetNew();
    char *str;
    size_t len = 0;
    int64_t llval = 0;
    setTypeIterator si;
    setTypeInitIterator(&si, set);
    while (setTypeNext(&si, &str, &len, &llval) != -1) {
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HT. */
            serverAssert(string2ll(str, len, (long long *)&llval));
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);
        serverAssert(success);
    }
    setTypeResetIterator(&si);
    freeSetObject(set); /* frees the internals but not robj itself */
    set->ptr = is;
    set->encoding = OBJ_ENCODING_INTSET;
}

/* ======================================================================
 * ▶ [6-2] SPOP key count의 CASE 3 — t_set.c L1003~L1012 (CASE 3 블록의 일부, 원본 L971부터 시작)
 *   꺼낼 개수가 Set 크기에 가까우면, "남길 원소"로 새 Set을 만들고 원래 Set은 통째로 응답합니다.
 *   새 Set은 [1]과 다른 규칙으로 시작합니다(첫 원소가 문자열이면 listpack, 정수면 intset. 인자 수는 안 봄).
 *   남길 원소가 기준 이하일 때만 큰 HT였던 Set이 작은 인코딩으로 돌아오고, 많으면 넣다가 다시 HT가 됩니다.
 * ====================================================================== */
        } else {
            while(remaining--) {
                int encoding = setTypeRandomElement(set, &str, &len, &llele);
                if (!newset) {
                    newset = str ? createSetListpackObject() : createIntsetObject();  /* ▶ ★ 새 Set은 작은 인코딩부터. 남는 원소가 많으면 setTypeAddAux가 다시 올려 줌 */
                }
                setTypeAddAux(newset, str, len, llele, encoding == OBJ_ENCODING_HT);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }
        }


/* ######################################################################
 * [부록 A] intset — intset.h L35~L39, intset.c L119~L265
 *   정렬된 정수 배열. 모든 원소가 같은 폭(int16/int32/int64)을 씁니다.
 *   조회는 이진 탐색 O(log N), 삽입/삭제는 자리를 만들려고 뒤쪽을 memmove → O(N).
 *   더 큰 정수가 들어오면 배열 전체를 넓은 폭으로 다시 씁니다(업그레이드).
 *   N ≤ 512라서 O(N) memmove도 몇 KB 복사에 그칩니다.
 * ###################################################################### */

typedef struct intset {
    uint32_t encoding;  /* ▶ 원소 하나의 바이트 수: 2, 4, 8 */
    uint32_t length;
    int8_t contents[];  /* ▶ 정렬된 원소들이 이어서 저장됨 */
} intset;

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0) {
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > _intsetGet(is,max)) {
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        } else if (value < _intsetGet(is,0)) {
            if (pos) *pos = 0;
            return 0;
        }
    }

    while(max >= min) {  /* ▶ 이진 탐색 */
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        cur = _intsetGet(is,mid);
        if (value > cur) {
            min = mid+1;
        } else if (value < cur) {
            max = mid-1;
        } else {
            break;
        }
    }

    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    uint8_t curenc = intrev32ifbe(is->encoding);
    uint8_t newenc = _intsetValueEncoding(value);
    int length = intrev32ifbe(is->length);
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    is->encoding = intrev32ifbe(newenc);
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    while(length--)  /* ▶ 모든 원소를 새 폭으로 다시 씀 O(N). 위로만 넓어지고 다시 좁아지지 않음 */
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    /* Set the value at the beginning or the end. */
    if (prepend)
        _intsetSet(is,0,value);
    else
        _intsetSet(is,intrev32ifbe(is->length),value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint32_t bytes = intrev32ifbe(is->length)-from;
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        src = (int64_t*)is->contents+from;
        dst = (int64_t*)is->contents+to;
        bytes *= sizeof(int64_t);
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst,src,bytes);  /* ▶ 뒤쪽을 한 칸 밀거나 당김 O(N) */
}

/* Insert an integer in the intset */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    if (valenc > intrev32ifbe(is->encoding)) {
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is,value);  /* ▶ 더 넓은 폭이 필요하면 업그레이드 */
    } else {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is,value,&pos)) {  /* ▶ 이미 있으면 끝 */
            if (success) *success = 0;
            return is;
        }

        is = intsetResize(is,intrev32ifbe(is->length)+1);  /* ▶ 한 칸 늘리고 (realloc) */
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);  /* ▶ 자리 만들기 */
    }

    _intsetSet(is,pos,value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

/* Determine whether a value belongs to this set */
uint8_t intsetFind(intset *is, int64_t value) {
    uint8_t valenc = _intsetValueEncoding(value);
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
}


/* ######################################################################
 * [부록 B] setDictType, 객체 생성 함수, 기준값 설정
 * ###################################################################### */

/* ======================================================================
 * ▶ [B-1] setDictType — server.c L633~L645
 *   메인 키 공간의 dbDictType과 비교해 볼 것: "allow to expand"(resizeAllowed)가 NULL입니다.
 *   즉 큰 Set의 dict는 확장할 때 maxmemory 검사를 하지 않습니다.
 *   또 이 dict는 kvstore에 속하지 않아서 databasesCron의 1ms rehash 대상도 아닙니다.
 * ====================================================================== */
/* Set dictionary type. Keys are SDS strings, values are not used. */
dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    setSdsDestructor,          /* key destructor */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */  /* ▶ ★ NULL: maxmemory 검사 없이 확장 */
    .no_value = 1,             /* no values in this dict */  /* ▶ 멤버 sds 포인터만 저장 */
    .keys_are_odd = 1,         /* an SDS string is always an odd pointer */
    .dictMetadataBytes = setDictMetadataBytes,
};

/* ======================================================================
 * ▶ [B-2] 객체 생성 함수 — object.c L473~L492
 * ====================================================================== */
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType);
    robj *o = createObject(OBJ_SET,d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(OBJ_SET,is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

robj *createSetListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_SET, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

/* ======================================================================
 * ▶ [B-3] 기준값 — config.c L3647~L3649 (각 줄의 512, 128, 64가 기본값)
 *   CONFIG SET으로 바꿀 수 있습니다. 크게 올리면 메모리는 아끼지만 원소마다 O(N) 탐색이 길어집니다.
 * ====================================================================== */
    createSizeTConfig("set-max-intset-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_intset_entries, 512, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("set-max-listpack-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_listpack_entries, 128, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("set-max-listpack-value", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_listpack_value, 64, INTEGER_CONFIG, NULL, NULL),
