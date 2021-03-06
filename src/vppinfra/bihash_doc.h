/*
 * Copyright (c) 2014 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#error do not #include this file!

/** \file

    Bounded-index extensible hashing. The basic algorithm performs
    thread-safe constant-time lookups in the face of a rational number
    of hash collisions. The computed hash code h(k) must have
    reasonable statistics with respect to the key space. It won't do
    to have h(k) = 0 or 1, for all values of k.

    Each bucket in the power-of-two bucket array contains the index
    (in a private vppinfra memory heap) of the "backing store" for the
    bucket, as well as a size field. The size field (log2_pages)
    corresponds to 1, 2, 4, ... contiguous "pages" containing the
    (key,value) pairs in the bucket.

    When a single page fills, we allocate two contiguous pages.  We
    recompute h(k) for each (key,value) pair, using an additional bit
    to deal the (key, value) pairs into the "top" and "bottom" pages.

    At lookup time, we compute h(k), using lg(bucket-array-size) to
    pick the bucket. We read the bucket to find the base of the
    backing pages.  We use an additional log2_pages' worth of bits
    from h(k) to compute the offset of the page which will contain the
    (key,value) pair we're trying to find.
*/

/** template key/value backing page structure */
typedef struct clib_bihash_value
{
  union
  {

    clib_bihash_kv kvp[BIHASH_KVP_PER_PAGE]; /**< the actual key/value pairs */
    clib_bihash_value *next_free;  /**< used when a KVP page (or block thereof) is on a freelist */
  };
} clib_bihash_value_t
/** bihash bucket structure */
  typedef struct
{
  union
  {
    struct
    {
      u32 offset;  /**< backing page offset in the clib memory heap */
      u8 pad[3];   /**< log2 (size of the packing page block) */
      u8 log2_pages;
    };
    u64 as_u64;
  };
} clib_bihash_bucket_t;

/** A bounded index extensible hash table */
typedef struct
{
  clib_bihash_bucket_t *buckets;  /**< Hash bucket vector, power-of-two in size */
  volatile u32 *writer_lock;  /**< Writer lock, in its own cache line */
    BVT (clib_bihash_value) ** working_copies;
					    /**< Working copies (various sizes), to avoid locking against readers */
  clib_bihash_bucket_t saved_bucket; /**< Saved bucket pointer */
  u32 nbuckets;			     /**< Number of hash buckets */
  u32 log2_nbuckets;		     /**< lg(nbuckets) */
  u8 *name;			     /**< hash table name */
    BVT (clib_bihash_value) ** freelists;
				      /**< power of two freelist vector */
  uword alloc_arena;		      /**< memory allocation arena  */
  uword alloc_arena_next;	      /**< first available mem chunk */
  uword alloc_arena_size;	      /**< size of the arena */
  uword alloc_arena_mapped;	      /**< size of mapped memory in the arena */
} clib_bihash_t;

/** Get pointer to value page given its clib mheap offset */
static inline void *clib_bihash_get_value (clib_bihash * h, uword offset);

/** Get clib mheap offset given a pointer */
static inline uword clib_bihash_get_offset (clib_bihash * h, void *v);

/**
 * initialize a bounded index extensible hash table
 *
 * @param h - the bi-hash table to initialize
 * @param name - name of the hash table
 * @param nbuckets - the number of buckets, will be rounded up to
 * a power of two
 * @param memory_size - clib mheap size, in bytes
 */
void clib_bihash_init
  (clib_bihash * h, char *name, u32 nbuckets, uword memory_size);

/**
 * initialize a bounded index extensible hash table with arguments passed as
 * a struct
 *
 * @param a - initialization parameters
 *   h - the bi-hash table to initialize;
 *   name - name of the hash table
 *   nbuckets - the number of buckets, will be rounded up to a power of two
 *   memory_size - clib mheap size, in bytes
 *   format_function_t - format function for the bihash kv pairs
 *   instantiate_immediately - allocate memory right away
 *   dont_add_to_all_bihash_list - dont mention in 'show bihash'
 */
void BV (clib_bihash_init2) (BVT (clib_bihash_init2_args) * a);

/**
 * Set the formating function for the bihash
 *
 * @param h - the bi-hash table
 * @param kvp_fmt_fn - the format function
 */
void BV (clib_bihash_set_kvp_format_fn) (BVT (clib_bihash) * h,
					 format_function_t *kvp_fmt_fn);

/**
 * Destroy a bounded index extensible hash table
 *
 * @param h - the bi-hash table to free
 */
void clib_bihash_free (clib_bihash *h);

/**
 * Add or delete a (key,value) pair from a bi-hash table
 *
 * @param h - the bi-hash table to search
 * @param add_v - the (key,value) pair to add
 * @param is_add - add=1 (BIHASH_ADD), delete=0 (BIHASH_DEL)
 * @returns 0 on success, < 0 on error
 * @note This function will replace an existing (key,value) pair if the
 * new key matches an existing key
 */
int clib_bihash_add_del (clib_bihash * h, clib_bihash_kv * add_v, int is_add);

/**
 * Add or delete a (key,value) pair from a bi-hash table, using a pre-computed
 * hash
 *
 * @param h - the bi-hash table to search
 * @param add_v - the (key,value) pair to add
 * @param hash - the precomputed hash of the key
 * @param is_add - add=1 (BIHASH_ADD), delete=0 (BIHASH_DEL)
 * @returns 0 on success, < 0 on error
 * @note This function will replace an existing (key,value) pair if the
 * new key matches an existing key
 */
int BV (clib_bihash_add_del_with_hash) (BVT (clib_bihash) * h,
					BVT (clib_bihash_kv) * add_v, u64 hash,
					int is_add);

/**
 * Add a (key,value) pair to a bi-hash table, and tries to free stale entries
 * on collisions with passed filter.
 *
 * @param h - the bi-hash table to search
 * @param add_v - the (key,value) pair to add
 * @param is_stale_cb - callback receiving a kv pair, returning 1 if the kv is
 * stale and can be overwriten. This will be called on adding a kv in a full
 * page before trying to split & rehash its bucket.
 * @param arg - opaque arguement passed to is_stale_cb
 * @returns 0 on success, < 0 on error
 * @note This function will replace an existing (key,value) pair if the
 * new key matches an existing key
 */
int BV (clib_bihash_add_or_overwrite_stale) (
  BVT (clib_bihash) * h, BVT (clib_bihash_kv) * add_v,
  int (*is_stale_cb) (BVT (clib_bihash_kv) *, void *), void *arg);

/**
 * Add a (key,value) pair to a bi-hash table, calling a callback on overwrite
 * with the bucket lock held.
 *
 * @param h - the bi-hash table to search
 * @param add_v - the (key,value) pair to add
 * @param overwrite_cb - callback called when overwriting a key, allowing
 * you to cleanup the value with the bucket lock held.
 * @param arg - opaque arguement passed to overwrite_cb
 * @returns 0 on success, < 0 on error
 * @note This function will replace an existing (key,value) pair if the
 * new key matches an existing key
 */
int BV (clib_bihash_add_with_overwrite_cb) (
  BVT (clib_bihash) * h, BVT (clib_bihash_kv) * add_v,
  void (*overwrite_cb) (BVT (clib_bihash_kv) *, void *), void *arg);

/**
 * Tells if the bihash was initialised (i.e. mem allocated by first add)
 *
 * @param h - the bi-hash table to search
 */
int BV (clib_bihash_is_initialised) (const BVT (clib_bihash) * h);

/**
 * Search a bi-hash table, use supplied hash code
 *
 * @param h - the bi-hash table to search
 * @param hash - the hash code
 * @param in_out_kv - (key,value) pair containing the search key
 * @returns 0 on success (with in_out_kv set), < 0 on error
 */
int clib_bihash_search_inline_with_hash (clib_bihash *h, u64 hash,
					 clib_bihash_kv *in_out_kv);

/**
 * Search a bi-hash table
 *
 * @param h - the bi-hash table to search
 * @param in_out_kv - (key,value) pair containing the search key
 * @returns 0 on success (with in_out_kv set), < 0 on error
 */
int clib_bihash_search_inline (clib_bihash *h, clib_bihash_kv *in_out_kv);

/**
 * Prefetch a bi-hash bucket given a hash code
 *
 * @param h - the bi-hash table to search
 * @param hash - the hash code
 * @note see also clib_bihash_hash to compute the code
 */
void clib_bihash_prefetch_bucket (clib_bihash * h, u64 hash);

/**
 * Prefetch bi-hash (key,value) data given a hash code
 *
 * @param h - the bi-hash table to search
 * @param hash - the hash code
 * @note assumes that the bucket has been prefetched, see
 * clib_bihash_prefetch_bucket
 */
void clib_bihash_prefetch_data (clib_bihash * h, u64 hash);

/**
 * Search a bi-hash table
 *
 * @param h - the bi-hash table to search
 * @param search_key - (key,value) pair containing the search key
 * @param valuep - (key,value) set to search result
 * @returns 0 on success (with valuep set), < 0 on error
 * @note used in situations where key modification is not desired
 */
int clib_bihash_search_inline_2
  (clib_bihash * h, clib_bihash_kv * search_key, clib_bihash_kv * valuep);

/**
 * Calback function for walking a bihash table
 *
 * @param kv - KV pair visited
 * @param ctx - Context passed to the walk
 * @return BIHASH_WALK_CONTINUE to continue BIHASH_WALK_STOP to stop
 */
typedef int (*clib_bihash_foreach_key_value_pair_cb) (clib_bihash_kv * kv,
						      void *ctx);

/**
 * Visit active (key,value) pairs in a bi-hash table
 *
 * @param h - the bi-hash table to search
 * @param callback - function to call with each active (key,value) pair
 * @param arg - arbitrary second argument passed to the callback function
 * First argument is the (key,value) pair to visit
 */
void clib_bihash_foreach_key_value_pair (clib_bihash * h,
					 clib_bihash_foreach_key_value_pair_cb
					 * callback, void *arg);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
