/* vma.c: the VMA table, an AVL tree keyed by start address. Owner: mm.
 *
 * Every lookup, insertion and removal is O(log n). Each node also keeps the end of its predecessor and
 * the largest gap (start - prev_end) of its subtree, so the free-range search skips full subtrees, like
 * the gap-augmented maple tree of Linux (mm/vma.c, vm_unmapped_area). Entries are about 100 bytes; the
 * path and label strings live in the shared backing object (HyperLinux kept a 4 KiB path in every
 * entry of a sorted array and moved the array on each insertion).
 */
#include <stdlib.h>

#include "mm_priv.h"

static int ht(const mm_vma *x) { return x ? x->h : 0; }
static uint64_t mg(const mm_vma *x) { return x ? x->maxgap : 0; }

static void upd(mm_vma *x)
{
    int hl = ht(x->l), hr = ht(x->r);
    x->h = 1 + (hl > hr ? hl : hr);
    uint64_t g = x->start - x->prev_end;
    if (mg(x->l) > g) g = mg(x->l);
    if (mg(x->r) > g) g = mg(x->r);
    x->maxgap = g;
}

static mm_vma *rotr(mm_vma *y)
{
    mm_vma *x = y->l;
    y->l = x->r;
    x->r = y;
    upd(y);
    upd(x);
    return x;
}

static mm_vma *rotl(mm_vma *x)
{
    mm_vma *y = x->r;
    x->r = y->l;
    y->l = x;
    upd(x);
    upd(y);
    return y;
}

static mm_vma *bal(mm_vma *n)
{
    upd(n);
    int b = ht(n->l) - ht(n->r);
    if (b > 1) {
        if (ht(n->l->l) < ht(n->l->r)) n->l = rotl(n->l);
        return rotr(n);
    }
    if (b < -1) {
        if (ht(n->r->r) < ht(n->r->l)) n->r = rotr(n->r);
        return rotl(n);
    }
    return n;
}

static mm_vma *ins(mm_vma *n, mm_vma *v)
{
    if (!n) return v;
    if (v->start < n->start) n->l = ins(n->l, v);
    else n->r = ins(n->r, v);
    return bal(n);
}

static mm_vma *rm_min(mm_vma *n, mm_vma **min)
{
    if (!n->l) { *min = n; return n->r; }
    n->l = rm_min(n->l, min);
    return bal(n);
}

static mm_vma *rm(mm_vma *n, uint64_t key)
{
    if (!n) return NULL;
    if (key < n->start) n->l = rm(n->l, key);
    else if (key > n->start) n->r = rm(n->r, key);
    else {
        mm_vma *l = n->l, *r = n->r;
        n->l = n->r = NULL;
        if (!r) return l;
        mm_vma *m;
        r = rm_min(r, &m);
        m->l = l;
        m->r = r;
        return bal(m);
    }
    return bal(n);
}

/* Recompute the aggregates on the path to key (after prev_end or start of that node changed). */
static void refresh(mm_vma *n, uint64_t key)
{
    if (!n) return;
    if (key < n->start) refresh(n->l, key);
    else if (key > n->start) refresh(n->r, key);
    upd(n);
}

void vt_init(vtree *t, uint64_t lo) { *t = (vtree){ .lo = lo }; }

/* ---- Views of shared objects (struct vt_view) ------------------------------------------------- */

uint64_t vt_view_base(const mm_vma *v) { return v->start - (v->pgoff + v->bk->hbias); }

int vt_view_same_object(const struct vt_view *e, const struct mm_backing *bk)
{
    if (e->bk == bk) return 1;
    if (e->kind != bk->kind) return 0;
    if (bk->kind == MB_FILE) return e->file == bk->file || (e->ino == bk->ino && e->dev_major == bk->dev_major && e->dev_minor == bk->dev_minor);
    if (bk->kind == MB_SYSV) return e->shm_id == bk->shm_id;
    return 0;
}

void vt_views_clear(vtree *t) { t->nviews = t->views_live = 0; }

static void views_push(vtree *t, const mm_vma *v, uint64_t base)
{
    if (t->nviews == t->views_cap) {
        uint32_t cap = t->views_cap ? t->views_cap * 2 : 16;
        struct vt_view *nv = realloc(t->views, cap * sizeof *nv);
        if (!nv) return;
        t->views = nv;
        t->views_cap = cap;
    }
    const struct mm_backing *b = v->bk;
    t->views[t->nviews++] = (struct vt_view){ .base = base, .bk = b, .file = b->file, .ino = b->ino,
                                              .dev_major = b->dev_major, .dev_minor = b->dev_minor,
                                              .kind = b->kind, .shm_id = b->shm_id };
}

static int views_find(vtree *t, const mm_vma *v, uint64_t base)
{
    uint32_t l = t->views_last;   /* the pieces of one mapping come one after the other (mprotect splits) */
    if (l < t->nviews && t->views[l].base == base && vt_view_same_object(&t->views[l], v->bk)) return 1;
    for (uint32_t i = 0; i < t->nviews; i++)
        if (t->views[i].base == base && vt_view_same_object(&t->views[i], v->bk)) { t->views_last = i; return 1; }
    return 0;
}

/* Entries of unmapped placements stay until the list has doubled since it was last built from the tree. */
static void views_add(vtree *t, const mm_vma *v)
{
    if (!(v->vflags & VF_HOSTSHARED) || !v->bk) return;
    uint64_t base = vt_view_base(v);
    if (views_find(t, v, base)) return;
    if (t->nviews >= 64 && t->nviews >= 2 * t->views_live) {
        t->nviews = 0;
        for (mm_vma *w = vt_first(t); w; w = vt_next(t, w)) {
            if (w == v || !(w->vflags & VF_HOSTSHARED) || !w->bk) continue;
            uint64_t b = vt_view_base(w);
            if (!views_find(t, w, b)) views_push(t, w, b);
        }
        t->views_live = t->nviews;
        if (views_find(t, v, base)) return;
    }
    views_push(t, v, base);
}

/* vm_stat_account with is_stack_mapping, is_exec_mapping and is_data_mapping (mm/internal.h): a VMA counts in
 * one class at most, the stack first. */
static uint64_t *class_of(vtree *t, const mm_vma *v)
{
    if (v->lkind == LBL_STACK) return &t->stack;
    if ((v->prot & (LX_PROT_WRITE | LX_PROT_EXEC)) == LX_PROT_EXEC) return &t->exec;
    if ((v->prot & LX_PROT_WRITE) && !(v->vflags & VF_SHARED)) return &t->data;
    return NULL;
}

void vt_account(vtree *t, const mm_vma *v, int sign)
{
    uint64_t sz = v->end - v->start, *c = class_of(t, v);
    if (sign > 0) {
        t->total += sz;
        if (c) *c += sz;
    } else {
        t->total -= sz;
        if (c) *c -= sz;
    }
}

mm_vma *vt_first_end_after(const vtree *t, uint64_t addr)
{
    mm_vma *n = t->root, *best = NULL;
    while (n) {
        if (n->end > addr) { best = n; n = n->l; }
        else n = n->r;
    }
    return best;
}

mm_vma *vt_find(const vtree *t, uint64_t addr)
{
    mm_vma *v = vt_first_end_after(t, addr);
    return v && v->start <= addr ? v : NULL;
}

mm_vma *vt_last_start_before(const vtree *t, uint64_t addr)
{
    mm_vma *n = t->root, *best = NULL;
    while (n) {
        if (n->start < addr) { best = n; n = n->r; }
        else n = n->l;
    }
    return best;
}

static mm_vma *first_start_ge(const vtree *t, uint64_t addr)
{
    mm_vma *n = t->root, *best = NULL;
    while (n) {
        if (n->start >= addr) { best = n; n = n->l; }
        else n = n->r;
    }
    return best;
}

mm_vma *vt_next(const vtree *t, const mm_vma *v) { return first_start_ge(t, v->start + 1); }
mm_vma *vt_prev(const vtree *t, const mm_vma *v) { return vt_last_start_before(t, v->start); }

mm_vma *vt_first(const vtree *t)
{
    mm_vma *n = t->root;
    while (n && n->l) n = n->l;
    return n;
}

mm_vma *vt_last(const vtree *t)
{
    mm_vma *n = t->root;
    while (n && n->r) n = n->r;
    return n;
}

void vt_insert(vtree *t, mm_vma *v)
{
    mm_vma *pred = vt_last_start_before(t, v->start);
    mm_vma *succ = first_start_ge(t, v->start);
    v->l = v->r = NULL;
    v->h = 1;
    v->prev_end = pred ? pred->end : t->lo;
    v->maxgap = v->start - v->prev_end;
    t->root = ins(t->root, v);
    t->n++;
    vt_account(t, v, 1);
    views_add(t, v);
    if (succ) {
        succ->prev_end = v->end;
        refresh(t->root, succ->start);
    }
}

void vt_remove(vtree *t, mm_vma *v)
{
    mm_vma *succ = vt_next(t, v);
    uint64_t pe = v->prev_end;
    t->root = rm(t->root, v->start);
    t->n--;
    vt_account(t, v, -1);
    v->l = v->r = NULL;
    if (succ) {
        succ->prev_end = pe;
        refresh(t->root, succ->start);
    }
}

void vt_set_start(vtree *t, mm_vma *v, uint64_t s)
{
    vt_account(t, v, -1);
    v->start = s;
    vt_account(t, v, 1);
    refresh(t->root, s);
}

void vt_set_end(vtree *t, mm_vma *v, uint64_t e)
{
    mm_vma *succ = vt_next(t, v);
    vt_account(t, v, -1);
    v->end = e;
    vt_account(t, v, 1);
    if (succ) {
        succ->prev_end = e;
        refresh(t->root, succ->start);
    }
}

/* ---- Gap search -------------------------------------------------------------------------------- */

struct srch {
    const vtree *t;
    uint64_t len, low, high;
    int topdown;
    vt_fit_fn fit;
    void *ctx;
};

static uint64_t try_gap(struct srch *s, mm_vma *node)
{
    uint64_t glo = node->prev_end, ghi = node->start;
    if (ghi <= s->low || glo >= s->high) return 0;
    if (glo < s->low) glo = s->low;
    if (ghi > s->high) ghi = s->high;
    if (ghi - glo < s->len) return 0;
    return s->fit(s->ctx, glo, ghi, vt_prev(s->t, node), node);
}

static uint64_t walk(struct srch *s, mm_vma *n)
{
    if (!n || n->maxgap < s->len) return 0;
    uint64_t r;
    if (s->topdown) {
        if (n->end < s->high && (r = walk(s, n->r))) return r;
        if ((r = try_gap(s, n))) return r;
        if (n->prev_end > s->low && (r = walk(s, n->l))) return r;
    } else {
        if (n->prev_end > s->low && (r = walk(s, n->l))) return r;
        if ((r = try_gap(s, n))) return r;
        if (n->end < s->high && (r = walk(s, n->r))) return r;
    }
    return 0;
}

uint64_t vt_search(const vtree *t, uint64_t len, uint64_t low, uint64_t high, uint64_t top, int topdown,
                   vt_fit_fn fit, void *ctx)
{
    struct srch s = { t, len, low, high, topdown, fit, ctx };
    mm_vma *last = vt_last(t);
    uint64_t tail_lo = last ? last->end : t->lo, tail_hi = top < high ? top : high;
    if (tail_lo < low) tail_lo = low;
    uint64_t r;
    if (topdown && tail_hi > tail_lo && tail_hi - tail_lo >= len && (r = fit(ctx, tail_lo, tail_hi, last, NULL)))
        return r;
    if ((r = walk(&s, t->root))) return r;
    if (!topdown && tail_hi > tail_lo && tail_hi - tail_lo >= len && (r = fit(ctx, tail_lo, tail_hi, last, NULL)))
        return r;
    return 0;
}

/* ---- Invariant check (tests) ------------------------------------------------------------------- */

static int chk(const mm_vma *n, uint64_t lo, uint64_t hi, int *bad)
{
    if (!n) return 0;
    if (n->start < lo || n->start >= hi || n->end <= n->start) (*bad)++;
    int hl = chk(n->l, lo, n->start, bad), hr = chk(n->r, n->start + 1, hi, bad);
    if (n->h != 1 + (hl > hr ? hl : hr)) (*bad)++;
    if (hl - hr > 1 || hr - hl > 1) (*bad)++;
    uint64_t g = n->start - n->prev_end;
    if (mg(n->l) > g) g = mg(n->l);
    if (mg(n->r) > g) g = mg(n->r);
    if (g != n->maxgap) (*bad)++;
    return n->h;
}

int vt_check(const vtree *t)
{
    int bad = 0;
    chk(t->root, 0, UINT64_MAX, &bad);
    uint64_t pe = t->lo;
    size_t n = 0;
    vtree sums = { 0 };
    for (mm_vma *v = vt_first(t); v; v = vt_next(t, v)) {
        if (v->prev_end != pe || v->start < pe) bad++;
        pe = v->end;
        n++;
        vt_account(&sums, v, 1);
    }
    if (n != t->n) bad++;
    if (sums.total != t->total || sums.stack != t->stack || sums.exec != t->exec || sums.data != t->data) bad++;
    return bad;
}
