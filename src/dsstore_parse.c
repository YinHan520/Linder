/*
 * dsstore_parse.c —— macOS .DS_Store 二进制解析器（纯 C 实现）
 *
 * 精确实现（结构对照 sindresorhus/DSStore Swift 库 + 0day.work 逆向）：
 *
 * 头部（36 字节）：
 *   [0]  alignment (4) = 0x01
 *   [4]  magic (4) = "Bud1" 0x42756431
 *   [8]  rootBlockOffset (4)
 *   [12] rootBlockSize (4)
 *   [16] rootBlockOffsetCheck (4)（应等于 rootBlockOffset）
 *   [20] 16 字节未知
 *
 * Allocator（在 rootBlockOffset + 4）：
 *   blockCount (4)
 *   unknown (4) = 0
 *   blockAddresses[addressCount]（addressCount = max(256, blockCount 向上取整到 256)）
 *   tableOfContentsCount (4)
 *   每个 TOC：nameLength(1) + name + blockNumber(4)   ← "DSDB" 指向 B-tree header
 *   freeLists[32]
 *
 * B-tree（DSDB 指向的 block 是 B-tree header）：
 *   rootNodeBlockNumber (4)
 *   treeHeight (4)
 *   recordCount (4)
 *   nodeCount (4)
 *   pageSize (4) = 0x1000
 *   然后从 rootNodeBlockNumber 开始遍历节点
 *
 * 节点（node）：
 *   rightmostChild (4) = 0 表示叶子
 *   recordCount (4)
 *   叶子：直接读 recordCount 条记录
 *   内部：交替 [childBlockNumber(4) 递归] + [record]，最后递归 rightmostChild
 *
 * 记录（record）：
 *   filenameLength (4, UTF-16 字符数) + filename(UTF-16BE) + structureType(4) + dataType(4) + value
 *
 * dataType → value：
 *   blob = 长度(4) + 数据（可能是 plist）
 *   ustr = 长度(4) + UTF-16 字符串
 *   type = 4 字节 FourCC
 *   bool = 1 字节
 *   long/shor = 4 字节
 *
 * 关键字段（structureType）：
 *   "BKGD" (blob) = 文件夹背景（PctB=图片 / 其它=纯色）
 *   "icvp"/"lsvp" (blob) = 图标/列表视图设置（plist）
 *   "icvo" (blob) = 图标选项（含图标大小）
 *   "Iloc" (blob) = 图标位置
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore_parse.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------- 大端读取 ---------- */

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static uint16_t rd16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* ---------- 初始化 / 释放 ---------- */

void dsdata_init(DsStoreData *d) {
    if (!d) return;
    d->bg_type = DSBG_NONE;
    d->bg_r = d->bg_g = d->bg_b = 0;
    d->bg_pict = NULL;
    d->bg_pict_len = 0;
    d->bg_pict_path = NULL;
    d->view = NULL;
    d->icon_size = -1;
    d->has_icon_pos = 0;
    d->icon_x = d->icon_y = 0;
}

void dsdata_free(DsStoreData *d) {
    if (!d) return;
    if (d->bg_pict) { free(d->bg_pict); d->bg_pict = NULL; }
    if (d->bg_pict_path) { free(d->bg_pict_path); d->bg_pict_path = NULL; }
    if (d->view) { free(d->view); d->view = NULL; }
    d->bg_pict_len = 0;
}

/* ---------- 解析上下文 ---------- */

typedef struct {
    const uint8_t *data;
    size_t         len;
    uint32_t      *addrs;     /* blockAddresses */
    int            n_addrs;
    DsStoreData   *out;
    int            depth;
} Ctx;

/* block 地址解码：offset = (addr>>5)<<5, size = 1<<(addr&0x1f) */
static void decode_addr(uint32_t addr, uint32_t *offset, uint32_t *size) {
    *offset = (addr >> 5) << 5;
    *size   = 1u << (addr & 0x1f);
}

/* 从 block id 拿 offset+size；越界返回 0。注意：block 实际数据从 decode 出的 offset + 4 开始 */
static int block_off(Ctx *c, uint32_t id, uint32_t *off, uint32_t *size) {
    if (id >= (uint32_t)c->n_addrs) return -1;
    uint32_t addr = c->addrs[id];
    decode_addr(addr, off, size);
    *off += 4; /* block-alignment: 数据从 offset+4 开始 */
    if (*off >= c->len) return -1;
    return 0;
}

/* 提取 structureType FourCC 字符串 */
static void type_str(uint32_t t, char out[5]) {
    out[0] = (char)((t >> 24) & 0xFF);
    out[1] = (char)((t >> 16) & 0xFF);
    out[2] = (char)((t >> 8) & 0xFF);
    out[3] = (char)(t & 0xFF);
    out[4] = 0;
}

/* ---------- book（Bookmark）路径解析 ---------- */

/* 小端读 32 位 */
static uint32_t rd32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 从 Apple Bookmark 读一个数据项头：返回 payload 指针；*ilen=长度 *itype=类型 */
static int bmk_item(const uint8_t *data, size_t dlen, uint32_t off,
                    uint32_t *ilen, uint32_t *itype, const uint8_t **payload) {
    if ((uint64_t)off + 8 > dlen) return 0;
    uint32_t l = rd32_le(data + off);
    uint32_t t = rd32_le(data + off + 4);
    if ((uint64_t)off + 8 + l > dlen) return 0;
    *ilen = l; *itype = t; *payload = data + off + 8;
    return 1;
}

/* 读一个字符串项，返回 malloc 的 UTF-8（调用者 free） */
static char *bmk_str(const uint8_t *data, size_t dlen, uint32_t off) {
    uint32_t l, t; const uint8_t *p;
    if (!bmk_item(data, dlen, off, &l, &t, &p)) return NULL;
    if ((t & 0xFFFFFF00) != 0x0100) return NULL;  /* 非字符串 */
    char *s = malloc(l + 1);
    if (!s) return NULL;
    memcpy(s, p, l); s[l] = 0;
    return s;
}

/*
 * 从 book（Bookmark）字节流解析出目标路径。
 * 结构（小端）：[0:4]"book" [4:8]总长 [8:12]版本 [12:16]header大小(48)
 * data_base = bk + header_size；first_toc_off = rd32_le(data_base)
 * 所有 data/TOC offset 都相对 data_base（不额外 +4）。
 * TOC 头 20 字节：size(4)+magic(4)+id(4)+next(4)+count(4)，后接 count 个
 * entry(key 4 + value_off 4 + reserved 4)。
 * 0x1004 = 目标路径数组，0x2002 = 卷路径（通常 "/"）。返回 malloc 路径。
 */
static char *book_extract_path(const uint8_t *bk, size_t bklen) {
    if (!bk || bklen < 16) return NULL;
    if (memcmp(bk, "book", 4) != 0) return NULL;

    uint32_t hdrsize = rd32_le(bk + 12);
    if (hdrsize < 16 || hdrsize + 4 > bklen) return NULL;

    const uint8_t *data = bk + hdrsize;
    size_t dlen = bklen - hdrsize;
    if (dlen < 4) return NULL;

    uint32_t toc_off = rd32_le(data);   /* 相对 data 起点 */

    uint32_t tgt_off = 0, vol_off = 0;
    int has_tgt = 0, has_vol = 0;
    int depth;
    for (depth = 0; depth < 32 && toc_off != 0; depth++) {
        if ((uint64_t)toc_off + 20 > dlen) return NULL;
        uint32_t count = rd32_le(data + toc_off + 16);
        if (count > 100000) return NULL;
        if ((uint64_t)toc_off + 20 + (uint64_t)count * 12 > dlen) return NULL;
        uint32_t next = rd32_le(data + toc_off + 12);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = data + toc_off + 20 + i * 12;
            uint32_t key = rd32_le(e);
            uint32_t val = rd32_le(e + 4);
            if (key == 0x1004) { tgt_off = val; has_tgt = 1; }
            else if (key == 0x2002) { vol_off = val; has_vol = 1; }
        }
        if (has_tgt) break;
        toc_off = next;
    }
    if (!has_tgt) return NULL;

    /* 卷路径（可选，通常是 "/"） */
    char *vol = NULL;
    if (has_vol) vol = bmk_str(data, dlen, vol_off);

    /* 目标路径数组 */
    uint32_t l, t; const uint8_t *p;
    if (!bmk_item(data, dlen, tgt_off, &l, &t, &p)) { free(vol); return NULL; }
    if ((t & 0xFFFFFF00) != 0x0600) { free(vol); return NULL; }  /* 非数组 */
    if ((l & 3) != 0) { free(vol); return NULL; }
    uint32_t n = l / 4;
    if (n == 0 || n > 256) { free(vol); return NULL; }

    char *segs[256];
    uint32_t nseg = 0, i;
    for (i = 0; i < n && nseg < 256; i++) {
        uint32_t eoff = rd32_le(p + i * 4);
        char *seg = bmk_str(data, dlen, eoff);
        if (seg) segs[nseg++] = seg;
    }

    size_t total = 2;
    if (vol && vol[0]) total += strlen(vol);
    else total += 1;
    for (i = 0; i < nseg; i++) total += strlen(segs[i]) + 1;

    char *path = malloc(total);
    if (!path) {
        for (i = 0; i < nseg; i++) free(segs[i]);
        free(vol);
        return NULL;
    }
    path[0] = 0;
    if (vol && vol[0] && strcmp(vol, "/") != 0) strcat(path, vol);
    if (path[0] == 0) strcat(path, "/");
    for (i = 0; i < nseg; i++) {
        size_t plen = strlen(path);
        if (plen == 0 || path[plen-1] != '/') strcat(path, "/");
        strcat(path, segs[i]);
        free(segs[i]);
    }
    free(vol);
    return path;
}

/*
 * 读取一条记录。p 指向记录起始；end 是 block 结束。
 * 返回下一条记录起始（或 NULL 表示越界）。
 * filename 会被跳过；structureType/dataType/value 用于提取背景/图标/视图字段。
 */
static const uint8_t *read_record(Ctx *c, const uint8_t *p, const uint8_t *end) {
    if (p + 4 > end) return NULL;
    uint32_t name_len = rd32(p);  /* UTF-16 字符数 */
    p += 4;
    if (p + name_len * 2 + 8 > end) return NULL; /* filename + 2 个 FourCC */
    p += name_len * 2;            /* 跳过文件名 */

    uint32_t stype = rd32(p); p += 4;  /* structureType (如 BKGD/icvp/Iloc) */
    uint32_t dtype = rd32(p); p += 4;  /* dataType (如 blob/ustr/long) */

    char st[5]; type_str(stype, st);
    char dt[5]; type_str(dtype, dt);

    if (getenv("DSSTORE_DEBUG")) {
        fprintf(stderr, "    [rec] filename_len=%u stype=%s dtype=%s\n",
                name_len, st, dt);
    }

    /* ---- 按 dataType 读取 value ---- */
    if (strcmp(dt, "blob") == 0 || strcmp(dt, "book") == 0) {
        if (p + 4 > end) return NULL;
        uint32_t blen = rd32(p); p += 4;
        if (p + blen > end) return NULL;
        const uint8_t *v = p;
        /* 处理各类 structureType 的 blob */
        if (strcmp(st, "BKGD") == 0) {
            /*
             * BKGD blob（12 字节）：文件夹背景。
             *   前 4 字节 FourCC：
             *     "DefB" = 默认背景
             *     "ClrB" = 纯色背景（后跟 RGB 6 字节 + 2 未知）
             *     "PctB" = 图片背景（后跟 pict 长度 + 4 未知；图片在 pict 记录里）
             */
            if (blen >= 12) {
                char kind[5];
                kind[0]=(char)v[0]; kind[1]=(char)v[1];
                kind[2]=(char)v[2]; kind[3]=(char)v[3]; kind[4]=0;
                if (strcmp(kind, "PctB") == 0) {
                    c->out->bg_type = DSBG_PICTURE;
                } else if (strcmp(kind, "ClrB") == 0) {
                    c->out->bg_type = DSBG_COLOR;
                    c->out->bg_r = rd16(v + 4);
                    c->out->bg_g = rd16(v + 6);
                    c->out->bg_b = rd16(v + 8);
                } else {
                    /* DefB 或未知：默认背景 */
                    c->out->bg_type = DSBG_NONE;
                }
            }
        }
        else if (strcmp(st, "icvp") == 0 || strcmp(st, "lsvp") == 0 || strcmp(st, "lsvP") == 0 || strcmp(st, "lsvC") == 0) {
            if (!c->out->view) {
                if (strcmp(st, "icvp") == 0) c->out->view = strdup("icon");
                else c->out->view = strdup("list");
            }
        }
        else if (strcmp(st, "icvo") == 0) {
            /*
             * icvo blob：图标视图选项，两种格式：
             *   "icvo" 版：4字节"icvo" + 8未知 + 2字节图标大小 + 4"none"
             *   "icv4" 版：4字节"icv4" + 2字节图标大小 + 4CC 排列 + ...
             * 两者图标大小都在偏移 12 处（“icv4”的图标大小在 12，符合文档）。
             * 但 icv4 版本图标大小在偏移 4。这里兼容：优先 icv4，否则 icvo。
             */
            if (blen >= 14) {
                char ver[5];
                ver[0]=(char)v[0]; ver[1]=(char)v[1];
                ver[2]=(char)v[2]; ver[3]=(char)v[3]; ver[4]=0;
                if (strcmp(ver, "icv4") == 0) {
                    c->out->icon_size = rd16(v + 4);
                } else {
                    c->out->icon_size = rd16(v + 12);
                }
            }
        }
        else if (strcmp(st, "Iloc") == 0) {
            if (blen >= 8) {
                c->out->has_icon_pos = 1;
                c->out->icon_x = (int)rd32(v);
                c->out->icon_y = (int)rd32(v + 4);
            }
        }
        else if (strcmp(st, "pict") == 0) {
            /* pict（data type 为 book 或 blob） = 背景图片的 Alias/Bookmark 记录（指向真实图片文件） */
            c->out->bg_type = DSBG_PICTURE;
            if (c->out->bg_pict) { free(c->out->bg_pict); c->out->bg_pict = NULL; }
            c->out->bg_pict = malloc(blen);
            if (c->out->bg_pict) {
                memcpy(c->out->bg_pict, v, blen);
                c->out->bg_pict_len = blen;
            }
            /* 可靠判别是 payload magic "book"，不是 dataType 字符串 */
            if (blen >= 4 && memcmp(v, "book", 4) == 0) {
                char *pth = book_extract_path(v, blen);
                if (pth) {
                    if (c->out->bg_pict_path) free(c->out->bg_pict_path);
                    c->out->bg_pict_path = pth;
                }
            }
        }
        p += blen;
        return p;
    }
    else if (strcmp(dt, "ustr") == 0) {
        if (p + 4 > end) return NULL;
        uint32_t slen = rd32(p); p += 4;
        if (p + slen * 2 > end) return NULL;
        p += slen * 2;
        return p;
    }
    else if (strcmp(dt, "type") == 0) {
        return p + 4;
    }
    else if (strcmp(dt, "bool") == 0) {
        return p + 1;
    }
    else if (strcmp(dt, "long") == 0 || strcmp(dt, "shor") == 0) {
        return p + 4;
    }
    else if (strcmp(dt, "comp") == 0 || strcmp(dt, "dutc") == 0) {
        return p + 8;
    }
    else if (strcmp(dt, "null") == 0) {
        return p;
    }
    /* 未知 dataType：安全结束 */
    return end;
}

/* 递归遍历节点 */
static void traverse_node(Ctx *c, uint32_t block_id) {
    if (c->depth > 1024) return;
    uint32_t off, size;
    if (block_off(c, block_id, &off, &size) != 0) return;
    if (off + size > c->len) return;

    const uint8_t *p = c->data + off;
    const uint8_t *end = c->data + off + size;

    if (p + 8 > end) return;
    uint32_t rightmost = rd32(p);   /* 0 = 叶子 */
    uint32_t count = rd32(p + 4);
    p += 8;

    if (getenv("DSSTORE_DEBUG")) {
        fprintf(stderr, "  [node] id=%u off=0x%x size=0x%x rightmost=%u count=%u depth=%d\n",
                block_id, off, size, rightmost, count, c->depth);
    }

    if (rightmost == 0) {
        /* 叶子：读 count 条记录 */
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *np = read_record(c, p, end);
            if (!np) break;
            p = np;
        }
    } else {
        /* 内部节点：交替 [child 递归] + [record]，最后递归 rightmost */
        for (uint32_t i = 0; i < count; i++) {
            if (p + 4 > end) break;
            uint32_t child = rd32(p);
            p += 4;
            c->depth++;
            traverse_node(c, child);
            c->depth--;

            const uint8_t *np = read_record(c, p, end);
            if (!np) break;
            p = np;
        }
        c->depth++;
        traverse_node(c, rightmost);
        c->depth--;
    }
}

/* 解析 allocator（blockAddresses + TOC + free list），返回 DSDB 指向的 B-tree header block id */
static int parse_allocator(Ctx *c, uint32_t alloc_off, uint32_t *btree_hdr_id) {
    const uint8_t *p = c->data + alloc_off;
    const uint8_t *end = c->data + c->len;
    if (p + 8 > end) return -1;

    uint32_t block_count = rd32(p); p += 4;
    p += 4; /* unknown (应为 0) */
    if (block_count == 0 || block_count > 65536) return -1;

    uint32_t addr_count = block_count < 256 ? 256 : ((block_count + 255) & ~255u);
    if (p + (size_t)addr_count * 4 > end) return -1;

    c->n_addrs = (int)block_count;
    c->addrs = malloc(sizeof(uint32_t) * block_count);
    if (!c->addrs) return -1;
    for (uint32_t i = 0; i < block_count; i++) c->addrs[i] = rd32(p + i * 4);
    p += (size_t)addr_count * 4;

    if (p + 4 > end) { free(c->addrs); c->addrs = NULL; return -1; }
    uint32_t toc_count = rd32(p); p += 4;

    *btree_hdr_id = 0;
    for (uint32_t i = 0; i < toc_count && p + 1 <= end; i++) {
        uint8_t name_len = p[0]; p += 1;
        if (p + name_len + 4 > end) break;
        char name[16] = {0};
        for (uint8_t k = 0; k < name_len && k < 15; k++) name[k] = (char)p[k];
        p += name_len;
        uint32_t val = rd32(p); p += 4;
        if (strcmp(name, "DSDB") == 0) *btree_hdr_id = val;
    }

    if (*btree_hdr_id == 0) { free(c->addrs); c->addrs = NULL; return -1; }
    return 0; /* free list 暂不解析（不影响提取字段） */
}

/* 主入口 */
int dsstore_parse_file(const char *path, DsStoreData *out) {
    if (!path || !out) return -1;
    dsdata_init(out);

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long flen = ftell(fp);
    if (flen < 0) { fclose(fp); return -1; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    if (flen < 36) { fclose(fp); return -1; }

    uint8_t *buf = malloc((size_t)flen);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)flen, fp) != (size_t)flen) { free(buf); fclose(fp); return -1; }
    fclose(fp);

    /* 校验 magic */
    if (rd32(buf + 4) != 0x42756431u) { free(buf); return -1; }

    uint32_t root_off = rd32(buf + 8);
    uint32_t root_size = rd32(buf + 12);

    /* 范围校验：root_off + 4（allocator 起点）必须在文件内 */
    if (root_off > (uint32_t)flen || root_off + 4 > (uint32_t)flen) {
        free(buf);
        return -1;
    }

    Ctx c;
    c.data = buf;
    c.len = (size_t)flen;
    c.addrs = NULL;
    c.n_addrs = 0;
    c.out = out;
    c.depth = 0;

    uint32_t btree_hdr_id = 0;
    if (parse_allocator(&c, root_off + 4, &btree_hdr_id) != 0) {
        free(buf);
        return -1;
    }

    /* B-tree header block */
    uint32_t hoff, hsize;
    if (block_off(&c, btree_hdr_id, &hoff, &hsize) != 0) {
        free(c.addrs); free(buf); return -1;
    }
    const uint8_t *hp = buf + hoff;
    if (hoff + 20 > c.len) { free(c.addrs); free(buf); return -1; }
    uint32_t root_node = rd32(hp);  /* rootNodeBlockNumber */

    if (getenv("DSSTORE_DEBUG")) {
        fprintf(stderr, "[btree] hdr_id=%u hoff=0x%x hsize=0x%x root_node=%u\n",
                btree_hdr_id, hoff, hsize, root_node);
    }

    /* 从根节点开始遍历 */
    traverse_node(&c, root_node);

    free(c.addrs);
    free(buf);
    (void)root_size;
    return 0;
}
