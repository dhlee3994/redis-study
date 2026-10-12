/* ======================================================================
 * [해설판] object.c 발췌 — 객체 생성, String 인코딩 선택, 해제, 관찰 도구
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/object.c (2028줄)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 2028줄 중 인코딩과 관련된 부분만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *   먼저 볼 파일: encoding/object.h (robj, kvobj 구조체)
 *
 * ▶ object.c가 하는 일
 *   1) 타입별 "작은 표현"으로 객체를 만든다 (createHashObject → listpack, createSetObject → dict ...)
 *   2) String만은 여기서 직접 인코딩을 고른다 (tryObjectEncoding: int / embstr / raw)
 *      나머지 타입의 변환(listpack → dict 등)은 t_hash.c, t_set.c, t_list.c, t_zset.c가 각자 한다
 *   3) refcount가 0이 되면 해제한다. 해제 비용이 인코딩에 따라 O(1)과 O(N)으로 갈린다
 *   4) OBJECT ENCODING, MEMORY USAGE로 내부를 보여 준다
 *
 * ▶ String 인코딩 3종 (값 "hello", 키 "k" 기준 그림)
 *   int    : [robj 16][kvBits 1][키 sds]                 ptr 자리에 정수 자체. 값 할당 없음
 *   embstr : [robj 16][kvBits 1][키 sds][값 sds]          한 번 할당, 한 덩어리 (CACHE_LINE_SIZE 이하.
 *                                                         리눅스·x86 64바이트, Apple Silicon macOS 빌드 128바이트)
 *   raw    : [robj 16][kvBits 1][키 sds] ──ptr──▶ [값 sds]  두 번 할당, 값은 따로
 *
 * ▶ 목차
 *   [1] kvobjCreate, createObject     : 객체 헤더 만들기
 *   [2] String 인코딩                  : 44바이트 규칙과, 키스페이스에서 바뀐 64바이트 규칙   ★
 *   [3] tryObjectEncoding             : SET 값이 int / embstr / raw 중 무엇이 되는가        ★
 *   [4] 타입별 생성 함수               : 처음엔 거의 다 작은 인코딩으로 시작
 *   [5] decrRefCount와 free 함수       : 해제 비용이 인코딩에 따라 다르다 (Big Key)          ★
 *   [6] 관찰 도구                      : OBJECT ENCODING, MEMORY USAGE
 * ====================================================================== */


/* ######################################################################
 * [1] 객체 헤더 만들기
 *   robj는 16바이트 헤더입니다. 키스페이스에 들어가는 값은 kvobj로 만들어 키를 같은 할당에 붙입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] kvobjCreate — object.c L44~L108
 *   키를 robj 뒤에 붙여 한 번에 할당합니다. 값(ptr)은 따로 있는 것을 가리킵니다.
 *   encoding을 RAW로 두는 것은 기본값입니다. 대부분 호출한 쪽이 곧바로 실제 인코딩으로 덮어쓰고,
 *   값이 따로 할당된 raw sds이면(kvobjSet의 embstr → raw 경로 등) RAW 그대로 둡니다.
 * ====================================================================== */
/* For objects with large embedded keys, we reserve space for an expire field,
 * so if expire is set later, we don't need to reallocate the object. */
#define KEY_SIZE_TO_INCLUDE_EXPIRE_THRESHOLD 128  /* ▶ 키가 128바이트 이상이면 만료 시간 자리를 미리 잡아 둠 (나중에 EXPIRE 때 재할당을 피함) */

/* ===================== Creation and parsing of objects ==================== */

/* Creates kvobj (with embedded key and optional metadata) 
 * 
 * keyMetaBits - bitmask of active metadata classes to allocate space for (bit 0 is
 *               reserved for expiration).
 * 
 * Example of "mykey" with expiration and metadata :
 * 
 *    +------------+------------+-----------+------------+------------------------+
 *    | m.meta (8) | expiry (8) | robj (16) | kvbits (1) | sdshdr5 "mykey" \0 (7) | 
 *    +------------+------------+-----------+------------+------------------------+
 *                              ^
 *                              |
 *                              kvobjCreate() returns pointer to here
 */
kvobj *kvobjCreate(int type, const sds key, void *ptr, uint32_t keyMetaBits) {
    /* Determine embedded key and expiration flags */
    serverAssert(key != NULL);

    /* If key is large and expire is not set, add space for it. */
    size_t key_sds_len = sdslen(key);
    if (key_sds_len >= KEY_SIZE_TO_INCLUDE_EXPIRE_THRESHOLD)
        keyMetaBits |= KEY_META_MASK_EXPIRE;

    /* Now that keyMetaBits is finalized, compute metadata size. */
    uint32_t sizeMetas = getNumMeta(keyMetaBits) * sizeof(uint64_t);  /* ▶ 메타데이터 하나당 8바이트. robj "앞쪽"에 붙음 */

    /* Calculate embedded key size */
    char key_sds_type = sdsReqType(key_sds_len);
    size_t key_sds_size = sdsReqSize(key_sds_len, key_sds_type);
    debugServerAssert(key_sds_type <= SDS_TYPE_32); /* keys are never > 4GB */

    /* Compute the base object size */
    size_t min_size = sizeof(robj) + sizeof(kvBits);  /* ▶ 16 + 1 */
    min_size += sizeMetas;
    min_size += key_sds_size;

    /* Allocate object memory */
    char *alloc = zmalloc(min_size);  /* ▶ ★ 할당 한 번: [메타][robj][kvBits][키 sds] */
    kvobj *kv = (kvobj *) (alloc + sizeMetas);  /* ▶ 반환 포인터는 메타데이터를 건너뛴 robj 시작점 */
    kv->type = type;
    kv->encoding = OBJ_ENCODING_RAW;  /* ▶ 호출한 쪽이 실제 인코딩으로 바꿈 (예: kvobjSet L326) */
    kv->ptr = ptr;
    kv->refcount = 1;
    kv->lru = 0;
    kv->iskvobj = 1;
    kv->metabits = keyMetaBits;
    kvobjBits(kv)->key_sds_type = key_sds_type;
    kvobjBits(kv)->no_evict = 0;
    kvobjBits(kv)->unused = 0;

    /* Store embedded key, right after the kvBits. */
    char *data = (char *) (kvobjBits(kv) + 1);
    sdsnewplacement(data, key_sds_size, key_sds_type, key, key_sds_len);  /* ▶ 키 문자열을 kvBits 바로 뒤에 복사 */

    /* Reset each allocated metadata to its reset_value (such as Expiry=-1, etc) */
    keyMetaResetValues(kv);

    return kv;
}

/* ======================================================================
 * ▶ [1-2] createObject — object.c L110~L120
 *   키 없는 일반 robj. 명령 인자(argv), 임시 값, 컬렉션 생성에 씁니다.
 *   키스페이스에 넣을 때 dbAdd → kvobjSet이 이것을 kvobj로 바꿉니다.
 * ====================================================================== */
robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));  /* ▶ 16바이트 헤더만 할당. 실제 데이터는 ptr이 가리키는 곳에 따로 */
    o->type = type;
    o->encoding = OBJ_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    o->lru = 0;
    o->iskvobj = 0;
    o->metabits = 0;
    return o;
}


/* ######################################################################
 * [2] String 인코딩: raw, embstr, 그리고 키스페이스의 64바이트 규칙
 *   같은 문자열이라도 길이와 내용에 따라 세 가지로 저장됩니다.
 *     raw    : robj와 sds를 따로 할당. 길이 제한 없음. APPEND, SETRANGE처럼 고쳐 쓸 수 있음
 *     embstr : robj와 sds를 한 덩어리로 할당. 할당 한 번, 캐시 미스 한 번. 고쳐 쓸 수 없음(읽기 전용)
 *     int    : 정수면 ptr 자리에 숫자를 그대로 넣음. 문자열 할당 자체가 없음
 *   주의: "44바이트까지 embstr"은 키 없는 robj의 규칙입니다. 키스페이스에 저장할 때는
 *   kvobjSet이 "키 + 값이 64바이트 캐시 라인에 들어가는가"로 다시 판단합니다 ([2-4]).
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] createRawStringObject — object.c L152~L156
 * ====================================================================== */
/* Create a string object with encoding OBJ_ENCODING_RAW, that is a plain
 * string object where o->ptr points to a proper sds string. */
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr,len));  /* ▶ 할당 두 번: robj 16바이트 + sds */
}

/* ======================================================================
 * ▶ [2-2] createStringObject — 44바이트 규칙 — object.c L340~L352
 *   44 = 64(jemalloc 크기 등급) - 16(robj) - 3(sdshdr8 헤더) - 1(널 문자).
 *   네트워크에서 받은 명령 인자 대부분이 이 함수로 만들어집니다.
 * ====================================================================== */
/* Create a string object with EMBSTR encoding if it is smaller than
 * OBJ_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 44 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44  /* ▶ 키 없는 robj 기준의 기준값 */
robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)  /* ▶ 44바이트 이하 → embstr */
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

/* ======================================================================
 * ▶ [2-3] createEmbeddedStringObject — object.c L217~L248
 * ====================================================================== */
/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself.
 * 
 * Example of robj with embedded "myvalue" (16+11 = 27 bytes):
 *    +-----------+----------------------------+
 *    | robj (16) | sdshdr8 "myvalue" \0  (11) | 
 *    +-----------+----------------------------+
 */
static inline robj *createEmbeddedStringObject(const char *val_ptr, size_t val_len) {
    /* Calculate size for embedded value (always SDS_TYPE_8) */
    size_t val_sds_size = sdsReqSize(val_len, SDS_TYPE_8);
    
    /* Allocate object memory */
    size_t bufsize = 0;
    robj *o = zmalloc_usable(sizeof(robj) + val_sds_size, &bufsize);  /* ▶ ★ robj와 sds를 한 번에 할당 */
    o->type = OBJ_STRING;
    o->encoding = OBJ_ENCODING_EMBSTR;
    o->refcount = 1;
    o->lru = 0;
    o->metabits = 0;
    o->iskvobj = 0;

    /* The memory after the struct where we embedded data. */
    char *data = (char *)(o + 1);  /* ▶ sds는 robj 바로 뒤 (o + 1) */
    
    /* Copy embedded value (EMBSTR) always as SDS TYPE 8. Account for unused
     * memory in the SDS alloc field. */
    size_t remaining_size = bufsize - (data - (char *)(void *)o);  /* ▶ jemalloc이 실제로 준 크기(bufsize)까지 sds 용량으로 씀 */
    o->ptr = sdsnewplacement(data, remaining_size, SDS_TYPE_8, val_ptr, val_len);
    return o;
}

/* ======================================================================
 * ▶ [2-4] kvobjSet — 키스페이스에 넣을 때 인코딩을 다시 정한다 — object.c L287~L338
 *   ★ 8.2부터 생긴 함수입니다(8.0에는 없음). 값 robj를 키를 품은 kvobj로 옮깁니다.
 *   embstr 값이면 "robj 16 + kvBits 1 + 키(길이+2) + 값(길이+4)"이 64바이트 이하일 때만 embstr로 남습니다.
 *   정리하면 키 길이 + 값 길이 <= 41 바이트일 때 embstr입니다.
 *     키 "k"(1바이트)       → 값 40바이트까지 embstr, 41~44바이트는 raw
 *     키 "user:1000"(9)    → 값 32바이트까지 embstr
 *     키 42바이트 이상      → 값이 비어 있어도 raw
 *   (위 64는 CACHE_LINE_SIZE입니다. Apple Silicon macOS에서 직접 빌드하면 128이라 "키+값 <= 105"이고,
 *    값 44바이트 상한이 먼저 걸리므로 키 "k"에 41~44바이트 값도 embstr로 남습니다. Docker(리눅스)는 64.
 *    또 TTL 메타데이터 8바이트는 이 계산에서 빠지므로 실제 할당은 64바이트를 넘을 수 있습니다.)
 *   그래서 unstable에서는 "44바이트 이하면 embstr"이 키스페이스 값에는 맞지 않습니다.
 *   OBJECT ENCODING으로 직접 확인할 수 있습니다.
 * ====================================================================== */
/* This functions may reallocate the value. The new allocation is returned and
 * the old object's reference counter is decremented and possibly freed. Use the
 * returned object instead of 'val' after calling this function. */
kvobj *kvobjSet(sds key, robj *val, uint32_t keyMetaBits) {
    kvobj *kv;
    if (val->type == OBJ_STRING && val->encoding == OBJ_ENCODING_EMBSTR) {  /* ▶ ★ embstr 인자일 때만 64바이트 검사 */
        size_t len = sdslen(val->ptr);

        /* Embed when the sum is less than a cache line (Metadata is discarded 
         * since we don't have to be accurate and it is placed before the object) */
        size_t size = sizeof(kvobj) + sizeof(kvBits);  /* ▶ 16 + 1 */
        size += (key != NULL) * (sdslen(key) + 2); /* sds hdr (1) + nullterm (1) */  /* ▶ 키 sds 헤더(1) + 널(1). 키가 32바이트 이상이면 실제 헤더는 3바이트지만 대략 계산 */
        size += 4 + len; /* embstr header (3) + nullterm (1) */  /* ▶ 값 sdshdr8 헤더(3) + 널(1) */
        if (size <= CACHE_LINE_SIZE) {  /* ▶ CACHE_LINE_SIZE = 64 (Apple M 계열만 128, config.h) */
            kv = kvobjCreateEmbedString(val->ptr, len, key, keyMetaBits);  /* ▶ → OBJECT ENCODING "embstr" */
        } else {
            kv = kvobjCreate(OBJ_STRING, key, sdsnewlen(val->ptr, len), keyMetaBits);  /* ▶ → 값 sds를 새로 복사해 따로 둠. kvobjCreate가 encoding을 RAW로 둠 → "raw" */
        }
    } else {
        /* Create a new object with embedded key. Reuse ptr if possible. */
        void *valptr;
        if (val->refcount == 1) {  /* ▶ int, raw, 그리고 모든 컬렉션 타입은 여기로 */
            /* Reuse the ptr. There are no other references to val. */
            valptr = val->ptr;
            val->ptr = NULL;  /* ▶ 값 데이터(dict, listpack ...)는 복사하지 않고 포인터만 넘김 → 크기와 무관하게 O(1) */
        } else if (val->type == OBJ_STRING &&  /* ▶ 공유 정수 객체(refcount = SHARED)가 여기로 옴 */
                   val->encoding == OBJ_ENCODING_INT) {
            /* The pointer is not allocated memory. We can just copy the pointer. */
            valptr = val->ptr;  /* ▶ 정수 값만 복사. 결과 kvobj는 refcount 1인 별도 객체 (공유되지 않음) */
        } else if (val->type == OBJ_STRING &&
                   val->encoding == OBJ_ENCODING_RAW) {
            /* Dup the string. */
            valptr = sdsdup(val->ptr);
        } else {
            /* There are multiple references to this non-string object. Most types
             * can be duplicated, but for a module type is not always possible. */
            serverPanic("Not implemented");
        }
        kv = kvobjCreate(val->type, key, valptr, keyMetaBits);
        kv->encoding = val->encoding;  /* ▶ 원래 인코딩을 그대로 가져옴 */
    }
    
    kv->lru = val->lru;
    if (val->iskvobj) kvobjBits(kv)->no_evict = kvobjBits(val)->no_evict;

    /* Transfer module metadata from `val` to new `kv` (if `val` of type kvobj with metadata). */
    if (val->metabits & KEY_META_MASK_MODULES)
        keyMetaTransition((kvobj *) val, kv);
    
    decrRefCount(val);  /* ▶ 옛 robj 껍데기 해제 (ptr은 위에서 NULL로 넘겨받았으므로 데이터는 살아 있음) */
    return kv;
}

/* ======================================================================
 * ▶ [2-5] kvobjCreateEmbedString — object.c L158~L215
 *   [2-3]과 같은 일을 키까지 포함해서 합니다. [robj][kvBits][키 sds][값 sds] 한 덩어리.
 * ====================================================================== */
/* Creates a new embedded string object and copies the content of key, val and
 * expire to the new object. LRU is set to 0. 
 * 
 * Example of kvobj "mykey" with embedded "myvalue" (16+1+7+11 = 35bytes):
 *    +-----------+------------+------------------------+----------------------------+
 *    | robj (16) | kvbits (1) | sdshdr5 "mykey" \0 (7) | sdshdr8 "myvalue" \0  (11) | 
 *    +-----------+------------+------------------------+----------------------------+
 */
static kvobj *kvobjCreateEmbedString(const char *val_ptr, size_t val_len,
                                     const sds key, uint32_t keyMetaBits)
{
    kvobj *o;
    debugServerAssert(key != NULL);
    uint32_t sizeMetas = getNumMeta(keyMetaBits) * sizeof(uint64_t);

    /* Calculate sizes for embedded key */
    size_t key_sds_len = sdslen(key);
    char key_sds_type = sdsReqType(key_sds_len);
    size_t key_sds_size = sdsReqSize(key_sds_len, key_sds_type);
    debugServerAssert(key_sds_type <= SDS_TYPE_32); /* keys are never > 4GB */

    /* Calculate size for embedded value (always SDS_TYPE_8) */
    size_t val_sds_size = sdsReqSize(val_len, SDS_TYPE_8);  /* ▶ 값은 항상 sdshdr8 (헤더 3바이트) */

    /* Compute base object size */
    size_t min_size = sizeof(robj) + sizeof(kvBits) + val_sds_size;
    min_size += sizeMetas;
    min_size += key_sds_size;

    /* Allocate object memory */
    size_t bufsize = 0;
    char *alloc = zmalloc_usable(min_size, &bufsize);  /* ▶ ★ 할당 한 번 */
    o = (kvobj *) (alloc + sizeMetas);

    o->type = OBJ_STRING;
    o->encoding = OBJ_ENCODING_EMBSTR;  /* ▶ OBJECT ENCODING "embstr" */
    o->refcount = 1;
    o->lru = 0;
    o->metabits = keyMetaBits;
    o->iskvobj = 1;
    kvobjBits(o)->key_sds_type = key_sds_type;
    kvobjBits(o)->no_evict = 0;
    kvobjBits(o)->unused = 0;

    /* Store embedded key, right after the kvBits. */
    char *data = (char *) (kvobjBits(o) + 1);
    sdsnewplacement(data, key_sds_size, key_sds_type, key, key_sds_len);
    data += key_sds_size;  /* ▶ 키 바로 뒤에 값 */

    /* Copy embedded value (EMBSTR) always as SDS TYPE 8. Account for unused
     * memory in the SDS alloc field. */
    size_t remaining_size = bufsize - (data - alloc);
    o->ptr = sdsnewplacement(data, remaining_size, SDS_TYPE_8, val_ptr, val_len);
    
    keyMetaResetValues(o); /* modules + expire */
    
    return o;
}

/* ======================================================================
 * ▶ [2-6] createStringObjectFromLongLongWithOptions, ...ForValue — 정수 문자열 — object.c L369~L410
 *   0~9999는 서버 시작 때 만들어 둔 공유 객체(shared.integers)를 돌려줍니다.
 *   그 밖의 long 범위 정수는 OBJ_ENCODING_INT로, ptr 자리에 숫자를 직접 넣습니다.
 *   maxmemory가 설정돼 있고 정책이 LRU/LFU/LRM 계열이면 키마다 lru 필드가 필요하므로 공유 객체를 쓰지 않습니다.
 *   참고: 키스페이스에 넣을 때는 [2-4] kvobjSet이 정수만 복사해 새 kvobj를 만들기 때문에,
 *   8.2 이후로는 공유 정수 객체가 키스페이스 값으로 "공유"되지는 않습니다(OBJECT REFCOUNT가 1).
 *   공유 객체는 여전히 응답 생성 같은 곳에서 할당을 줄여 줍니다.
 * ====================================================================== */
/* Create a string object from a long long value according to the specified flag. */
#define LL2STROBJ_AUTO 0       /* automatically create the optimal string object */
#define LL2STROBJ_NO_SHARED 1  /* disallow shared objects */
#define LL2STROBJ_NO_INT_ENC 2 /* disallow integer encoded objects. */
robj *createStringObjectFromLongLongWithOptions(long long value, int flag) {
    robj *o;

    if (value >= 0 && value < OBJ_SHARED_INTEGERS && flag == LL2STROBJ_AUTO) {  /* ▶ OBJ_SHARED_INTEGERS = 10000 (server.h) */
        o = shared.integers[value];  /* ▶ 할당 없음 */
    } else {
        if ((value >= LONG_MIN && value <= LONG_MAX) && flag != LL2STROBJ_NO_INT_ENC) {
            o = createObject(OBJ_STRING, NULL);
            o->encoding = OBJ_ENCODING_INT;  /* ▶ ★ int 인코딩: 할당은 robj 헤더뿐, 문자열 sds 없음 */
            o->ptr = (void*)((long)value);
        } else {
            char buf[LONG_STR_SIZE];
            int len = ll2string(buf, sizeof(buf), value);
            o = createStringObject(buf, len);  /* ▶ long 범위 밖(32비트 빌드 등)이거나 int 인코딩을 금지한 경우(…WithSds) → 문자열로 */
        }
    }
    return o;
}

/* Wrapper for createStringObjectFromLongLongWithOptions() always demanding
 * to create a shared object if possible. */
robj *createStringObjectFromLongLong(long long value) {
    return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_AUTO);
}

/* The function avoids returning a shared integer when LFU/LRU info
 * are needed, that is, when the object is used as a value in the key
 * space(for instance when the INCR command is used), and Redis is
 * configured to evict based on LFU/LRU, so we want LFU/LRU values
 * specific for each key. */
robj *createStringObjectFromLongLongForValue(long long value) {
    if (server.maxmemory == 0 || !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) {  /* ▶ maxmemory가 설정돼 있고 정책이 LRU/LFU/LRM이면 공유 금지 */
        /* If the maxmemory policy permits, we can still return shared integers */
        return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_AUTO);
    } else {
        return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_NO_SHARED);
    }
}


/* ######################################################################
 * [3] tryObjectEncoding — SET 값의 인코딩이 정해지는 곳
 *   SET, SETNX, SETEX, APPEND(새 키) 등이 값을 저장하기 전에 부릅니다 (t_string_발췌.c [2]).
 *   순서: 정수로 바꿀 수 있나 → 44바이트 이하인가 → 아니면 raw 그대로.
 *   (32KB 이상 큰 인자 등 특정 경우에만 sds 끝의 남는 공간을 줄임)
 *   다른 타입(List, Hash ...)은 이 함수를 쓰지 않고 각 t_*.c가 인코딩을 관리합니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] tryObjectEncodingEx — object.c L947~L1010
 * ====================================================================== */
/* Try to encode a string object in order to save space */
robj *tryObjectEncodingEx(robj *o, int try_trim) {
    long value;
    sds s = o->ptr;
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type. */
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars. */
    if (!sdsEncodedObject(o)) return o;  /* ▶ int로 이미 바뀐 객체 등은 그대로 */

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis and may end in places where
     * they are not handled. We handle them only as values in the keyspace. */
     if (o->refcount > 1) return o;  /* ▶ 다른 곳에서도 참조 중인 객체는 건드리지 않음 */

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 20 chars is not
     * representable as a 32 nor 64 bit integer. */
    len = sdslen(s);
    if (len <= 20 && string2l(s,len,&value)) {  /* ▶ ★ 20자 이하이고 앞뒤 공백, 앞자리 0 없이 long으로 정확히 바뀌는 문자열만. "007"은 문자열로 남음 */
        /* This object is encodable as a long. */
        if (o->encoding == OBJ_ENCODING_RAW) {
            sdsfree(o->ptr);  /* ▶ raw → int: sds 해제 */
            o->encoding = OBJ_ENCODING_INT;
            o->ptr = (void*) value;
            return o;
        } else if (o->encoding == OBJ_ENCODING_EMBSTR) {
            decrRefCount(o);
            return createStringObjectFromLongLongForValue(value);  /* ▶ embstr → int: 객체를 새로 받음 (0~9999면 공유 객체) */
        }
    }

    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses. */
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {  /* ▶ ★ 44바이트 이하 raw → embstr로 다시 만듦 (할당 2번 → 1번) */
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

    /* We can't encode the object...
     * Do the last try, and at least optimize the SDS string inside */
    if (try_trim)
        trimStringObjectIfNeeded(o, 0);  /* ▶ 32KB(PROTO_MBULK_BIG_ARG) 이상 큰 인자일 때만(그 밖엔 Lua 스크립트 인자 캐시),
         *   남는 공간이 10% 넘으면 줄임. 45바이트~32KB 값은 그대로 둠 (L929 trimStringObjectIfNeeded) */

    /* Return the original object. */
    return o;
}

robj *tryObjectEncoding(robj *o) {
    return tryObjectEncodingEx(o, 1);
}


/* ######################################################################
 * [4] 타입별 생성 함수 — 처음에는 거의 다 "작은 인코딩"
 *   List, Hash는 listpack으로 시작합니다. ZSet은 ZADD 인자 수와 첫 멤버 길이로 listpack/skiplist를
 *   고릅니다(t_zset.c zsetTypeCreate). Set은 t_set.c setTypeCreate가 첫 원소와 인자 수를 보고
 *   intset / listpack / dict 중에서 고릅니다 (hashset/t_set_발췌.c [1]).
 *   작은 인코딩에서 큰 인코딩으로 바꾸는 일은 각 타입 파일이 합니다.
 *     List : t_list_발췌.c       Hash : hashset/t_hash_발췌.c
 *     Set  : hashset/t_set_발췌.c   ZSet : 4주차 t_zset.c
 * ###################################################################### */

/* ======================================================================
 * ▶ [4-1] create*Object — object.c L459~L517
 * ====================================================================== */
robj *createQuicklistObject(int fill, int compress) {
    quicklist *l = quicklistNew(fill, compress);
    robj *o = createObject(OBJ_LIST,l);
    o->encoding = OBJ_ENCODING_QUICKLIST;  /* ▶ 큰 List */
    return o;
}

robj *createListListpackObject(void) {
    unsigned char *lp = lpNew(0);  /* ▶ ★ 새 List는 listpack (t_list.c pushGenericCommand) */
    robj *o = createObject(OBJ_LIST,lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType);  /* ▶ 큰 Set (처음부터 큰 SADD일 때 등) */
    robj *o = createObject(OBJ_SET,d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();  /* ▶ 정수만 있는 Set */
    robj *o = createObject(OBJ_SET,is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

robj *createSetListpackObject(void) {
    unsigned char *lp = lpNew(0);  /* ▶ 작은 Set */
    robj *o = createObject(OBJ_SET, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createHashObject(void) {
    unsigned char *zl = lpNew(0);  /* ▶ ★ 새 Hash는 언제나 빈 listpack */
    robj *o = createObject(OBJ_HASH, zl);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType);  /* ▶ 큰 ZSet: dict(멤버 → 점수 조회 O(1)) + skiplist(순위/범위 O(log N)) 두 개를 같이 유지 */
    zs->zsl = zslCreate();  /* ▶ 4주차 주제 */
    o = createObject(OBJ_ZSET,zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}

robj *createZsetListpackObject(void) {
    unsigned char *lp = lpNew(0);  /* ▶ ★ 작은 ZSet: [멤버][점수][멤버][점수]... 연속 메모리 */
    robj *o = createObject(OBJ_ZSET,lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}


/* ######################################################################
 * [5] 해제 비용 — 인코딩이 Big Key의 지연을 바꾸는 지점
 *   refcount가 0이 되면 decrRefCount가 해제합니다. 이때 걸리는 시간이 인코딩에 따라 다릅니다.
 *     listpack, intset, embstr, int : zfree 한 번. 원소 수와 상관없이 O(1)
 *     HT(dict), skiplist, quicklist : 원소(또는 노드)마다 free. O(N)
 *   원소 100만 개짜리 Hash를 DEL하면 메인 스레드에서 free 100만 번이 돕니다.
 *   이것을 백그라운드로 넘기는 것이 lazyfree입니다 (lazyfree_발췌.c).
 * ###################################################################### */

/* ======================================================================
 * ▶ [5-1] decrRefCount — object.c L652~L698
 * ====================================================================== */
void decrRefCount(robj *o) {
    if (o->refcount == OBJ_SHARED_REFCOUNT)
        return; /* Nothing to do: this refcount is immutable. */  /* ▶ 공유 객체는 해제하지 않음 */

    if (unlikely(o->refcount <= 0)) {
        serverPanic("illegal decrRefCount for object with: type %u, encoding %u, refcount %d",
            o->type, o->encoding, o->refcount);
    }

    if (--(o->refcount) == 0) {
        /* Fast path for embedded strings: no inner allocation to free, and we
         * can compute the alloc size to hint jemalloc for a faster deallocation. */
        if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_EMBSTR && !o->iskvobj) {  /* ▶ kvobj가 아닌 embstr (명령 인자 등) */
            serverAssert(sdsType(o->ptr) == SDS_TYPE_8); /* embstr always type_8 */
            zfree_with_size(o, sizeof(robj) + sdsAllocSize(o->ptr));  /* ▶ 할당 한 번이므로 free도 한 번 */
            return;
        }

        void *alloc = o;
        
        if (o->iskvobj) {
            /* eval real allocation pointer */
            alloc = kvobjGetAllocPtr(o);  /* ▶ 메타데이터가 앞에 붙어 있으므로 실제 할당 시작점으로 되돌림 */
            /* if kvobj has metadata attached. */
            if (getModuleMetaBits(o->metabits))
                keyMetaOnFree((kvobj *)o);
        }
        
        if (o->ptr != NULL) {
            switch(o->type) {
            case OBJ_STRING: freeStringObject(o); break;  /* ▶ raw면 sds 하나 해제. embstr/int면 할 일 없음 */
            case OBJ_LIST: freeListObject(o); break;
            case OBJ_SET: freeSetObject(o); break;  /* ▶ ★ 타입별 해제. 비용은 아래 [5-2]처럼 인코딩에 따라 다름 */
            case OBJ_ZSET: freeZsetObject(o); break;
            case OBJ_HASH: freeHashObject(o); break;  /* ▶ Hash는 t_hash.c hashTypeFree. listpack이면 free 한 번, HT면 dictRelease(필드마다 free) */
            case OBJ_MODULE: freeModuleObject(o); break;
            case OBJ_STREAM: freeStreamObject(o); break;
#ifdef ENABLE_GCRA
            case OBJ_GCRA: freeGCRAObject(o); break;
#endif
            case OBJ_ARRAY: freeArrayObject(o); break;
            default: serverPanic("Unknown object type"); break;
            }
        }
        zfree(alloc);  /* ▶ 객체 헤더(+ 붙어 있는 키, embstr 값) 해제 */
    }
}

/* ======================================================================
 * ▶ [5-2] free*Object — object.c L559~L612
 * ====================================================================== */
void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(o->ptr);  /* ▶ raw만 따로 할당된 sds가 있음 */
    }
}

void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease(o->ptr);  /* ▶ O(노드 수). 노드 하나가 listpack 하나 */
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        lpFree(o->ptr);  /* ▶ O(1) */
    } else {
        serverPanic("Unknown list encoding type");
    }
}

void freeSetObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
#ifdef DEBUG_ASSERTIONS
        dictEmpty(o->ptr, NULL);
        debugServerAssert(*htGetMetadataSize(o->ptr) == 0);
#endif
        dictRelease((dict*) o->ptr);  /* ▶ ★ O(N): 원소마다 sds free + 버킷 배열 free */
        break;
    case OBJ_ENCODING_INTSET:
    case OBJ_ENCODING_LISTPACK:
        zfree(o->ptr);  /* ▶ ★ O(1): 연속 메모리 하나 */
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case OBJ_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);  /* ▶ O(N) */
        zslFree(zs->zsl);  /* ▶ O(N). skiplist 노드마다 free */
        zfree(zs);
        break;
    case OBJ_ENCODING_LISTPACK:
        zfree(o->ptr);  /* ▶ O(1) */
        break;
    default:
        serverPanic("Unknown sorted set encoding");
    }
}

void freeHashObject(robj *o) {
    hashTypeFree(o);
}


/* ######################################################################
 * [6] 관찰 도구 — OBJECT ENCODING, MEMORY USAGE
 *   OBJECT ENCODING은 encoding 4비트를 이름으로 바꿔 줄 뿐입니다.
 *   MEMORY USAGE는 unstable에서 kvobjAllocSize로 바로 계산합니다. 내장 타입은 샘플링하지 않고
 *   타입별로 추적해 둔 크기를 더하므로, SAMPLES 옵션은 모듈 타입에서만 의미가 있습니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [6-1] strEncoding — object.c L1311~L1328
 *   OBJECT ENCODING이 돌려주는 이름. "hashtable"은 OBJ_ENCODING_HT입니다.
 * ====================================================================== */
char *strEncoding(int encoding) {
    switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw";
    case OBJ_ENCODING_INT: return "int";
    case OBJ_ENCODING_HT: return "hashtable";
    case OBJ_ENCODING_QUICKLIST: return "quicklist";
    case OBJ_ENCODING_LISTPACK: return "listpack";
    case OBJ_ENCODING_LISTPACK_EX: return "listpackex";
    case OBJ_ENCODING_INTSET: return "intset";
    case OBJ_ENCODING_SKIPLIST: return "skiplist";
    case OBJ_ENCODING_EMBSTR: return "embstr";
    case OBJ_ENCODING_STREAM: return "stream";
    case OBJ_ENCODING_SLICED_ARRAY: return "sliced-array";
	case OBJ_ENCODING_TMPL_LP: return "template-listpack";
	case OBJ_ENCODING_TMPL_ARRAY: return "template-array";
    default: return "unknown";
    }
}

/* ======================================================================
 * ▶ [6-2] kvobjComputeSize, kvobjAllocSize — object.c L1332~L1397
 * ====================================================================== */
/* Returns the size in bytes consumed by the object header, key and value in RAM.
 * Note that the returned value is just an approximation, especially in the
 * case of aggregated data types where only "sample_size" elements
 * are checked and averaged to estimate the total size. */
#define OBJ_COMPUTE_SIZE_DEF_SAMPLES 5 /* Default sample size. */  /* ▶ 내장 타입은 아래에서 이 값을 쓰지 않음 */
size_t kvobjComputeSize(robj *key, kvobj *o, size_t sample_size, int dbid) {
    if (o->type == OBJ_STRING ||
        o->type == OBJ_LIST ||
        o->type == OBJ_SET ||
        o->type == OBJ_ZSET ||
        o->type == OBJ_HASH ||
        o->type == OBJ_STREAM ||
#ifdef ENABLE_GCRA
        o->type == OBJ_GCRA ||
#endif
        o->type == OBJ_ARRAY)
    {
        return kvobjAllocSize(o);  /* ▶ ★ 내장 타입: 샘플링 없이 계산 */
    } else if (o->type == OBJ_MODULE) {
        return zmalloc_size(o) + moduleGetMemUsage(key, o, sample_size, dbid);
    }
    serverPanic("Unknown object type");
}

/* Returns the size in bytes consumed by the object header, key and value in RAM.
 * Note that the returned value is accurate approximation of the actual allocated
 * size. For performance reasons it accumulates requested size instead in several
 * cases (e.g. kvobj allocation, type 5 sds, listpacks, etc) but it does so in a
 * self-consistent way.
 */
size_t kvobjAllocSize(kvobj *o) {
    debugServerAssert(o->iskvobj);
    size_t asize = sizeof(kvobj) + sizeof(kvBits);  /* ▶ 헤더 16 + kvBits 1 */
    /* Add metadata size */
    asize += getNumMeta(o->metabits) * sizeof(uint64_t);
    /* Add embedded key size */
    asize += sdsAllocSize(kvobjGetKey(o));  /* ▶ 키스페이스의 키 이름도 MEMORY USAGE에 포함됨 */
    /* Add embedded string size */
    if (o->encoding == OBJ_ENCODING_EMBSTR)
        asize += sdsAllocSize(o->ptr);  /* ▶ embstr이면 값이 같은 할당 안에 있음 */

    if (o->type == OBJ_STRING) {
        asize += stringObjectAllocSize(o);
    } else if (o->type == OBJ_LIST) {
        asize += listTypeAllocSize(o);
    } else if (o->type == OBJ_SET) {
        asize += setTypeAllocSize(o);
    } else if (o->type == OBJ_ZSET) {
        asize += zsetAllocSize(o);
    } else if (o->type == OBJ_HASH) {
        asize += hashTypeAllocSize(o);  /* ▶ Hash: listpack이면 lpBytes, HT면 dict 메모리 + 필드별로 추적한 크기 (t_hash.c L2775) */
    } else if (o->type == OBJ_STREAM) {
        stream *s = o->ptr;
        asize += s->alloc_size;
#ifdef ENABLE_GCRA
    } else if (o->type == OBJ_GCRA) {
        asize += gcraTypeAllocSize(o);
#endif
    } else if (o->type == OBJ_ARRAY) {
        redisArray *ar = o->ptr;
        asize += ar->alloc_size;
    } else if (o->type == OBJ_MODULE) {
        /* TODO: Provide moduleGetAllocSize() module API for O(1) allocation size retrieval */
    }
    return asize;
}

/* ======================================================================
 * ▶ [6-3] objectCommand (ENCODING, REFCOUNT 부분) — object.c L1767~L1820
 * ====================================================================== */
/* Object command allows to inspect the internals of a Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime|freq> <key> */
void objectCommand(client *c) {
    kvobj *kv;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        /* ▶ [생략] 원본 L1773~L1788: HELP 문구 */
    } else if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((kv = kvobjCommandLookupOrReply(c, c->argv[2], shared.null[c->resp]))
                == NULL) return;
        addReplyLongLong(c, kv->refcount);  /* ▶ kvobj의 refcount. 8.2 이후 공유 정수 값도 1로 나옴 ([2-4] 참고) */
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((kv = kvobjCommandLookupOrReply(c, c->argv[2], shared.null[c->resp]))
                == NULL) return;
        addReplyBulkCString(c, strEncoding(kv->encoding));  /* ▶ ★ OBJECT ENCODING key */
    /* ▶ [생략] 원본 L1797~L1818: IDLETIME, FREQ 처리와 구문 오류 분기 */
    }
}

/* ======================================================================
 * ▶ [6-4] memoryCommand (USAGE 부분) — object.c L1822~L2028
 * ====================================================================== */
/* The memory command will eventually be a complete interface for the
 * memory introspection capabilities of Redis.
 *
 * Usage: MEMORY usage <key> */
void memoryCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"help") && c->argc == 2) {
        /* ▶ [생략] 원본 L1828~L1842: HELP 문구 */
    } else if (!strcasecmp(c->argv[1]->ptr,"usage") && c->argc >= 3) {
        kvobj *kv;
        long long samples = OBJ_COMPUTE_SIZE_DEF_SAMPLES;  /* ▶ 기본 5. 내장 타입에는 쓰이지 않음 ([6-2]) */
        for (int j = 3; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"samples") &&
                j+1 < c->argc)
            {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&samples,NULL)
                     == C_ERR) return;
                if (samples < 0) {
                    addReplyErrorObject(c,shared.syntaxerr);
                    return;
                }
                if (samples == 0) samples = LLONG_MAX;
                j++; /* skip option argument. */
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
        if ((kv = dbFind(c->db, c->argv[2]->ptr)) == NULL) {  /* ▶ LRU/LFU를 건드리지 않고 키 찾기 */
            addReplyNull(c);
            return;
        }
        size_t usage = kvobjComputeSize(c->argv[2], kv, samples, c->db->id);  /* ▶ ★ MEMORY USAGE key */

        /* Add this key's share of the hash-template cost (see the comment of
         * hashTemplatePerKeyMemoryShare()). */
        if (kv->type == OBJ_HASH &&
            (kv->encoding == OBJ_ENCODING_TMPL_LP ||
             kv->encoding == OBJ_ENCODING_TMPL_ARRAY))
            usage += hashTemplatePerKeyMemoryShare(kv);

        addReplyLongLong(c,usage);
    /* ▶ [생략] 원본 L1877~L2027: STATS, DOCTOR, MALLOC-STATS, PURGE 처리 */
}
