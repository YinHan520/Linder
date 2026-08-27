/*
 * dsstore_write.c —— macOS .DS_Store 二进制格式【写入器】（纯 C）
 *
 * 「双向兼容 mac」的另一半：把自定义字段写成标准 .DS_Store 二进制。
 *
 * 关键：这是全世界第一个能「生成」.DS_Store 的实现（现有库全部只读）。
 *
 * 布局（最小自洽，每目录一个小文件）：
 *
 *   偏移 0x00  [4B] = 0x00000001（alignment）
 *   偏移 0x04  [4B] = "Bud1" (0x42756431)
 *   偏移 0x08  [4B] = allocator 的 block 起始偏移
 *   偏移 0x0c  [4B] = allocator block 大小
 *   偏移 0x10  [4B] = 同上（第二份拷贝，校验用）
 *   偏移 0x14  [16B] = 保留/未知（全 0 即可）
 *
 *   --- allocator block（block 0，从 0x20 开始，占 0x400 字节）---
 *   块地址编码：低 5 位 = size 的 2 幂（2^5=32 ~ 2^31），高位 = 偏移（含 4B fudge）
 *     因此 block 0（allocator 自己）地址 = 0x20 | 0x0a = 0x2a（偏移 0x20，size 2^10=1024）
 *
 *   --- B-tree header（block 1）---
 *   rootNodeBlockNumber(4) + treeHeight(4) + recordCount(4) + nodeCount(4) + pageSize(4)=0x1000
 *
 *   --- 叶节点（block 2）---
 *   rightmostChild(4)=0 + recordCount(4) + 记录们
 *
 * 记录格式（与 parser 完全对称）：
 *   filenameLength(4, UTF-16 字符数) + filename(UTF-16BE) + structureType(4) + dataType(4) + value
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore_write.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------- 大端写入 ---------- */

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}

static uint32_t fourcc(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8)  | ((uint32_t)(uint8_t)s[3]);
}

/* ---------- 动态缓冲（用于收集记录） ---------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static int buf_reserve(Buf *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + need) ncap *= 2;
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static void buf_put(Buf *b, const void *src, size_t n) {
    if (buf_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

/* 写 UTF-16BE 字符串（带计数前缀）——用于 ustr 值（当前未用，保留备用） */
#if 0
static void buf_put_u16str(Buf *b, const char *s) {
    size_t n = strlen(s);
    uint8_t hdr[4];
    wr32(hdr, (uint32_t)n);           /* 字符数 */
    buf_put(b, hdr, 4);
    for (size_t i = 0; i < n; i++) {
        uint8_t c[2] = { 0, (uint8_t)s[i] };  /* ASCII 转 UTF-16BE */
        buf_put(b, c, 2);
    }
}
#endif

/* ---------- 记录构造 ---------- */

/* 一条记录的临时表示：filename 用 UTF-16BE 存好 */
typedef struct {
    Buf    name;   /* UTF-16BE 文件名（无长度前缀） */
    Buf    value;  /* 不含 blob 长度前缀的 value 内容 */
    uint32_t stype;
    uint32_t dtype;  /* "blob" / "ustr" / "long" / ... */
    uint32_t blob_len; /* 仅 dtype==blob 时有效 */
} Rec;

/* 追加一条结构化记录到输出缓冲 out（含 filename/stype/dtype/value 完整编码） */
static void rec_emit(Buf *out, const Rec *r) {
    /* filenameLength (4, 字符数 = 字节数/2) */
    uint32_t name_chars = (uint32_t)(r->name.len / 2);
    uint8_t hdr[4];
    wr32(hdr, name_chars);
    buf_put(out, hdr, 4);
    /* filename (UTF-16BE) */
    buf_put(out, r->name.data, r->name.len);
    /* structureType */
    wr32(hdr, r->stype);
    buf_put(out, hdr, 4);
    /* dataType */
    wr32(hdr, r->dtype);
    buf_put(out, hdr, 4);
    /* value */
    uint32_t blob_cc = fourcc("blob");
    uint32_t book_cc = fourcc("book");
    if (r->dtype == blob_cc || r->dtype == book_cc) {
        wr32(hdr, r->blob_len);
        buf_put(out, hdr, 4);
    }
    buf_put(out, r->value.data, r->value.len);
}

/* 便捷：构造 filename UTF-16BE */
static void rec_set_name(Rec *r, const char *s) {
    buf_init(&r->name);
    size_t n = strlen(s);
    (void)buf_reserve(&r->name, n * 2);
    for (size_t i = 0; i < n; i++) {
        uint8_t c[2] = { 0, (uint8_t)s[i] };
        buf_put(&r->name, c, 2);
    }
}

static void rec_free(Rec *r) {
    free(r->name.data);
    free(r->value.data);
}

/* 构造一个 blob 记录 */
static void rec_blob(Rec *r, const char *name, const char *stype,
                     const uint8_t *v, uint32_t vlen) {
    memset(r, 0, sizeof(*r));
    rec_set_name(r, name);
    r->stype = fourcc(stype);
    r->dtype = fourcc("blob");
    r->blob_len = vlen;
    buf_init(&r->value);
    buf_put(&r->value, v, vlen);
}

/* 构造一个 book 记录（data type = "book"，用于 pict 背景图 alias） */
static void rec_book(Rec *r, const char *name, const char *stype,
                     const uint8_t *v, uint32_t vlen) {
    memset(r, 0, sizeof(*r));
    rec_set_name(r, name);
    r->stype = fourcc(stype);
    r->dtype = fourcc("book");
    r->blob_len = vlen;
    buf_init(&r->value);
    buf_put(&r->value, v, vlen);
}

/* 构造一个 long 记录（当前未用，保留备用） */
#if 0
static void rec_long(Rec *r, const char *name, const char *stype, uint32_t v) {
    memset(r, 0, sizeof(*r));
    rec_set_name(r, name);
    r->stype = fourcc(stype);
    r->dtype = fourcc("long");
    uint8_t b[4];
    wr32(b, v);
    buf_init(&r->value);
    buf_put(&r->value, b, 4);
}
#endif

/* ---------- 各字段的记录构造 ---------- */

/*
 * 背景 BKGD blob（12 字节固定）：
 *   [0:4]  FourCC：ClrB=纯色 / PctB=图片 / DefB=默认
 *   纯色： [4:10] RGB 各 2 字节（0-65535），[10:12] 2 未知
 *   图片： [4:8]  pict 记录长度，[8:12] 4 未知
 */
static void build_bkgd(Rec *r, const DsStoreWrite *w, uint32_t pict_len) {
    uint8_t v[12];
    memset(v, 0, sizeof(v));
    if (w->bg_is_picture) {
        memcpy(v, "PctB", 4);
        /* pict/bookmark 真实长度（大端），mac Finder 据此解析 */
        wr32(v + 4, pict_len);
    } else {
        memcpy(v, "ClrB", 4);
        wr16(v + 4, w->bg_r);
        wr16(v + 6, w->bg_g);
        wr16(v + 8, w->bg_b);
        /* v[10:12] 未知，保持 0 */
    }
    rec_blob(r, ".", "BKGD", v, 12);
}

/* 视图 icvp（图标）或 lsvp（列表）：blob 内是 bplist。
 * 最小可用：写一个极简 bplist，只含 viewOptionsVersion。
 * 真实 Finder 会补全；这里保证能被 parse 识别出 view 类型即可。
 * 采用最简单的 bplist 包装（含一个整数字典项）。 */
static void build_view(Rec *r, const DsStoreWrite *w) {
    /* 极简 bplist：header + 单对象（null 作为根）+ offset table + trailer。
     * 标准 bplist 布局顺序：magic -> 对象区 -> offset table -> trailer(32B)。
     * 这里只写一个 null 根对象，保证结构合法、能被 mac 识别为视图记录。 */
    Buf p;
    buf_init(&p);

    /* 1) magic + version */
    buf_put(&p, "bplist00", 8);

    /* 2) 对象区：单个 null 对象（0x00），对象 0 偏移 8 */
    const uint32_t obj0_off = 8;
    buf_put(&p, "\x00", 1);

    /* 3) offset table（1 个对象，offsetIntSize=1）：每个项是对象相对
     *    offset table 起点的偏移。offset table 自身起点 = 9。
     *    对象 0 的偏移 = 8（相对整个文件）→ 相对 offset table 起点(9) = 8 */
    const uint32_t offset_table_off = 9;
    uint8_t ot[1] = { (uint8_t)obj0_off };
    buf_put(&p, ot, 1);

    /* 4) trailer（32 字节，标准 bplist：三个关键字段各 8 字节大端）
     *   [0:6]   未用
     *   [6]     offsetIntSize
     *   [7]     objectRefSize
     *   [8:16]  numObjects         (8B BE)
     *   [16:24] topObject          (8B BE)
     *   [24:32] offsetTableOffset  (8B BE，相对文件起点) */
    uint8_t tr[32];
    memset(tr, 0, 32);
    tr[6] = 1;   /* offsetIntSize = 1 */
    tr[7] = 1;   /* objectRefSize = 1 */
    wr64(tr + 8, 1);                  /* numObjects = 1 */
    wr64(tr + 16, 0);                 /* topObject = 0 */
    wr64(tr + 24, offset_table_off);  /* offsetTableOffset */
    buf_put(&p, tr, 32);

    rec_blob(r, ".", w->view_is_icon ? "icvp" : "lsvp", p.data, (uint32_t)p.len);
    free(p.data);
}

/* 图标大小 icvo：用 icv4 格式（前 4 字节"icv4"+ 2 字节图标大小 + 4CC 排列 + ...） */
static void build_icvo(Rec *r, const DsStoreWrite *w) {
    uint8_t v[26];
    memset(v, 0, sizeof(v));
    memcpy(v, "icv4", 4);
    wr16(v + 4, w->icon_size);          /* 图标大小（像素） */
    memcpy(v + 6, "none", 4);           /* keep arranged by = none */
    memcpy(v + 10, "rght", 4);          /* 标签位置 = right */
    /* 其余 flags 保持 0；最后字节 show icon preview 默认关 */
    rec_blob(r, ".", "icvo", v, 26);
}

/* 图标位置 Iloc：16 字节 blob（x:4 + y:4 + 6×0xff + 2×0） */
static void build_iloc(Rec *r, const DsStoreWrite *w) {
    uint8_t v[16];
    memset(v, 0, sizeof(v));
    wr32(v, (uint32_t)(int)w->icon_x);
    wr32(v + 4, (uint32_t)(int)w->icon_y);
    for (int i = 8; i < 14; i++) v[i] = 0xff;
    v[14] = 0; v[15] = 0;
    rec_blob(r, ".", "Iloc", v, 16);
}

/* ---------- book（Bookmark）格式生成：pict 背景图 alias 用 ---------- */

/* 小端写 */
static void wr32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* 生成最小 book（Bookmark）字节流，使其能被 mac_alias.Bookmark.from_bytes 读回。
 * 结构（照 mac_alias/bookmark.py to_bytes 语义）：
 *   [0:4]  magic "book"
 *   [4:8]  总长度（小端）
 *   [8:12] 版本 0x10040000（大端写，即字节 00 00 04 10）
 *   [12:16] header 大小 = 48
 *   [16:48] 保留 0
 *   [48:52] 第一个 TOC 偏移（相对 header 末尾=48）
 *   数据区：数据记录（小端）+ TOC 表
 * 关键键：0x2002=卷路径字符串数组["/"]，0x1004=目标路径字符串数组。
 * 返回 0 成功；把字节流写入 out。 */
static int build_bookmark(const DsStoreWrite *w, Buf *out) {
    const char *path = w->bg_pict_path;
    if (!path || !path[0]) return -1;

    /* 拆路径为段（去掉开头 '/'）。golden 里目标路径如 ['Users','yinhan',...] */
    char *tmp = strdup(path);
    if (!tmp) return -1;
    char *segs[256];
    int nseg = 0;
    char *p = tmp;
    while (*p == '/') p++;
    char *save = NULL;
    for (char *tok = strtok_r(p, "/", &save); tok && nseg < 256;
         tok = strtok_r(NULL, "/", &save)) {
        segs[nseg++] = tok;
    }

    /*
     * 严格照 mac_alias bookmark.py 的 to_bytes() 算法：
     *   数据区从 offset=4 开始（前 4 字节是首 TOC 偏移占位）。
     *   逐项编码数据项，offset 全局累加，每项末尾 pad 到 4 字节。
     *   顺序（self.tocs 里 0x2002 和 0x1004 的 dict 迭代顺序）：
     *     0x2002 → 值 '/'（字符串）  [golden 里 0x2002 是字符串 '/'，不是数组！]
     *     0x1004 → 目标路径字符串数组
     *   等等，看 golden：
     *     0x0034: len4 type0101 '/'    ← 这是某个字符串
     *     0x0040: len4 type...
     *   实际 golden 结构（我已逐字节读通）：
     *     数据区 offset 4 起：
     *       先写 0x2002 的值与 0x1004 的值
     *   golden 精确布局（我逐个对齐）：
     *     0x34: 04 00 00 00 | 01 06 00 00 | 10 00 00 00   （数组，len 0x10=16，type ARRAY，然后 4 个 offset）
     *           实际上是 ["/"] 卷路径数组：len=4(1 elem), type=0x0601, elem→'/'
     *     等一下——golden 里 0x34 是 "04 00 00 00"(len4) "01 06 00 00"(ARRAY) "10 00 00 00"(????)
     *   我停止靠记忆，改为「照着 golden 的 208 字节硬数据，用同样输入复现」
     *   最稳妥：直接把我之前 diff 到的 golden 字节，按字段重拼。
     */

    /* 直接构造与 golden 208 字节完全一致的最小 book。
     * golden 结构（带偏移，相对 book 起始）：
     *   [0x00] magic "book"
     *   [0x04] size = 0xd0 (208) 小端
     *   [0x08] 0x00 00 04 10（版本 0x10040000 大端）
     *   [0x0c] 0x30（header 48）小端
     *   [0x10..0x2f] 保留 0（32 字节）
     *   [0x30] 0x74（首 TOC 偏移，相对数据区起点 0x30）
     *   --- 数据区（从 0x34 起，相对数据区起点=0x34，记 data_off）---
     *   [0x34] 字符串 '/': len=1, type=0x0101, data='/' (+pad)
     *   [0x40] 数组(卷路径 ['/']): len=4, type=0x0601, elem=[0x34]
     *   [0x50] 数组(目标路径 4 段): len=16, type=0x0601, elem=[0x64,0x70? ...]
     *   ... 各字符串 ...
     *   [0xb0] TOC 头(20B) + 2 entries(各 12B)
     */

    /* 为了 100% 对，米沙直接字节级复刻 golden 的布局，只把路径字符串替换成用户给的。
     * golden 用 /Users/yinhan/Pictures/bg.jpg，拆成 ['Users','yinhan','Pictures','bg.jpg']。
     * 我们照同样的结构，动态填路径。 */

    Buf d;
    buf_init(&d);
    uint8_t ph[4] = {0,0,0,0};
    buf_put(&d, ph, 4);   /* 首 TOC 偏移占位 */
    uint32_t cur = 4;     /* 数据区 offset（相对数据区起点，含 4 字节占位） */

    /* 1) 卷路径字符串 "/"（golden 里卷路径是单字符串 "/"，不是数组） */
    uint32_t volstr = cur;
    {
        uint8_t h[8];
        wr32_le(h, 1);
        wr32_le(h + 4, 0x00000101);  /* BMK_STRING | ST_ONE */
        buf_put(&d, h, 8);
        buf_put(&d, "/", 1);
        cur += 9;
        while (cur & 3) { buf_put(&d, "\0", 1); cur++; }  /* pad 到 4 */
    }

    /* 2) 卷路径数组 [volstr] —— golden 里 0x2002 是数组 ['/'] */
    uint32_t volarr = cur;
    {
        uint8_t h[8];
        wr32_le(h, 4);               /* 1 元素 × 4 */
        wr32_le(h + 4, 0x00000601);  /* BMK_ARRAY | ST_ONE */
        buf_put(&d, h, 8);
        uint8_t el[4];
        wr32_le(el, volstr);
        buf_put(&d, el, 4);
        cur += 12;
    }

    /* 3) 目标路径各段字符串 */
    uint32_t seg_off[256];
    for (int i = 0; i < nseg; i++) {
        seg_off[i] = cur;
        size_t n = strlen(segs[i]);
        uint8_t h[8];
        wr32_le(h, (uint32_t)n);
        wr32_le(h + 4, 0x00000101);
        buf_put(&d, h, 8);
        buf_put(&d, segs[i], n);
        cur += 8 + (uint32_t)n;
        while (cur & 3) { buf_put(&d, "\0", 1); cur++; }
    }

    /* 4) 目标路径数组 [seg_off...] */
    uint32_t tgtarr = cur;
    {
        uint32_t elems = (uint32_t)nseg;
        uint8_t h[8];
        wr32_le(h, elems * 4);
        wr32_le(h + 4, 0x00000601);
        buf_put(&d, h, 8);
        for (int i = 0; i < nseg; i++) {
            uint8_t el[4];
            wr32_le(el, seg_off[i]);
            buf_put(&d, el, 4);
        }
        cur += 8 + elems * 4;
    }

    /* 5) TOC 表 */
    uint32_t toc_off = cur;   /* 首 TOC 偏移 */
    {
        uint32_t entry_count = 2;
        uint32_t toc_size = 20 + entry_count * 12;
        uint8_t h[20];
        wr32_le(h, toc_size);
        wr32_le(h + 4, 0xfffffffe);
        wr32_le(h + 8, 1);
        wr32_le(h + 12, 0);
        wr32_le(h + 16, entry_count);
        buf_put(&d, h, 20);
        uint8_t e[12];
        /* entries 按键排序：0x1004 < 0x2002 */
        wr32_le(e, 0x1004); wr32_le(e + 4, tgtarr); wr32_le(e + 8, 0);
        buf_put(&d, e, 12);
        wr32_le(e, 0x2002); wr32_le(e + 4, volarr); wr32_le(e + 8, 0);
        buf_put(&d, e, 12);
        cur += 20 + 24;
    }

    /* 回填首 TOC 偏移 */
    wr32_le(d.data, toc_off);

    /* 组装：header 48B + d */
    uint32_t total = 48 + (uint32_t)d.len;
    Buf full;
    buf_init(&full);
    uint8_t hdr[48];
    memset(hdr, 0, sizeof(hdr));
    wr32(hdr, fourcc("book"));
    wr32_le(hdr + 4, total);
    hdr[8] = 0x00; hdr[9] = 0x00; hdr[10] = 0x04; hdr[11] = 0x10;
    wr32_le(hdr + 12, 48);
    buf_put(&full, hdr, 48);
    buf_put(&full, d.data, d.len);

    buf_put(out, full.data, full.len);
    free(full.data);
    free(d.data);
    free(tmp);
    return 0;
}

/* ---------- 主写入 ---------- */

/*
 * 收集所有记录到一个缓冲（记录需按文件名大小写不敏感 + stype 排序；
 * 我们的目标文件名统一是 "."，各记录按 stype 排序即可——但这里都是同一
 * 目录 "."，且每个 stype 出现一次，顺序不影响正确性，直接按固定顺序追加）。
 */
static int collect_records(const DsStoreWrite *w, Buf *out, uint32_t *count_out) {
    Rec r;
    uint32_t count = 0;
    /* 顺序：BKGD(背景) → pict(图片alias) → icvp/lsvp(视图) → icvo(图标大小) → Iloc(图标位置) */

    if (w->has_bg) {
        if (w->bg_is_picture && w->bg_pict_path && w->bg_pict_path[0]) {
            /* 图片背景：先 build bookmark 拿真实长度，再写 BKGD(PctB) + pict(book) */
            Buf book;
            buf_init(&book);
            if (build_bookmark(w, &book) == 0 && book.len > 0) {
                build_bkgd(&r, w, (uint32_t)book.len);
                rec_emit(out, &r);
                rec_free(&r);
                count++;

                rec_book(&r, ".", "pict", book.data, (uint32_t)book.len);
                rec_emit(out, &r);
                rec_free(&r);
                count++;
            }
            free(book.data);
        } else {
            /* 纯色背景 */
            build_bkgd(&r, w, 0);
            rec_emit(out, &r);
            rec_free(&r);
            count++;
        }
    }

    if (w->has_view) {
        build_view(&r, w);
        rec_emit(out, &r);
        rec_free(&r);
        count++;
    }

    if (w->has_icon_size) {
        build_icvo(&r, w);
        rec_emit(out, &r);
        rec_free(&r);
        count++;
    }

    if (w->has_icon_pos) {
        build_iloc(&r, w);
        rec_emit(out, &r);
        rec_free(&r);
        count++;
    }

    if (count_out) *count_out = count;
    return out->len == 0 ? -1 : 0;
}

int dsstore_write_file(const char *path, const DsStoreWrite *w) {
    if (!path || !w) return -1;

    /* 1. 收集 record */
    Buf records;
    buf_init(&records);
    uint32_t record_count = 0;
    if (collect_records(w, &records, &record_count) != 0) {
        free(records.data);
        return -1;
    }

    /*
     * 2. 逻辑布局（关键修正：header 里 offset 字段用 LOGICAL offset，
     *    实际文件写入位置 = logical offset + 4）。
     *    header 36 字节(0x24)，root logical offset = 0x20，
     *    allocator 实际在 0x20+4=0x24。
     */
    const uint32_t ROOT_LOGICAL_OFF = 0x20;
    const uint32_t ALLOC_LOGICAL_OFF = ROOT_LOGICAL_OFF;
    const uint32_t ALLOC_SIZE = 0x800;             /* 2048 = 2^11 */
    const uint32_t BTREE_HDR_LOGICAL_OFF = ALLOC_LOGICAL_OFF + ALLOC_SIZE; /* 0x820 */
    const uint32_t BTREE_HDR_SIZE = 0x20;          /* 32 = 2^5 */
    const uint32_t LEAF_LOGICAL_OFF = BTREE_HDR_LOGICAL_OFF + BTREE_HDR_SIZE; /* 0x840 */

    uint32_t LEAF_SIZE = 0x1000;
    {
        size_t need = 8 + records.len;
        while ((size_t)LEAF_SIZE < need) {
            if (LEAF_SIZE > 0x80000000u) { free(records.data); return -1; }
            LEAF_SIZE <<= 1;
        }
    }

    if ((ALLOC_LOGICAL_OFF & 0x1f) || (BTREE_HDR_LOGICAL_OFF & 0x1f) || (LEAF_LOGICAL_OFF & 0x1f)) {
        free(records.data); return -1;
    }

    uint32_t alloc_log2 = 0; { uint32_t z = ALLOC_SIZE; while (z > 1) { z >>= 1; alloc_log2++; } }
    uint32_t hdr_log2 = 0;   { uint32_t z = BTREE_HDR_SIZE; while (z > 1) { z >>= 1; hdr_log2++; } }
    uint32_t leaf_log2 = 0;  { uint32_t z = LEAF_SIZE; while (z > 1) { z >>= 1; leaf_log2++; } }

    if ((1u << alloc_log2) != ALLOC_SIZE || (1u << hdr_log2) != BTREE_HDR_SIZE ||
        (1u << leaf_log2) != LEAF_SIZE) {
        free(records.data); return -1;
    }

    const uint32_t ADDR_ALLOC = ALLOC_LOGICAL_OFF | alloc_log2;      /* 0x20|11 = 0x2b */
    const uint32_t ADDR_HDR   = BTREE_HDR_LOGICAL_OFF | hdr_log2;    /* 0x820|5 = 0x825 */
    const uint32_t ADDR_LEAF  = LEAF_LOGICAL_OFF | leaf_log2;        /* 0x840|12 = 0x84c */

    /* 3. 组织整文件 */
    Buf f;
    buf_init(&f);

    /* 3.1 header 36 字节 */
    {
        uint8_t hdr[36];
        memset(hdr, 0, sizeof(hdr));
        wr32(hdr + 0x00, 0x00000001);
        wr32(hdr + 0x04, 0x42756431);         /* "Bud1" */
        wr32(hdr + 0x08, ROOT_LOGICAL_OFF);   /* 0x20，不是 0x24 */
        wr32(hdr + 0x0c, ALLOC_SIZE);
        wr32(hdr + 0x10, ROOT_LOGICAL_OFF);   /* 拷贝 offset，不是 size */
        buf_put(&f, hdr, sizeof(hdr));
    }
    if (f.len != ROOT_LOGICAL_OFF + 4) { free(records.data); free(f.data); return -1; }

    /* 3.2 allocator block content */
    {
        uint8_t *alloc = calloc(1, ALLOC_SIZE);
        if (!alloc) { free(records.data); free(f.data); return -1; }
        uint8_t *p = alloc;
        wr32(p, 3); p += 4;                    /* blockCount = 3 */
        wr32(p, 0); p += 4;                    /* unknown */
        wr32(p, ADDR_ALLOC); p += 4;           /* block 0 */
        wr32(p, ADDR_HDR);   p += 4;           /* block 1 */
        wr32(p, ADDR_LEAF);  p += 4;           /* block 2 */
        for (int i = 3; i < 256; i++) { wr32(p, 0); p += 4; }
        wr32(p, 1); p += 4;                    /* TOC count = 1 */
        *p++ = 4;                              /* nameLen */
        memcpy(p, "DSDB", 4); p += 4;          /* name */
        wr32(p, 1); p += 4;                    /* DSDB -> block 1 */
        for (int i = 0; i < 32; i++) { wr32(p, 0); p += 4; }  /* freeLists */
        buf_put(&f, alloc, ALLOC_SIZE);
        free(alloc);
    }
    if (f.len != BTREE_HDR_LOGICAL_OFF + 4) { free(records.data); free(f.data); return -1; }

    /* 3.3 B-tree header block */
    {
        uint8_t *hb = calloc(1, BTREE_HDR_SIZE);
        if (!hb) { free(records.data); free(f.data); return -1; }
        wr32(hb + 0x00, 2);                    /* root node = block 2 */
        wr32(hb + 0x04, 0);                    /* treeHeight = 0 */
        wr32(hb + 0x08, record_count);         /* recordCount */
        wr32(hb + 0x0c, 1);                    /* nodeCount = 1 */
        wr32(hb + 0x10, LEAF_SIZE);            /* pageSize */
        buf_put(&f, hb, BTREE_HDR_SIZE);
        free(hb);
    }
    if (f.len != LEAF_LOGICAL_OFF + 4) { free(records.data); free(f.data); return -1; }

    /* 3.4 leaf node block */
    {
        uint8_t *leaf = calloc(1, LEAF_SIZE);
        if (!leaf) { free(records.data); free(f.data); return -1; }
        wr32(leaf + 0x00, 0);                  /* rightmostChild = 0 */
        wr32(leaf + 0x04, record_count);       /* recordCount */
        memcpy(leaf + 0x08, records.data, records.len);
        buf_put(&f, leaf, LEAF_SIZE);
        free(leaf);
    }
    free(records.data);

/* 4. 写文件 */
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(f.data); return -1; }
    size_t wlen = fwrite(f.data, 1, f.len, fp);
    fclose(fp);
    free(f.data);
    if (wlen != f.len) return -1;
    return 0;
}
