/**
 * @file chttp_h2_hpack.c
 * @brief Self-contained HPACK (RFC 7541) encoder/decoder.
 *
 * Static and Huffman tables are RFC 7541 protocol constants. Migrated from
 * the legacy HTTP repository commit 38f1e389b3f94909db6cb2482a8cbc16522e7e4f.
 */

#include "chttp_h2_hpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RFC 7541 Appendix A (61 entries) and Appendix B (Huffman codes). */
#include "chttp_h2_hpack_huffman_data.inc"
#include "chttp_h2_hpack_static.inc"

/* ── Writer ───────────────────────────────────────────────────────── */

int chttp_h2_hpack_buffer_init(chttp_h2_hpack_buffer *b, size_t hint, size_t max_capacity) {
  if (!b || max_capacity == 0u || hint > max_capacity) {
    return -1;
  }
  memset(b, 0, sizeof(*b));
  if (hint == 0) {
    hint = max_capacity < 256u ? max_capacity : 256u;
  }
  b->data = (uint8_t *)malloc(hint);
  if (!b->data) {
    return -1;
  }
  b->capacity = hint;
  b->max_capacity = max_capacity;
  return 0;
}

void chttp_h2_hpack_buffer_destroy(chttp_h2_hpack_buffer *b) {
  if (!b) {
    return;
  }
  free(b->data);
  memset(b, 0, sizeof(*b));
}

int chttp_h2_hpack_buffer_reserve(chttp_h2_hpack_buffer *b, size_t extra) {
  uint8_t *nd;
  size_t need;
  size_t cap;
  if (!b || b->size > b->capacity || b->capacity > b->max_capacity) {
    return -1;
  }
  if (extra <= b->capacity - b->size) {
    return 0;
  }
  if (extra > b->max_capacity - b->size) {
    return -1;
  }
  need = b->size + extra;
  cap = b->capacity ? b->capacity : (b->max_capacity < 256u ? b->max_capacity : 256u);
  while (cap < need) {
    size_t next = cap > b->max_capacity / 2u ? b->max_capacity : cap * 2u;
    if (next <= cap) return -1;
    cap = next;
  }
  nd = (uint8_t *)realloc(b->data, cap);
  if (!nd) {
    return -1;
  }
  b->data = nd;
  b->capacity = cap;
  return 0;
}

/* ── Integer representation (RFC 7541 §5.1) ───────────────────────── */

int chttp_h2_hpack_integer_encode(chttp_h2_hpack_buffer *b, uint32_t value, unsigned int prefix) {
  uint8_t first;
  uint32_t remaining;
  size_t encoded_size;
  int rc;

  if (prefix < 1u || prefix > 8u) {
    return -1;
  }
  first = (uint8_t)((1u << prefix) - 1u);
  if (value < (uint32_t)first) {
    rc = chttp_h2_hpack_buffer_reserve(b, 1);
    if (rc != 0) {
      return rc;
    }
    b->data[b->size++] = (uint8_t)value;
    return 0;
  }
  remaining = value - (uint32_t)first;
  encoded_size = 2u;
  while (remaining >= 128u) {
    ++encoded_size;
    remaining >>= 7;
  }
  rc = chttp_h2_hpack_buffer_reserve(b, encoded_size);
  if (rc != 0) {
    return rc;
  }
  b->data[b->size++] = first;
  value -= (uint32_t)first;
  while (value >= 128u) {
    b->data[b->size++] = (uint8_t)((value & 0x7fu) | 0x80u);
    value >>= 7;
  }
  b->data[b->size++] = (uint8_t)value;
  return 0;
}

/* Read an N-bit-prefix integer.  Returns 0 and stores the value, or -1. */
static int hpack_get_int(const uint8_t *in, size_t in_len, size_t *pos, int prefix,
                         uint32_t *out_value) {
  uint32_t first_mask;
  uint32_t value;
  size_t shift = 0;

  if (prefix < 1 || prefix > 8) {
    return -1;
  }
  first_mask = (uint32_t)((1u << prefix) - 1u);
  if (*pos >= in_len) {
    return -1;
  }
  value = in[(*pos)++] & first_mask;
  if (value < first_mask) {
    *out_value = value;
    return 0;
  }
  for (;;) {
    uint32_t byte;
    if (*pos >= in_len || shift > 28) {
      return -1; /* truncated or overflow */
    }
    byte = in[(*pos)++];
    value += (byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0) {
      break;
    }
    shift += 7;
  }
  *out_value = value;
  return 0;
}

/* ── String literal (RFC 7541 §5.2) ───────────────────────────────── */

static int hpack_put_string(chttp_h2_hpack_buffer *b, const char *s, size_t len, int huffman) {
  int rc;
  if (len > (size_t)0x7fffffff) {
    return -1;
  }
  if (huffman) {
    /* Huffman-encode the string; first byte = 0x80 | length... handled below. */
    size_t enc_len = 0;
    size_t i;
    uint64_t bits = 0;
    int nbits = 0;
    size_t out_off;
    /* Worst case: each byte expands to at most 30 bits -> 4 bytes. */
    size_t max_enc;
    if (len > (SIZE_MAX - 8u) / 4u) return -1;
    max_enc = len * 4u + 8u;
    if (max_enc > SIZE_MAX - 8u) return -1;
    if (chttp_h2_hpack_buffer_reserve(b, max_enc + 8) != 0) {
      return -1;
    }
    out_off = b->size + 1; /* reserve 1 byte for the length prefix */
    for (i = 0; i < len; i++) {
      uint8_t nb = s_huff_sym[(uint8_t)s[i]].nbits;
      uint32_t code = s_huff_sym[(uint8_t)s[i]].code;
      bits = (bits << nb) | (code >> (32 - nb));
      nbits += nb;
      while (nbits >= 8) {
        b->data[out_off++] = (uint8_t)(bits >> (nbits - 8));
        nbits -= 8;
      }
    }
    /* Padding: emit EOS prefix (all 1s) up to byte boundary. */
    if (nbits > 0) {
      bits = (bits << (8 - nbits)) | ((1u << (8 - nbits)) - 1u);
      b->data[out_off++] = (uint8_t)bits;
      nbits = 0;
    }
    enc_len = out_off - (b->size + 1);
    /* Write the length prefix (H flag + 7-bit length). */
    rc = chttp_h2_hpack_integer_encode(b, (uint32_t)enc_len, 7);
    if (rc != 0) {
      return rc;
    }
    b->data[b->size] |= 0x80u; /* set H flag on the first length byte */
    b->size = out_off;
    return 0;
  }
  rc = chttp_h2_hpack_integer_encode(b, (uint32_t)len, 7);
  if (rc != 0) {
    return rc;
  }
  rc = chttp_h2_hpack_buffer_reserve(b, len);
  if (rc != 0) {
    return rc;
  }
  memcpy(b->data + b->size, s, len);
  b->size += len;
  return 0;
}

/* Huffman decode state machine: a binary tree built from s_huff_sym. */
typedef struct hpack_huff_node_s {
  int16_t zero; /* child index, or -1 */
  int16_t one;
  int16_t sym; /* symbol if leaf, or -1 */
} hpack_huff_node_t;

typedef struct hpack_huff_tree_s {
  hpack_huff_node_t *nodes;
  size_t node_count;
  size_t node_cap;
} hpack_huff_tree_t;

enum { CHTTP_H2_HUFFMAN_MAX_NODES = 8192 };

static int huff_tree_add(hpack_huff_tree_t *t, uint32_t code, int nbits, int sym) {
  size_t node = 0;
  int i;
  for (i = 0; i < nbits; i++) {
    int bit = (int)((code >> (31 - i)) & 1u);
    if ((bit ? t->nodes[node].one : t->nodes[node].zero) < 0) {
      /* The specific child is missing: allocate a new node.  realloc may move
         the array, so the child pointer is recomputed after the realloc. */
      int16_t *child;
      size_t idx;
      if (t->node_count >= t->node_cap) {
        size_t ncap = t->node_cap ? t->node_cap * 2 : 16;
        if (t->node_cap > CHTTP_H2_HUFFMAN_MAX_NODES / 2u) ncap = CHTTP_H2_HUFFMAN_MAX_NODES;
        if (ncap <= t->node_cap || ncap > SIZE_MAX / sizeof(*t->nodes)) return -1;
        hpack_huff_node_t *nn = (hpack_huff_node_t *)realloc(t->nodes, ncap * sizeof(*nn));
        if (!nn) {
          return -1;
        }
        t->nodes = nn;
        t->node_cap = ncap;
      }
      idx = t->node_count++;
      t->nodes[idx].zero = -1;
      t->nodes[idx].one = -1;
      t->nodes[idx].sym = -1;
      child = bit ? &t->nodes[node].one : &t->nodes[node].zero;
      *child = (int16_t)idx;
    }
    node = (size_t)(bit ? t->nodes[node].one : t->nodes[node].zero);
  }
  t->nodes[node].sym = (int16_t)sym;
  return 0;
}

static int huff_tree_build(hpack_huff_tree_t *t) {
  int i;
  if (t->node_cap == 0) {
    t->node_cap = 16;
    t->nodes = (hpack_huff_node_t *)malloc(t->node_cap * sizeof(*t->nodes));
    if (!t->nodes) {
      return -1;
    }
    t->node_count = 1;
    t->nodes[0].zero = -1;
    t->nodes[0].one = -1;
    t->nodes[0].sym = -1;
  }
  for (i = 0; i < 257; i++) {
    if (huff_tree_add(t, s_huff_sym[i].code, s_huff_sym[i].nbits, i) != 0) {
      return -1;
    }
  }
  return 0;
}

/* ── Dynamic table (RFC 7541 §2.3.3) ──────────────────────────────── */

typedef struct hpack_entry_s {
  char *name;
  size_t name_size;
  char *value;
  size_t value_size;
  size_t size; /* name_len + value_len + 32 */
} hpack_entry_t;

typedef struct hpack_dyn_table_s {
  hpack_entry_t *entries; /* index 0 = newest */
  size_t count;
  size_t capacity;
  size_t max_entries;
  size_t size;
  size_t max_size;
} hpack_dyn_table_t;

static void dyn_entry_free(hpack_entry_t *e) {
  if (!e) {
    return;
  }
  free(e->name);
  free(e->value);
  e->name = NULL;
  e->value = NULL;
  e->name_size = e->value_size = e->size = 0;
}

static void dyn_table_destroy(hpack_dyn_table_t *t) {
  size_t i;
  if (!t) {
    return;
  }
  for (i = 0; i < t->count; i++) {
    dyn_entry_free(&t->entries[i]);
  }
  free(t->entries);
  memset(t, 0, sizeof(*t));
}

/* Evict oldest entries until size <= max_size (RFC 7541 §4.4). */
static void dyn_table_evict(hpack_dyn_table_t *t) {
  while (t->count > 0 && t->size > t->max_size) {
    size_t last = t->count - 1;
    t->size -= t->entries[last].size;
    dyn_entry_free(&t->entries[last]);
    t->count--;
  }
}

/* Add a header to the dynamic table (index 0 = newest). */
static int dyn_table_add(hpack_dyn_table_t *t, const char *name, size_t name_len, const char *value,
                         size_t value_len) {
  size_t esize;
  char *ncopy;
  char *vcopy;

  if (name_len > SIZE_MAX - value_len || name_len + value_len > SIZE_MAX - 32u) return -1;
  esize = name_len + value_len + 32u;
  if (esize > t->max_size) {
    return 0; /* entry larger than the table: not added (RFC 7541 §4.4) */
  }
  if (t->count >= t->capacity) {
    size_t ncap;
    if (t->count >= t->max_entries) return -1;
    ncap = t->capacity ? t->capacity * 2u : 8u;
    if (ncap > t->max_entries) ncap = t->max_entries;
    if (ncap <= t->capacity || ncap > SIZE_MAX / sizeof(*t->entries)) return -1;
    hpack_entry_t *ne = (hpack_entry_t *)realloc(t->entries, ncap * sizeof(*ne));
    if (!ne) {
      return -1;
    }
    t->entries = ne;
    t->capacity = ncap;
  }
  ncopy = (char *)malloc(name_len ? name_len : 1);
  vcopy = (char *)malloc(value_len ? value_len : 1);
  if (!ncopy || !vcopy) {
    free(ncopy);
    free(vcopy);
    return -1;
  }
  if (name_len) {
    memcpy(ncopy, name, name_len);
  }
  if (value_len) {
    memcpy(vcopy, value, value_len);
  }
  /* Shift entries right to insert at index 0. */
  if (t->count > 0) {
    memmove(&t->entries[1], &t->entries[0], t->count * sizeof(*t->entries));
  }
  t->entries[0].name = ncopy;
  t->entries[0].name_size = name_len;
  t->entries[0].value = vcopy;
  t->entries[0].value_size = value_len;
  t->entries[0].size = esize;
  t->count++;
  t->size += esize;
  dyn_table_evict(t);
  return 0;
}

/* Look up (name,value) in the dynamic table; returns the 1-based index
 * (62 + pos) or 0.  name_only selects name-only matching. */
static uint32_t dyn_table_find(const hpack_dyn_table_t *t, const char *name, size_t name_len,
                               const char *value, size_t value_len, int name_only) {
  size_t i;
  for (i = 0; i < t->count; i++) {
    const hpack_entry_t *e = &t->entries[i];
    if (e->name_size == name_len && memcmp(e->name, name, name_len) == 0 &&
        (name_only || (e->value_size == value_len && memcmp(e->value, value, value_len) == 0))) {
      return (uint32_t)(62 + i);
    }
  }
  return 0;
}

/* Static table lookups (RFC 7541 Appendix A). */
static uint32_t static_find_full(const char *name, size_t name_len, const char *value,
                                 size_t value_len) {
  size_t i;
  for (i = 0; i < 61; i++) {
    if (s_hpack_static[i].name_size == name_len &&
        memcmp(s_hpack_static[i].name, name, name_len) == 0 &&
        s_hpack_static[i].value_size == value_len &&
        memcmp(s_hpack_static[i].value, value, value_len) == 0) {
      return (uint32_t)(i + 1);
    }
  }
  return 0;
}

static uint32_t static_find_name(const char *name, size_t name_len) {
  size_t i;
  for (i = 0; i < 61; i++) {
    if (s_hpack_static[i].name_size == name_len &&
        memcmp(s_hpack_static[i].name, name, name_len) == 0) {
      return (uint32_t)(i + 1);
    }
  }
  return 0;
}

/* ── HPACK context ────────────────────────────────────────────────── */

struct chttp_h2_hpack_s {
  hpack_dyn_table_t encoder; /* encoder's dynamic table (peer table size) */
  hpack_dyn_table_t decoder; /* decoder's dynamic table (our table size) */
  hpack_huff_tree_t huff;    /* Huffman decode tree */
  size_t decoder_max_size;   /* our advertised SETTINGS_HEADER_TABLE_SIZE */
  size_t encoder_pending_min_size;
  size_t encoder_pending_final_size;
  int encoder_size_update_pending;
  size_t max_dynamic_table_bytes;
  size_t max_header_block_bytes;
  size_t max_string_bytes;
};

chttp_h2_hpack *chttp_h2_hpack_create(const chttp_h2_hpack_config *config) {
  chttp_h2_hpack *h = (chttp_h2_hpack *)calloc(1, sizeof(*h));
  if (!config || config->max_dynamic_table_bytes > UINT32_MAX ||
      config->max_header_block_bytes == 0u || config->max_string_bytes == 0u || !h) {
    free(h);
    return NULL;
  }
  h->max_dynamic_table_bytes = config->max_dynamic_table_bytes;
  h->max_header_block_bytes = config->max_header_block_bytes;
  h->max_string_bytes = config->max_string_bytes;
  h->encoder.max_size = config->max_dynamic_table_bytes;
  h->decoder.max_size = config->max_dynamic_table_bytes;
  h->decoder_max_size = config->max_dynamic_table_bytes;
  h->encoder.max_entries = config->max_dynamic_table_bytes / 32u;
  h->decoder.max_entries = config->max_dynamic_table_bytes / 32u;
  if (huff_tree_build(&h->huff) != 0) {
    free(h);
    return NULL;
  }
  return h;
}

void chttp_h2_hpack_destroy(chttp_h2_hpack *h) {
  if (!h) {
    return;
  }
  dyn_table_destroy(&h->encoder);
  dyn_table_destroy(&h->decoder);
  free(h->huff.nodes);
  free(h);
}

int chttp_h2_hpack_encoder_set_max_size(chttp_h2_hpack *h, size_t max_size) {
  if (!h || max_size > h->max_dynamic_table_bytes) return -1;
  if (max_size != h->encoder.max_size) {
    if (!h->encoder_size_update_pending || max_size < h->encoder_pending_min_size) {
      h->encoder_pending_min_size = max_size;
    }
    h->encoder_pending_final_size = max_size;
    h->encoder_size_update_pending = 1;
  }
  h->encoder.max_size = max_size;
  dyn_table_evict(&h->encoder);
  return 0;
}

int chttp_h2_hpack_decoder_set_max_size(chttp_h2_hpack *h, size_t max_size) {
  if (!h || max_size > h->max_dynamic_table_bytes) return -1;
  h->decoder.max_size = max_size;
  h->decoder_max_size = max_size;
  dyn_table_evict(&h->decoder);
  return 0;
}

int chttp_h2_hpack_decoder_table_size_update(chttp_h2_hpack *h, size_t new_size) {
  if (!h) {
    return -1;
  }
  if (new_size > h->decoder_max_size) {
    return -1; /* RFC 7541 §4.2: must not exceed the advertised limit */
  }
  h->decoder.max_size = new_size;
  dyn_table_evict(&h->decoder);
  return 0;
}

/* ── Huffman decode (RFC 7541 Appendix B) ─────────────────────────── */

/* Decode a Huffman string into a malloc'd buffer.  Returns 0 on success.
 * Rejects the EOS symbol and invalid padding. */
static int hpack_huff_decode(chttp_h2_hpack *h, const uint8_t *in, size_t in_len, uint8_t **out,
                             size_t *out_len) {
  size_t node = 0;
  size_t i;
  size_t pad_n = 0;
  uint8_t pad_bits[8];
  size_t out_cap;
  uint8_t *buf;

  if (in_len > h->max_string_bytes || in_len > (SIZE_MAX - 16u) / 2u) return -1;
  out_cap = in_len * 2u + 16u;
  if (out_cap > h->max_string_bytes) out_cap = h->max_string_bytes;
  if (out_cap == 0u) return -1;
  buf = (uint8_t *)malloc(out_cap);
  if (!buf) {
    return -1;
  }
  *out_len = 0;
  for (i = 0; i < in_len; i++) {
    int bit;
    for (bit = 7; bit >= 0; bit--) {
      int b = (int)((in[i] >> bit) & 1u);
      size_t child;
      if (node == (size_t)-1) {
        free(buf);
        return -1;
      }
      child = (size_t)(b ? h->huff.nodes[node].one : h->huff.nodes[node].zero);
      if (child == (size_t)-1) {
        free(buf);
        return -1; /* invalid code */
      }
      node = child;
      if (pad_n < sizeof(pad_bits)) {
        pad_bits[pad_n] = (uint8_t)b;
      }
      pad_n++;
      if (h->huff.nodes[node].sym >= 0) {
        int sym = h->huff.nodes[node].sym;
        if (sym == 256) {
          free(buf);
          return -1; /* EOS in payload */
        }
        if (*out_len >= out_cap) {
          uint8_t *nb;
          size_t next = out_cap > h->max_string_bytes / 2u ? h->max_string_bytes : out_cap * 2u;
          if (next <= out_cap) {
            free(buf);
            return -1;
          }
          out_cap = next;
          nb = (uint8_t *)realloc(buf, out_cap);
          if (!nb) {
            free(buf);
            return -1;
          }
          buf = nb;
        }
        buf[(*out_len)++] = (uint8_t)sym;
        node = 0;
        pad_n = 0;
      }
    }
  }
  /* RFC 7541 §5.2: padding must be the prefix of EOS (all ones) and at most
   * 7 bits.  Reject too-long padding and any zero bit in it. */
  if (pad_n > 7) {
    free(buf);
    return -1;
  }
  {
    size_t j;
    for (j = 0; j < pad_n; j++) {
      if (pad_bits[j] != 1) {
        free(buf);
        return -1;
      }
    }
  }
  *out = buf;
  return 0;
}

/* Read a string literal.  Returns 0 on success; *str points either into
 * @p in (literal) or a freshly allocated buffer (*owned = 1). */
static int hpack_get_string(chttp_h2_hpack *h, const uint8_t *in, size_t in_len, size_t *pos,
                            const char **str, size_t *str_len, int *owned) {
  uint32_t len;
  int huff;
  size_t p = *pos;

  *owned = 0;
  if (p >= in_len) {
    return -1;
  }
  huff = (in[p] & 0x80u) != 0;
  if (hpack_get_int(in, in_len, &p, 7, &len) != 0) {
    return -1;
  }
  if ((size_t)len > in_len - p) {
    return -1;
  }
  if ((size_t)len > h->max_string_bytes) return -1;
  if (huff) {
    uint8_t *buf;
    size_t buf_len;
    if (hpack_huff_decode(h, in + p, len, &buf, &buf_len) != 0) {
      return -1;
    }
    *str = (const char *)buf;
    *str_len = buf_len;
    *owned = 1;
  } else {
    *str = (const char *)(in + p);
    *str_len = len;
  }
  *pos = p + len;
  return 0;
}

/* Look up a table entry (static + dynamic combined) and return name/value. */
static int table_get(const chttp_h2_hpack *h, uint32_t index, const char **name, size_t *name_len,
                     const char **value, size_t *value_len) {
  if (index == 0) {
    return -1;
  }
  if (index <= 61) {
    *name = s_hpack_static[index - 1].name;
    *name_len = s_hpack_static[index - 1].name_size;
    *value = s_hpack_static[index - 1].value;
    *value_len = s_hpack_static[index - 1].value_size;
    return 0;
  }
  {
    uint32_t d = index - 62;
    if (d >= h->decoder.count) {
      return -1;
    }
    *name = h->decoder.entries[d].name;
    *name_len = h->decoder.entries[d].name_size;
    *value = h->decoder.entries[d].value;
    *value_len = h->decoder.entries[d].value_size;
    return 0;
  }
}

/* ── Header block encode (RFC 7541 §6) ────────────────────────────── */

static int hpack_put_table_size_update(chttp_h2_hpack_buffer *out, size_t size) {
  size_t start = out->size;
  if (chttp_h2_hpack_integer_encode(out, (uint32_t)size, 5) != 0) {
    return -1;
  }
  out->data[start] |= 0x20u;
  return 0;
}

int chttp_h2_hpack_encode(chttp_h2_hpack *h, chttp_h2_hpack_buffer *out,
                          const chttp_h2_hpack_header *hdrs, size_t count) {
  size_t i;

  if (!h || !out || out->max_capacity > h->max_header_block_bytes || (count && !hdrs)) {
    return -1;
  }
  if (h->encoder_size_update_pending) {
    if (hpack_put_table_size_update(out, h->encoder_pending_min_size) != 0) {
      return -1;
    }
    if (h->encoder_pending_final_size != h->encoder_pending_min_size &&
        hpack_put_table_size_update(out, h->encoder_pending_final_size) != 0) {
      return -1;
    }
    h->encoder_size_update_pending = 0;
  }

  for (i = 0; i < count; i++) {
    const chttp_h2_hpack_header *hd = &hdrs[i];
    uint32_t full;
    uint32_t name_idx;
    int indexed;

    if (!hd->name || !hd->value || hd->name_size > h->max_string_bytes ||
        hd->value_size > h->max_string_bytes)
      return -1;
    full = static_find_full(hd->name, hd->name_size, hd->value, hd->value_size);
    if (full == 0) {
      full = dyn_table_find(&h->encoder, hd->name, hd->name_size, hd->value, hd->value_size, 0);
    }
    if (full != 0) {
      /* Indexed header field. */
      size_t start = out->size;
      if (chttp_h2_hpack_integer_encode(out, full, 7) != 0) {
        return -1;
      }
      out->data[start] |= 0x80u; /* 1xxxxxxx */
      continue;
    }

    name_idx = static_find_name(hd->name, hd->name_size);
    if (name_idx == 0) {
      name_idx = dyn_table_find(&h->encoder, hd->name, hd->name_size, NULL, 0, 1);
    }

    if (h->encoder.max_size == 0) {
      /* Literal without indexing (peer disabled the dynamic table). */
      if (chttp_h2_hpack_integer_encode(out, name_idx, 4) != 0) {
        return -1;
      }
      if (name_idx == 0) {
        if (hpack_put_string(out, hd->name, hd->name_size, 0) != 0) {
          return -1;
        }
      }
      if (hpack_put_string(out, hd->value, hd->value_size, 0) != 0) {
        return -1;
      }
      continue;
    }

    /* Literal with incremental indexing. */
    indexed = 1;
    (void)indexed;
    {
      size_t start = out->size;
      if (chttp_h2_hpack_integer_encode(out, name_idx, 6) != 0) {
        return -1;
      }
      out->data[start] |= 0x40u; /* 01xxxxxx */
    }
    if (name_idx == 0) {
      if (hpack_put_string(out, hd->name, hd->name_size, 0) != 0) {
        return -1;
      }
    }
    if (hpack_put_string(out, hd->value, hd->value_size, 0) != 0) {
      return -1;
    }
    if (dyn_table_add(&h->encoder, hd->name, hd->name_size, hd->value, hd->value_size) != 0) {
      return -1;
    }
  }
  return 0;
}

/* ── Header block decode (RFC 7541 §6) ────────────────────────────── */

static int hpack_add_header_list_size(size_t *list_size, size_t name_size, size_t value_size,
                                      size_t max_size) {
  size_t field_size;
  if (max_size == 0u) {
    return 0;
  }
  if (name_size > SIZE_MAX - value_size || name_size + value_size > SIZE_MAX - 32u) {
    return -1;
  }
  field_size = name_size + value_size + 32u;
  if (*list_size > max_size || field_size > max_size - *list_size) {
    return -1;
  }
  *list_size += field_size;
  return 0;
}

int chttp_h2_hpack_decode(chttp_h2_hpack *h, const uint8_t *in, size_t in_len, size_t *consumed,
                          chttp_h2_hpack_callback cb, void *user_data,
                          size_t max_header_list_size) {
  size_t pos = 0;
  size_t list_bytes = 0;
  int header_list_too_large = 0;

  if (!h || !in || !consumed || in_len > h->max_header_block_bytes) {
    return -1;
  }
  *consumed = 0;
  while (pos < in_len) {
    uint8_t b = in[pos];
    if (b & 0x80u) {
      /* Indexed header field. */
      uint32_t index;
      const char *name = NULL;
      size_t name_len = 0;
      const char *value = NULL;
      size_t value_len = 0;
      if (hpack_get_int(in, in_len, &pos, 7, &index) != 0) {
        return -1;
      }
      if (table_get(h, index, &name, &name_len, &value, &value_len) != 0) {
        return -1;
      }
      if (!header_list_too_large &&
          hpack_add_header_list_size(&list_bytes, name_len, value_len, max_header_list_size) != 0)
        header_list_too_large = 1;
      if (!header_list_too_large && cb && cb(user_data, name, name_len, value, value_len) != 0) {
        return -3; /* callback aborted */
      }
    } else if (b & 0x40u) {
      /* Literal with incremental indexing. */
      uint32_t index;
      const char *name = NULL;
      size_t name_len = 0;
      const char *value = NULL;
      size_t value_len = 0;
      int n_owned = 0;
      int v_owned = 0;
      if (hpack_get_int(in, in_len, &pos, 6, &index) != 0) {
        return -1;
      }
      if (index == 0) {
        if (hpack_get_string(h, in, in_len, &pos, &name, &name_len, &n_owned) != 0) {
          return -1;
        }
      } else if (table_get(h, index, &name, &name_len, &value, &value_len) != 0) {
        return -1;
      }
      if (index != 0) {
        /* name came from the table; value follows */
        if (hpack_get_string(h, in, in_len, &pos, &value, &value_len, &v_owned) != 0) {
          if (n_owned) free((void *)name);
          return -1;
        }
      }
      /* For index == 0, both name and value were read as strings. */
      if (index == 0) {
        if (hpack_get_string(h, in, in_len, &pos, &value, &value_len, &v_owned) != 0) {
          if (n_owned) free((void *)name);
          return -1;
        }
      }
      if (!header_list_too_large &&
          hpack_add_header_list_size(&list_bytes, name_len, value_len, max_header_list_size) != 0)
        header_list_too_large = 1;
      if (!header_list_too_large && cb && cb(user_data, name, name_len, value, value_len) != 0) {
        if (n_owned) free((void *)name);
        if (v_owned) free((void *)value);
        return -3;
      }
      if (dyn_table_add(&h->decoder, name, name_len, value, value_len) != 0) {
        if (n_owned) free((void *)name);
        if (v_owned) free((void *)value);
        return -1;
      }
      if (n_owned) free((void *)name);
      if (v_owned) free((void *)value);
    } else if (b & 0x20u) {
      /* Dynamic table size update. */
      uint32_t new_size;
      if (hpack_get_int(in, in_len, &pos, 5, &new_size) != 0) {
        return -1;
      }
      if (chttp_h2_hpack_decoder_table_size_update(h, new_size) != 0) {
        return -1;
      }
    } else {
      /* Literal without indexing / never indexed. */
      uint32_t index;
      const char *name = NULL;
      size_t name_len = 0;
      const char *value = NULL;
      size_t value_len = 0;
      int n_owned = 0;
      int v_owned = 0;
      if (hpack_get_int(in, in_len, &pos, 4, &index) != 0) {
        return -1;
      }
      if (index == 0) {
        if (hpack_get_string(h, in, in_len, &pos, &name, &name_len, &n_owned) != 0) {
          return -1;
        }
        if (hpack_get_string(h, in, in_len, &pos, &value, &value_len, &v_owned) != 0) {
          if (n_owned) free((void *)name);
          return -1;
        }
      } else {
        if (table_get(h, index, &name, &name_len, &value, &value_len) != 0) {
          return -1;
        }
        if (hpack_get_string(h, in, in_len, &pos, &value, &value_len, &v_owned) != 0) {
          return -1;
        }
      }
      if (!header_list_too_large &&
          hpack_add_header_list_size(&list_bytes, name_len, value_len, max_header_list_size) != 0)
        header_list_too_large = 1;
      if (!header_list_too_large && cb && cb(user_data, name, name_len, value, value_len) != 0) {
        if (n_owned) free((void *)name);
        if (v_owned) free((void *)value);
        return -3;
      }
      if (n_owned) free((void *)name);
      if (v_owned) free((void *)value);
    }
  }
  *consumed = pos;
  return header_list_too_large ? -2 : 0;
}
