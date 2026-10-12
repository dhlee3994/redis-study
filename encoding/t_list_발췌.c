/* ======================================================================
 * [해설판] t_list.c 발췌 — List의 listpack ↔ quicklist 변환
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/t_list.c (1756줄)
 *   함께: quicklist.c (노드 크기 기준), config.c (기준값)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본에서 인코딩 변환과 관련된 경로만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 파일과 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *
 * ▶ 왜 List를 보나: Hash와 비교하면 "변환 설계"의 선택지가 보입니다
 *
 *                      Hash (hashset/t_hash_발췌.c)          List (이 파일)
 *   작은 표현          listpack                              listpack
 *   큰 표현            dict                                  quicklist = listpack 노드들의 연결 리스트
 *   커질 때 기준       필드 512개 또는 64바이트 초과          listpack 하나가 8KB 초과 (기본값 -2)
 *   커질 때 비용       O(N): 필드를 하나씩 dict로 옮김         O(1): 기존 listpack을 첫 노드로 그대로 씀  ★
 *   작아질 때          명령으로는 안 돌아감(RDB 로딩 때만)    노드 1개이고 기준의 절반 이하면 listpack으로   ★
 *   돌아가는 비용      -                                      O(1): 노드의 listpack을 꺼내 씀
 *
 *   List는 큰 표현이 "작은 표현의 묶음"이라 변환이 포인터 교체로 끝납니다. 그래서 양방향 변환을 해도
 *   싸고, 대신 경계에서 넣고 빼기를 반복할 때 계속 바뀌지 않도록 "절반" 기준(히스테리시스)을 둡니다.
 *   Hash는 큰 표현(dict)이 전혀 다른 구조라 변환이 O(N) 복사이고, 명령 실행 중에는 되돌리지 않습니다
 *   (재시작·DEBUG RELOAD·RESTORE 같은 RDB 로딩 때만 크기 기준을 다시 적용해 listpack으로 만듦).
 *
 * ▶ list-max-listpack-size (기본 -2)
 *   음수: 노드 하나의 바이트 상한. -1=4KB, -2=8KB, -3=16KB, -4=32KB, -5=64KB
 *   양수: 노드 하나의 원소 수 상한 (단, 바이트는 8KB 안전 상한을 따로 둠)
 *   작은 List(listpack 인코딩)의 상한도 이 값을 그대로 씁니다.
 *
 * ▶ 목차
 *   [1] 변환 함수               : listpack → quicklist, quicklist → listpack       ★
 *   [2] 노드 크기 기준          (quicklist.c)
 *   [3] 변환을 부르는 곳        : LPUSH(커질 때), POP 계열(작아질 때)
 *   [부록] 기준값               (config.c)
 * ====================================================================== */


/* ######################################################################
 * [1] 변환 함수 — t_list.c
 *   두 방향 모두 데이터를 복사하지 않습니다. listpack 포인터를 quicklist 노드에 넣거나 꺼낼 뿐입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] listTypeTryConvertListpack — 커질 때 — t_list.c L17~L56
 * ====================================================================== */
/* Check the length and size of a number of objects that will be added to list to see
 * if we need to convert a listpack to a quicklist. Note that we only check string
 * encoded objects as their string length can be queried in constant time.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertListpack(robj *o, robj **argv, int start, int end,
                                       beforeConvertCB fn, void *data)
{
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    size_t add_bytes = 0;
    size_t add_length = 0;

    if (argv) {
        for (int i = start; i <= end; i++) {
            if (!sdsEncodedObject(argv[i]))  /* ▶ 정수 인자(int 인코딩)는 길이를 바로 알 수 없어 건너뜀. 대략적인 계산 */
                continue;
            add_bytes += sdslen(argv[i]->ptr);  /* ▶ 이번에 넣을 바이트 수 합계 */
        }
        add_length = end - start + 1;
    }

    if (quicklistNodeExceedsLimit(server.list_max_listpack_size,  /* ▶ ★ "넣은 뒤" 크기가 노드 상한(기본 8KB)을 넘을지 미리 검사 */
            lpBytes(o->ptr) + add_bytes, lpLength(o->ptr) + add_length))
    {
        /* Invoke callback before conversion. */
        if (fn) fn(data);

        quicklist *ql = quicklistNew(server.list_max_listpack_size, server.list_compress_depth);  /* ▶ 빈 quicklist */

        /* Append listpack to quicklist if it's not empty, otherwise release it. */
        if (lpLength(o->ptr))
            quicklistAppendListpack(ql, o->ptr);  /* ▶ ★ 기존 listpack을 첫 노드로 "그대로" 붙임. 원소 복사 없음 → O(1) */
        else
            lpFree(o->ptr);
        o->ptr = ql;
        o->encoding = OBJ_ENCODING_QUICKLIST;  /* ▶ 이제 OBJECT ENCODING "quicklist" */
    }
}

/* ======================================================================
 * ▶ [1-2] listTypeTryConvertQuicklist — 작아질 때 — t_list.c L58~L96
 * ====================================================================== */
/* Check the length and size of a quicklist to see if we need to convert it to listpack.
 *
 * 'shrinking' is 1 means that the conversion is due to a list shrinking, to avoid
 * frequent conversions of quicklist and listpack due to frequent insertion and
 * deletion, we don't convert quicklist to listpack until its length or size is
 * below half of the limit.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertQuicklist(robj *o, int shrinking, beforeConvertCB fn, void *data) {
    serverAssert(o->encoding == OBJ_ENCODING_QUICKLIST);

    size_t sz_limit;
    unsigned int count_limit;
    quicklist *ql = o->ptr;

    /* A quicklist can be converted to listpack only if it has only one packed node. */
    if (ql->len != 1 || ql->head->container != QUICKLIST_NODE_CONTAINER_PACKED)  /* ▶ 노드가 1개이고, 그 노드가 큰 원소 하나짜리(PLAIN)가 아닐 때만 */
        return;

    /* Check the length or size of the quicklist is below the limit. */
    quicklistNodeLimit(server.list_max_listpack_size, &sz_limit, &count_limit);
    if (shrinking) {  /* ▶ ★ 줄어드는 중이면 기준을 절반으로 (기본 8KB → 4KB) */
        sz_limit /= 2;
        count_limit /= 2;
    }
    if (ql->head->sz > sz_limit || ql->count > count_limit) return;  /* ▶ 아직 크면 그대로 quicklist */

    /* Invoke callback before conversion. */
    if (fn) fn(data);

    /* Extract the listpack from the unique quicklist node,
     * then reset it and release the quicklist. */
    o->ptr = ql->head->entry;  /* ▶ ★ 노드 안의 listpack을 꺼내서 객체 값으로. 복사 없음 → O(1) */
    ql->head->entry = NULL;
    ql->alloc_size -= ql->head->sz;
    quicklistRelease(ql);  /* ▶ quicklist 껍데기와 빈 노드만 해제 */
    o->encoding = OBJ_ENCODING_LISTPACK;  /* ▶ 다시 OBJECT ENCODING "listpack" */
}

/* ======================================================================
 * ▶ [1-3] listTypeTryConversionRaw와 래퍼 — t_list.c L98~L140
 * ====================================================================== */
/* Check if the list needs to be converted to appropriate encoding due to
 * growing, shrinking or other cases.
 *
 * 'lct' can be one of the following values:
 * LIST_CONV_AUTO      - Used after we built a new list, and we want to let the
 *                       function decide on the best encoding for that list.
 * LIST_CONV_GROWING   - Used before or right after adding elements to the list,
 *                       in which case we are likely to only consider converting
 *                       from listpack to quicklist.
 *                       'argv' is only used in this case to calculate the size
 *                       of a number of objects that will be added to list.
 * LIST_CONV_SHRINKING - Used after removing an element from the list, in which case we
 *                       wanna consider converting from quicklist to listpack. When we
 *                       know we're shrinking, we use a lower (more strict) threshold in
 *                       order to avoid repeated conversions on every list change. */
static void listTypeTryConversionRaw(robj *o, list_conv_type lct,
                                     robj **argv, int start, int end,
                                     beforeConvertCB fn, void *data)
{
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        if (lct == LIST_CONV_GROWING) return; /* Growing has nothing to do with quicklist */  /* ▶ 커지는 중에는 quicklist에서 할 일 없음 */
        listTypeTryConvertQuicklist(o, lct == LIST_CONV_SHRINKING, fn, data);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (lct == LIST_CONV_SHRINKING) return; /* Shrinking has nothing to do with listpack */  /* ▶ 줄어드는 중에는 listpack에서 할 일 없음 */
        listTypeTryConvertListpack(o, argv, start, end, fn, data);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion without passing 'argv'. */
void listTypeTryConversion(robj *o, list_conv_type lct, beforeConvertCB fn, void *data) {
    listTypeTryConversionRaw(o, lct, NULL, 0, 0, fn, data);
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion before adding elements to the list. */
void listTypeTryConversionAppend(robj *o, robj **argv, int start, int end,
                                 beforeConvertCB fn, void *data)
{
    listTypeTryConversionRaw(o, LIST_CONV_GROWING, argv, start, end, fn, data);
}


/* ######################################################################
 * [2] 노드 크기 기준 — quicklist.c
 *   작은 List(listpack 인코딩)의 상한과 quicklist 노드 하나의 상한이 같은 함수로 정해집니다.
 *   listpack 하나가 8KB를 넘지 않게 해서, 노드 안에서의 O(B) 삽입/삭제(listpack_발췌.c [5])를 짧게 유지합니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] optimization_level — quicklist.c L46~L49
 * ====================================================================== */
/* Optimization levels for size-based filling.
 * Note that the largest possible limit is 64k, so even if each record takes
 * just one byte, it still won't overflow the 16 bit count field. */
static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};  /* ▶ fill -1 ~ -5에 대응. 기본 -2 → 8192바이트 */

/* ======================================================================
 * ▶ [2-2] SIZE_SAFETY_LIMIT — quicklist.c L64~L69
 * ====================================================================== */
/* Maximum size in bytes of any multi-element listpack.
 * Larger values will live in their own isolated listpacks.
 * This is used only if we're limited by record count. when we're limited by
 * size, the maximum limit is bigger, but still safe.
 * 8k is a recommended / default size limit */
#define SIZE_SAFETY_LIMIT 8192  /* ▶ 양수 fill(원소 수 기준)일 때도 바이트는 8KB를 넘지 않게 */

/* ======================================================================
 * ▶ [2-3] quicklistNodeLimit, quicklistNodeExceedsLimit, isLargeElement — quicklist.c L484~L539
 * ====================================================================== */
#define sizeMeetsSafetyLimit(sz) ((sz) <= SIZE_SAFETY_LIMIT)

/* Calculate the size limit of the quicklist node based on negative 'fill'. */
static size_t quicklistNodeNegFillLimit(int fill) {
    assert(fill < 0);
    size_t offset = (-fill) - 1;
    size_t max_level = sizeof(optimization_level) / sizeof(*optimization_level);
    if (offset >= max_level) offset = max_level - 1;
    return optimization_level[offset];
}

/* Calculate the size limit or length limit of the quicklist node
 * based on 'fill', and is also used to limit list listpack. */
void quicklistNodeLimit(int fill, size_t *size, unsigned int *count) {
    *size = SIZE_MAX;
    *count = UINT_MAX;

    if (fill >= 0) {
        /* Ensure that one node have at least one entry */
        *count = (fill == 0) ? 1 : fill;  /* ▶ 양수: 원소 수 상한 */
    } else {
        *size = quicklistNodeNegFillLimit(fill);  /* ▶ 음수: 바이트 상한 (기본 8KB) */
    }
}

/* Check if the limit of the quicklist node has been reached to determine if
 * insertions, merges or other operations that would increase the size of
 * the node can be performed.
 * Return 1 if exceeds the limit, otherwise 0. */
int quicklistNodeExceedsLimit(int fill, size_t new_sz, unsigned int new_count) {
    size_t sz_limit;
    unsigned int count_limit;
    quicklistNodeLimit(fill, &sz_limit, &count_limit);

    if (likely(sz_limit != SIZE_MAX)) {
        return new_sz > sz_limit;  /* ▶ ★ 기본 설정(음수)에서는 여기: 새 크기가 8KB를 넘으면 1 */
    } else if (count_limit != UINT_MAX) {
        /* when we reach here we know that the limit is a size limit (which is
         * safe, see comments next to optimization_level and SIZE_SAFETY_LIMIT) */
        if (!sizeMeetsSafetyLimit(new_sz)) return 1;
        return new_count > count_limit;
    }

    redis_unreachable();
}

/* Determines whether a given size qualifies as a large element based on a threshold
 * determined by the 'fill'. If the size is considered large, it will be stored in
 * a plain node. */
static int isLargeElement(size_t sz, int fill) {
    if (unlikely(packed_threshold != 0)) return sz >= packed_threshold;
    if (fill >= 0)
        return !sizeMeetsSafetyLimit(sz);
    else
        return sz > quicklistNodeNegFillLimit(fill);  /* ▶ 원소 하나가 노드 상한(8KB)을 넘으면 그 원소만 따로 PLAIN 노드에 담음 */
}

/* ======================================================================
 * ▶ [2-4] quicklistAppendListpack — quicklist.c L665~L678
 *   RDB 로딩용으로 만든 함수지만, [1-1]의 listpack → quicklist 변환에도 그대로 쓰입니다.
 * ====================================================================== */
/* Create new node consisting of a pre-formed listpack.
 * Used for loading RDBs where entire listpacks have been stored
 * to be retrieved later. */
void quicklistAppendListpack(quicklist *quicklist, unsigned char *zl) {
    quicklistNode *node = quicklistCreateNode(quicklist);

    node->entry = zl;  /* ▶ ★ 넘겨받은 listpack 포인터를 노드에 그대로 넣음 */
    node->count = lpLength(node->entry);  /* ▶ listpack 헤더의 원소 수 (보통 O(1)) */
    node->sz = lpBytes(zl);

    quicklist->alloc_size += node->sz;
    _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);
    quicklist->count += node->count;
}


/* ######################################################################
 * [3] 변환을 부르는 곳 — t_list.c
 *   커질 때(LPUSH, RPUSH, LINSERT, LSET, LMOVE ...)는 넣기 "전에" GROWING으로,
 *   작아질 때(LPOP, RPOP, LREM, LTRIM ...)는 뺀 "후에" SHRINKING으로 부릅니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] pushGenericCommand — LPUSH / RPUSH — t_list.c L482~L526
 * ====================================================================== */
/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
void pushGenericCommand(client *c, int where, int xx) {
    unsigned long llen;
    dictEntryLink link;
    int j;
    size_t oldsize = 0;

    kvobj *lobj = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (checkType(c,lobj,OBJ_LIST)) return;
    int existed = (lobj != NULL);
    if (!lobj) {
        if (xx) {
            addReply(c, shared.czero);
            return;
        }

        lobj = createListListpackObject();  /* ▶ ★ 새 List는 빈 listpack */
        dbAddByLink(c->db, c->argv[1], &lobj, &link);
    }

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(lobj);
    listTypeTryConversionAppend(lobj,c->argv,2,c->argc-1,NULL,NULL);  /* ▶ ★ 넣기 전에 검사 → 넘을 것 같으면 지금 quicklist로 */
    for (j = 2; j < c->argc; j++) {
        listTypePush(lobj,c->argv[j],where);  /* ▶ listpack이면 lpAppend/lpPrepend, quicklist면 끝 노드에 추가 */
        server.dirty++;
    }

    llen = listTypeLength(lobj);
    addReplyLongLong(c, llen);

    char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
    keyModified(c,c->db,c->argv[1],lobj,1);
    /* Wake clients blocked on this key. dbAdd() already signals a freshly
     * created key, but a push to a pre-existing list must signal too, so that
     * clients blocked on an existing key (e.g. BLMOVEM EXACTLY waiting for more
     * elements) are re-processed. */
    if (existed)
        signalKeyAsReadyNonEmptyList(c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    updateKeysizesHist(c->db, OBJ_LIST, llen - (c->argc - 2), llen);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), lobj, oldsize, kvobjAllocSize(lobj));
}

/* ======================================================================
 * ▶ [3-2] listElementsRemoved — POP 뒤 정리 — t_list.c L788~L816
 * ====================================================================== */
/* A housekeeping helper for list elements popping tasks.
 *
 * If 'signal' is 0, skip calling keyModified().
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count, size_t oldsize, int signal, int *deleted) {
    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
    unsigned long llen = listTypeLength(o);
    
    notifyKeyspaceEvent(NOTIFY_LIST, event, key, c->db->id);
    updateKeysizesHist(c->db, OBJ_LIST, llen + count, llen);
    if (llen == 0) {
        if (deleted) *deleted = 1;

        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(key->ptr), o, oldsize, kvobjAllocSize(o));
        dbDelete(c->db, key);  /* ▶ 다 비면 키 자체를 지움 */
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    } else {
        listTypeTryConversion(o, LIST_CONV_SHRINKING, NULL, NULL);  /* ▶ ★ 줄어든 뒤 검사 → 노드 1개이고 4KB 이하면 listpack으로 */
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(key->ptr), o, oldsize, kvobjAllocSize(o));
        if (deleted) *deleted = 0;
    }
    if (signal)
        keyModified(c, c->db, key, llen ? o : NULL, 1);
    server.dirty += count;
}


/* ######################################################################
 * [부록] 기준값 — config.c
 * ###################################################################### */

/* ======================================================================
 * ▶ list-max-listpack-size, list-compress-depth — config.c L3558~L3558
 * ====================================================================== */
    createIntConfig("list-max-listpack-size", "list-max-ziplist-size", MODIFIABLE_CONFIG, INT_MIN, INT_MAX, server.list_max_listpack_size, -2, INTEGER_CONFIG, NULL, NULL),  /* ▶ 기본 -2 = 노드(그리고 작은 List) 하나당 8KB. 옛 이름 list-max-ziplist-size도 받음 */

/* ======================================================================
 * ▶ list-compress-depth — config.c L3580~L3580
 * ====================================================================== */
    createIntConfig("list-compress-depth", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, 0, INT_MAX, server.list_compress_depth, 0, INTEGER_CONFIG, NULL, NULL),  /* ▶ 기본 0 = 압축 안 함. 1 이상이면 양 끝 몇 노드를 빼고 LZF 압축 */
