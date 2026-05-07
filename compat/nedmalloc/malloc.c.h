/* -------------------------- System allocation -------------------------- */

/* Get memory from system using MORECORE or MMAP */
static void* sys_alloc(mstate m, size_t nb) {
  char* tbase = CMFAIL;
  size_t tsize = 0;
  flag_t mmap_flag = 0;

  ensure_initialization();

  /* Directly map large chunks */
  if (use_mmap(m) && nb >= mparams.mmap_threshold) {
    void* mem = mmap_alloc(m, nb);
    if (mem != 0)
      return mem;
  }

  /*
    Try getting memory in any of three ways (in most-preferred to
    least-preferred order):
    1. A call to MORECORE that can normally contiguously extend memory.
       (disabled if not MORECORE_CONTIGUOUS or not HAVE_MORECORE or
       main space is mmapped or a previous contiguous call failed)
    2. A call to MMAP new space (disabled if not HAVE_MMAP).
       Note that under the default settings, if MORECORE is unable to
       fulfill a request, and HAVE_MMAP is true, then mmap is
       used as a noncontiguous system allocator. This is a useful backup
       strategy for systems with holes in address spaces -- in this case
       sbrk cannot contiguously expand the heap, but mmap may be able to
       find space.
    3. A call to MORECORE that cannot usually contiguously extend memory.
       (disabled if not HAVE_MORECORE)

   In all cases, we need to request enough bytes from system to ensure
   we can malloc nb bytes upon success, so pad with enough space for
   top_foot, plus alignment-pad to make sure we don't lose bytes if
   not on boundary, and round this up to a granularity unit.
  */

  if (MORECORE_CONTIGUOUS && !use_noncontiguous(m)) {
    char* br = CMFAIL;
    msegmentptr ss = (m->top == 0)? 0 : segment_holding(m, (char*)m->top);
    size_t asize = 0;
    ACQUIRE_MALLOC_GLOBAL_LOCK();

    if (ss == 0) {  /* First time through or recovery */
      char* base = (char*)CALL_MORECORE(0);
      if (base != CMFAIL) {
	asize = granularity_align(nb + SYS_ALLOC_PADDING);
	/* Adjust to end on a page boundary */
	if (!is_page_aligned(base))
	  asize += (page_align((size_t)base) - (size_t)base);
	/* Can't call MORECORE if size is negative when treated as signed */
	if (asize < HALF_MAX_SIZE_T &&
	    (br = (char*)(CALL_MORECORE(asize))) == base) {
	  tbase = base;
	  tsize = asize;
	}
      }
    }
    else {
      /* Subtract out existing available top space from MORECORE request. */
      asize = granularity_align(nb - m->topsize + SYS_ALLOC_PADDING);
      /* Use mem here only if it did continuously extend old space */
      if (asize < HALF_MAX_SIZE_T &&
	  (br = (char*)(CALL_MORECORE(asize))) == ss->base+ss->size) {
	tbase = br;
	tsize = asize;
      }
    }

    if (tbase == CMFAIL) {    /* Cope with partial failure */
      if (br != CMFAIL) {    /* Try to use/extend the space we did get */
	if (asize < HALF_MAX_SIZE_T &&
	    asize < nb + SYS_ALLOC_PADDING) {
	  size_t esize = granularity_align(nb + SYS_ALLOC_PADDING - asize);
	  if (esize < HALF_MAX_SIZE_T) {
	    char* end = (char*)CALL_MORECORE(esize);
	    if (end != CMFAIL)
	      asize += esize;
	    else {            /* Can't use; try to release */
	      (void) CALL_MORECORE(-asize);
	      br = CMFAIL;
	    }
	  }
	}
      }
      if (br != CMFAIL) {    /* Use the space we did get */
	tbase = br;
	tsize = asize;
      }
      else
	disable_contiguous(m); /* Don't try contiguous path in the future */
    }

    RELEASE_MALLOC_GLOBAL_LOCK();
  }

  if (HAVE_MMAP && tbase == CMFAIL) {  /* Try MMAP */
    size_t rsize = granularity_align(nb + SYS_ALLOC_PADDING);
    if (rsize > nb) { /* Fail if wraps around zero */
      char* mp = (char*)(CALL_MMAP(rsize));
      if (mp != CMFAIL) {
	tbase = mp;
	tsize = rsize;
	mmap_flag = IS_MMAPPED_BIT;
      }
    }
  }

  if (HAVE_MORECORE && tbase == CMFAIL) { /* Try noncontiguous MORECORE */
    size_t asize = granularity_align(nb + SYS_ALLOC_PADDING);
    if (asize < HALF_MAX_SIZE_T) {
      char* br = CMFAIL;
      char* end = CMFAIL;
      ACQUIRE_MALLOC_GLOBAL_LOCK();
      br = (char*)(CALL_MORECORE(asize));
      end = (char*)(CALL_MORECORE(0));
      RELEASE_MALLOC_GLOBAL_LOCK();
      if (br != CMFAIL && end != CMFAIL && br < end) {
	size_t ssize = end - br;
	if (ssize > nb + TOP_FOOT_SIZE) {
	  tbase = br;
	  tsize = ssize;
	}
      }
    }
  }

  if (tbase != CMFAIL) {

    if ((m->footprint += tsize) > m->max_footprint)
      m->max_footprint = m->footprint;

    if (!is_initialized(m)) { /* first-time initialization */
      m->seg.base = m->least_addr = tbase;
      m->seg.size = tsize;
      m->seg.sflags = mmap_flag;
      m->magic = mparams.magic;
      m->release_checks = MAX_RELEASE_CHECK_RATE;
      init_bins(m);
#if !ONLY_MSPACES
      if (is_global(m))
	init_top(m, (mchunkptr)tbase, tsize - TOP_FOOT_SIZE);
      else
#endif
      {
	/* Offset top by embedded malloc_state */
	mchunkptr mn = next_chunk(mem2chunk(m));
	init_top(m, mn, (size_t)((tbase + tsize) - (char*)mn) -TOP_FOOT_SIZE);
      }
    }

    else {
      /* Try to merge with an existing segment */
      msegmentptr sp = &m->seg;
      /* Only consider most recent segment if traversal suppressed */
      while (sp != 0 && tbase != sp->base + sp->size)
	sp = (NO_SEGMENT_TRAVERSAL) ? 0 : sp->next;
      if (sp != 0 &&
	  !is_extern_segment(sp) &&
	  (sp->sflags & IS_MMAPPED_BIT) == mmap_flag &&
	  segment_holds(sp, m->top)) { /* append */
	sp->size += tsize;
	init_top(m, m->top, m->topsize + tsize);
      }
      else {
	if (tbase < m->least_addr)
	  m->least_addr = tbase;
	sp = &m->seg;
	while (sp != 0 && sp->base != tbase + tsize)
	  sp = (NO_SEGMENT_TRAVERSAL) ? 0 : sp->next;
	if (sp != 0 &&
	    !is_extern_segment(sp) &&
	    (sp->sflags & IS_MMAPPED_BIT) == mmap_flag) {
	  char* oldbase = sp->base;
	  sp->base = tbase;
	  sp->size += tsize;
	  return prepend_alloc(m, tbase, oldbase, nb);
	}
	else
	  add_segment(m, tbase, tsize, mmap_flag);
      }
    }

    if (nb < m->topsize) { /* Allocate from new or extended top space */
      size_t rsize = m->topsize -= nb;
      mchunkptr p = m->top;
      mchunkptr r = m->top = chunk_plus_offset(p, nb);
      r->head = rsize | PINUSE_BIT;
      set_size_and_pinuse_of_inuse_chunk(m, p, nb);
      check_top_chunk(m, m->top);
      check_malloced_chunk(m, chunk2mem(p), nb);
      return chunk2mem(p);
    }
  }

  MALLOC_FAILURE_ACTION;
  return 0;
}

/* -----------------------  system deallocation -------------------------- */

/* Unmap and unlink any mmapped segments that don't contain used chunks */
static size_t release_unused_segments(mstate m) {
  size_t released = 0;
  int nsegs = 0;
  msegmentptr pred = &m->seg;
  msegmentptr sp = pred->next;
  while (sp != 0) {
    char* base = sp->base;
    size_t size = sp->size;
    msegmentptr next = sp->next;
    ++nsegs;
    if (is_mmapped_segment(sp) && !is_extern_segment(sp)) {
      mchunkptr p = align_as_chunk(base);
      size_t psize = chunksize(p);
      /* Can unmap if first chunk holds entire segment and not pinned */
      if (!cinuse(p) && (char*)p + psize >= base + size - TOP_FOOT_SIZE) {
	tchunkptr tp = (tchunkptr)p;
	assert(segment_holds(sp, (char*)sp));
	if (p == m->dv) {
	  m->dv = 0;
	  m->dvsize = 0;
	}
	else {
	  unlink_large_chunk(m, tp);
	}
	if (CALL_MUNMAP(base, size) == 0) {
	  released += size;
	  m->footprint -= size;
	  /* unlink obsoleted record */
	  sp = pred;
	  sp->next = next;
	}
	else { /* back out if cannot unmap */
	  insert_large_chunk(m, tp, psize);
	}
      }
    }
    if (NO_SEGMENT_TRAVERSAL) /* scan only first segment */
      break;
    pred = sp;
    sp = next;
  }
  /* Reset check counter */
  m->release_checks = ((nsegs > MAX_RELEASE_CHECK_RATE)?
		       nsegs : MAX_RELEASE_CHECK_RATE);
  return released;
}

static int sys_trim(mstate m, size_t pad) {
  size_t released = 0;
  ensure_initialization();
  if (pad < MAX_REQUEST && is_initialized(m)) {
    pad += TOP_FOOT_SIZE; /* ensure enough room for segment overhead */

    if (m->topsize > pad) {
      /* Shrink top space in granularity-size units, keeping at least one */
      size_t unit = mparams.granularity;
      size_t extra = ((m->topsize - pad + (unit - SIZE_T_ONE)) / unit -
		      SIZE_T_ONE) * unit;
      msegmentptr sp = segment_holding(m, (char*)m->top);

      if (!is_extern_segment(sp)) {
	if (is_mmapped_segment(sp)) {
	  if (HAVE_MMAP &&
	      sp->size >= extra &&
	      !has_segment_link(m, sp)) { /* can't shrink if pinned */
	    size_t newsize = sp->size - extra;
	    /* Prefer mremap, fall back to munmap */
	    if ((CALL_MREMAP(sp->base, sp->size, newsize, 0) != MFAIL) ||
		(CALL_MUNMAP(sp->base + newsize, extra) == 0)) {
	      released = extra;
	    }
	  }
	}
	else if (HAVE_MORECORE) {
	  if (extra >= HALF_MAX_SIZE_T) /* Avoid wrapping negative */
	    extra = (HALF_MAX_SIZE_T) + SIZE_T_ONE - unit;
	  ACQUIRE_MALLOC_GLOBAL_LOCK();
	  {
	    /* Make sure end of memory is where we last set it. */
	    char* old_br = (char*)(CALL_MORECORE(0));
	    if (old_br == sp->base + sp->size) {
	      char* rel_br = (char*)(CALL_MORECORE(-extra));
	      char* new_br = (char*)(CALL_MORECORE(0));
	      if (rel_br != CMFAIL && new_br < old_br)
		released = old_br - new_br;
	    }
	  }
	  RELEASE_MALLOC_GLOBAL_LOCK();
	}
      }

      if (released != 0) {
	sp->size -= released;
	m->footprint -= released;
	init_top(m, m->top, m->topsize - released);
	check_top_chunk(m, m->top);
      }
    }

    /* Unmap any unused mmapped segments */
    if (HAVE_MMAP)
      released += release_unused_segments(m);

    /* On failure, disable autotrim to avoid repeated failed future calls */
    if (released == 0 && m->topsize > m->trim_check)
      m->trim_check = MAX_SIZE_T;
  }

  return (released != 0)? 1 : 0;
}


/* ---------------------------- malloc support --------------------------- */

/* allocate a large request from the best fitting chunk in a treebin */
static void* tmalloc_large(mstate m, size_t nb) {
  tchunkptr v = 0;
  size_t rsize = -nb; /* Unsigned negation */
  tchunkptr t;
  bindex_t idx;
  compute_tree_index(nb, idx);
  if ((t = *treebin_at(m, idx)) != 0) {
    /* Traverse tree for this bin looking for node with size == nb */
    size_t sizebits = nb << leftshift_for_tree_index(idx);
    tchunkptr rst = 0;  /* The deepest untaken right subtree */
    for (;;) {
      tchunkptr rt;
      size_t trem = chunksize(t) - nb;
      if (trem < rsize) {
	v = t;
	if ((rsize = trem) == 0)
	  break;
      }
      rt = t->child[1];
      t = t->child[(sizebits >> (SIZE_T_BITSIZE-SIZE_T_ONE)) & 1];
      if (rt != 0 && rt != t)
	rst = rt;
      if (t == 0) {
	t = rst; /* set t to least subtree holding sizes > nb */
	break;
      }
      sizebits <<= 1;
    }
  }
  if (t == 0 && v == 0) { /* set t to root of next non-empty treebin */
    binmap_t leftbits = left_bits(idx2bit(idx)) & m->treemap;
    if (leftbits != 0) {
      bindex_t i;
      binmap_t leastbit = least_bit(leftbits);
      compute_bit2idx(leastbit, i);
      t = *treebin_at(m, i);
    }
  }

  while (t != 0) { /* find smallest of tree or subtree */
    size_t trem = chunksize(t) - nb;
    if (trem < rsize) {
      rsize = trem;
      v = t;
    }
    t = leftmost_child(t);
  }

  /*  If dv is a better fit, return 0 so malloc will use it */
  if (v != 0 && rsize < (size_t)(m->dvsize - nb)) {
    if (RTCHECK(ok_address(m, v))) { /* split */
      mchunkptr r = chunk_plus_offset(v, nb);
      assert(chunksize(v) == rsize + nb);
      if (RTCHECK(ok_next(v, r))) {
	unlink_large_chunk(m, v);
	if (rsize < MIN_CHUNK_SIZE)
	  set_inuse_and_pinuse(m, v, (rsize + nb));
	else {
	  set_size_and_pinuse_of_inuse_chunk(m, v, nb);
	  set_size_and_pinuse_of_free_chunk(r, rsize);
	  insert_chunk(m, r, rsize);
	}
	return chunk2mem(v);
      }
    }
    CORRUPTION_ERROR_ACTION(m);
  }
  return 0;
}

/* allocate a small request from the best fitting chunk in a treebin */
static void* tmalloc_small(mstate m, size_t nb) {
  tchunkptr t, v;
  size_t rsize;
  bindex_t i;
  binmap_t leastbit = least_bit(m->treemap);
  compute_bit2idx(leastbit, i);
  v = t = *treebin_at(m, i);
  rsize = chunksize(t) - nb;

  while ((t = leftmost_child(t)) != 0) {
    size_t trem = chunksize(t) - nb;
    if (trem < rsize) {
      rsize = trem;
      v = t;
    }
  }

  if (RTCHECK(ok_address(m, v))) {
    mchunkptr r = chunk_plus_offset(v, nb);
    assert(chunksize(v) == rsize + nb);
    if (RTCHECK(ok_next(v, r))) {
      unlink_large_chunk(m, v);
      if (rsize < MIN_CHUNK_SIZE)
	set_inuse_and_pinuse(m, v, (rsize + nb));
      else {
	set_size_and_pinuse_of_inuse_chunk(m, v, nb);
	set_size_and_pinuse_of_free_chunk(r, rsize);
	replace_dv(m, r, rsize);
      }
      return chunk2mem(v);
    }
  }

  CORRUPTION_ERROR_ACTION(m);
  return 0;
}

/* --------------------------- realloc support --------------------------- */

static void* internal_realloc(mstate m, void* oldmem, size_t bytes) {
  if (bytes >= MAX_REQUEST) {
    MALLOC_FAILURE_ACTION;
    return 0;
  }
  if (!PREACTION(m)) {
    mchunkptr oldp = mem2chunk(oldmem);
    size_t oldsize = chunksize(oldp);
    mchunkptr next = chunk_plus_offset(oldp, oldsize);
    mchunkptr newp = 0;
    void* extra = 0;

    /* Try to either shrink or extend into top. Else malloc-copy-free */

    if (RTCHECK(ok_address(m, oldp) && ok_cinuse(oldp) &&
		ok_next(oldp, next) && ok_pinuse(next))) {
      size_t nb = request2size(bytes);
      if (is_mmapped(oldp))
	newp = mmap_resize(m, oldp, nb);
      else if (oldsize >= nb) { /* already big enough */
	size_t rsize = oldsize - nb;
	newp = oldp;
	if (rsize >= MIN_CHUNK_SIZE) {
	  mchunkptr remainder = chunk_plus_offset(newp, nb);
	  set_inuse(m, newp, nb);
	  set_inuse(m, remainder, rsize);
	  extra = chunk2mem(remainder);
	}
      }
      else if (next == m->top && oldsize + m->topsize > nb) {
	/* Expand into top */
	size_t newsize = oldsize + m->topsize;
	size_t newtopsize = newsize - nb;
	mchunkptr newtop = chunk_plus_offset(oldp, nb);
	set_inuse(m, oldp, nb);
	newtop->head = newtopsize |PINUSE_BIT;
	m->top = newtop;
	m->topsize = newtopsize;
	newp = oldp;
      }
    }
    else {
      USAGE_ERROR_ACTION(m, oldmem);
      POSTACTION(m);
      return 0;
    }

    POSTACTION(m);

    if (newp != 0) {
      if (extra != 0) {
	internal_free(m, extra);
      }
      check_inuse_chunk(m, newp);
      return chunk2mem(newp);
    }
    else {
      void* newmem = internal_malloc(m, bytes);
      if (newmem != 0) {
	size_t oc = oldsize - overhead_for(oldp);
	memcpy(newmem, oldmem, (oc < bytes)? oc : bytes);
	internal_free(m, oldmem);
      }
      return newmem;
    }
  }
  return 0;
}

/* --------------------------- memalign support -------------------------- */

static void* internal_memalign(mstate m, size_t alignment, size_t bytes) {
  if (alignment <= MALLOC_ALIGNMENT)    /* Can just use malloc */
    return internal_malloc(m, bytes);
  if (alignment <  MIN_CHUNK_SIZE) /* must be at least a minimum chunk size */
    alignment = MIN_CHUNK_SIZE;
  if ((alignment & (alignment-SIZE_T_ONE)) != 0) {/* Ensure a power of 2 */
    size_t a = MALLOC_ALIGNMENT << 1;
    while (a < alignment) a <<= 1;
    alignment = a;
  }

  if (bytes >= MAX_REQUEST - alignment) {
    if (m != 0)  { /* Test isn't needed but avoids compiler warning */
      MALLOC_FAILURE_ACTION;
    }
  }
  else {
    size_t nb = request2size(bytes);
    size_t req = nb + alignment + MIN_CHUNK_SIZE - CHUNK_OVERHEAD;
    char* mem = (char*)internal_malloc(m, req);
    if (mem != 0) {
      void* leader = 0;
      void* trailer = 0;
      mchunkptr p = mem2chunk(mem);

      if (PREACTION(m)) return 0;
      if ((((size_t)(mem)) % alignment) != 0) { /* misaligned */
	/*
	  Find an aligned spot inside chunk.  Since we need to give
	  back leading space in a chunk of at least MIN_CHUNK_SIZE, if
	  the first calculation places us at a spot with less than
	  MIN_CHUNK_SIZE leader, we can move to the next aligned spot.
	  We've allocated enough total room so that this is always
	  possible.
	*/
	char* br = (char*)mem2chunk((size_t)(((size_t)(mem +
						       alignment -
						       SIZE_T_ONE)) &
					     -alignment));
	char* pos = ((size_t)(br - (char*)(p)) >= MIN_CHUNK_SIZE)?
	  br : br+alignment;
	mchunkptr newp = (mchunkptr)pos;
	size_t leadsize = pos - (char*)(p);
	size_t newsize = chunksize(p) - leadsize;

	if (is_mmapped(p)) { /* For mmapped chunks, just adjust offset */
	  newp->prev_foot = p->prev_foot + leadsize;
	  newp->head = (newsize|CINUSE_BIT);
	}
	else { /* Otherwise, give back leader, use the rest */
	  set_inuse(m, newp, newsize);
	  set_inuse(m, p, leadsize);
	  leader = chunk2mem(p);
	}
	p = newp;
      }

      /* Give back spare room at the end */
      if (!is_mmapped(p)) {
	size_t size = chunksize(p);
	if (size > nb + MIN_CHUNK_SIZE) {
	  size_t remainder_size = size - nb;
	  mchunkptr remainder = chunk_plus_offset(p, nb);
	  set_inuse(m, p, nb);
	  set_inuse(m, remainder, remainder_size);
	  trailer = chunk2mem(remainder);
	}
      }

      assert (chunksize(p) >= nb);
      assert((((size_t)(chunk2mem(p))) % alignment) == 0);
      check_inuse_chunk(m, p);
      POSTACTION(m);
      if (leader != 0) {
	internal_free(m, leader);
      }
      if (trailer != 0) {
	internal_free(m, trailer);
      }
      return chunk2mem(p);
    }
  }
  return 0;
}

/* ------------------------ comalloc/coalloc support --------------------- */

static void** ialloc(mstate m,
		     size_t n_elements,
		     size_t* sizes,
		     int opts,
		     void* chunks[]) {
  /*
    This provides common support for independent_X routines, handling
    all of the combinations that can result.

    The opts arg has:
    bit 0 set if all elements are same size (using sizes[0])
    bit 1 set if elements should be zeroed
  */

  size_t    element_size;   /* chunksize of each element, if all same */
  size_t    contents_size;  /* total size of elements */
  size_t    array_size;     /* request size of pointer array */
  void*     mem;            /* malloced aggregate space */
  mchunkptr p;              /* corresponding chunk */
  size_t    remainder_size; /* remaining bytes while splitting */
  void**    marray;         /* either "chunks" or malloced ptr array */
  mchunkptr array_chunk;    /* chunk for malloced ptr array */
  flag_t    was_enabled;    /* to disable mmap */
  size_t    size;
  size_t    i;

  ensure_initialization();
  /* compute array length, if needed */
  if (chunks != 0) {
    if (n_elements == 0)
      return chunks; /* nothing to do */
    marray = chunks;
    array_size = 0;
  }
  else {
    /* if empty req, must still return chunk representing empty array */
    if (n_elements == 0)
      return (void**)internal_malloc(m, 0);
    marray = 0;
    array_size = request2size(n_elements * (sizeof(void*)));
  }

  /* compute total element size */
  if (opts & 0x1) { /* all-same-size */
    element_size = request2size(*sizes);
    contents_size = n_elements * element_size;
  }
  else { /* add up all the sizes */
    element_size = 0;
    contents_size = 0;
    for (i = 0; i != n_elements; ++i)
      contents_size += request2size(sizes[i]);
  }

  size = contents_size + array_size;

  /*
     Allocate the aggregate chunk.  First disable direct-mmapping so
     malloc won't use it, since we would not be able to later
     free/realloc space internal to a segregated mmap region.
  */
  was_enabled = use_mmap(m);
  disable_mmap(m);
  mem = internal_malloc(m, size - CHUNK_OVERHEAD);
  if (was_enabled)
    enable_mmap(m);
  if (mem == 0)
    return 0;

  if (PREACTION(m)) return 0;
  p = mem2chunk(mem);
  remainder_size = chunksize(p);

  assert(!is_mmapped(p));

  if (opts & 0x2) {       /* optionally clear the elements */
    memset((size_t*)mem, 0, remainder_size - SIZE_T_SIZE - array_size);
  }

  /* If not provided, allocate the pointer array as final part of chunk */
  if (marray == 0) {
    size_t  array_chunk_size;
    array_chunk = chunk_plus_offset(p, contents_size);
    array_chunk_size = remainder_size - contents_size;
    marray = (void**) (chunk2mem(array_chunk));
    set_size_and_pinuse_of_inuse_chunk(m, array_chunk, array_chunk_size);
    remainder_size = contents_size;
  }

  /* split out elements */
  for (i = 0; ; ++i) {
    marray[i] = chunk2mem(p);
    if (i != n_elements-1) {
      if (element_size != 0)
	size = element_size;
      else
	size = request2size(sizes[i]);
      remainder_size -= size;
      set_size_and_pinuse_of_inuse_chunk(m, p, size);
      p = chunk_plus_offset(p, size);
    }
    else { /* the final element absorbs any overallocation slop */
      set_size_and_pinuse_of_inuse_chunk(m, p, remainder_size);
      break;
    }
  }

#if DEBUG
  if (marray != chunks) {
    /* final element must have exactly exhausted chunk */
    if (element_size != 0) {
      assert(remainder_size == element_size);
    }
    else {
      assert(remainder_size == request2size(sizes[i]));
    }
    check_inuse_chunk(m, mem2chunk(marray));
  }
  for (i = 0; i != n_elements; ++i)
    check_inuse_chunk(m, mem2chunk(marray[i]));

#endif /* DEBUG */

  POSTACTION(m);
  return marray;
}


/* -------------------------- public routines ---------------------------- */

#if !ONLY_MSPACES

void* dlmalloc(size_t bytes) {
  /*
     Basic algorithm:
     If a small request (< 256 bytes minus per-chunk overhead):
       1. If one exists, use a remainderless chunk in associated smallbin.
	  (Remainderless means that there are too few excess bytes to
	  represent as a chunk.)
       2. If it is big enough, use the dv chunk, which is normally the
	  chunk adjacent to the one used for the most recent small request.
       3. If one exists, split the smallest available chunk in a bin,
	  saving remainder in dv.
       4. If it is big enough, use the top chunk.
       5. If available, get memory from system and use it
     Otherwise, for a large request:
       1. Find the smallest available binned chunk that fits, and use it
	  if it is better fitting than dv chunk, splitting if necessary.
       2. If better fitting than any binned chunk, use the dv chunk.
       3. If it is big enough, use the top chunk.
       4. If request size >= mmap threshold, try to directly mmap this chunk.
       5. If available, get memory from system and use it

     The ugly goto's here ensure that postaction occurs along all paths.
  */

#if USE_LOCKS
  ensure_initialization(); /* initialize in sys_alloc if not using locks */
#endif

  if (!PREACTION(gm)) {
    void* mem;
    size_t nb;
    if (bytes <= MAX_SMALL_REQUEST) {
      bindex_t idx;
      binmap_t smallbits;
      nb = (bytes < MIN_REQUEST)? MIN_CHUNK_SIZE : pad_request(bytes);
      idx = small_index(nb);
      smallbits = gm->smallmap >> idx;

      if ((smallbits & 0x3U) != 0) { /* Remainderless fit to a smallbin. */
	mchunkptr b, p;
	idx += ~smallbits & 1;       /* Uses next bin if idx empty */
	b = smallbin_at(gm, idx);
	p = b->fd;
	assert(chunksize(p) == small_index2size(idx));
	unlink_first_small_chunk(gm, b, p, idx);
	set_inuse_and_pinuse(gm, p, small_index2size(idx));
	mem = chunk2mem(p);
	check_malloced_chunk(gm, mem, nb);
	goto postaction;
      }

      else if (nb > gm->dvsize) {
	if (smallbits != 0) { /* Use chunk in next nonempty smallbin */
	  mchunkptr b, p, r;
	  size_t rsize;
	  bindex_t i;
	  binmap_t leftbits = (smallbits << idx) & left_bits(idx2bit(idx));
	  binmap_t leastbit = least_bit(leftbits);
	  compute_bit2idx(leastbit, i);
	  b = smallbin_at(gm, i);
	  p = b->fd;
	  assert(chunksize(p) == small_index2size(i));
	  unlink_first_small_chunk(gm, b, p, i);
	  rsize = small_index2size(i) - nb;
	  /* Fit here cannot be remainderless if 4byte sizes */
	  if (SIZE_T_SIZE != 4 && rsize < MIN_CHUNK_SIZE)
	    set_inuse_and_pinuse(gm, p, small_index2size(i));
	  else {
	    set_size_and_pinuse_of_inuse_chunk(gm, p, nb);
	    r = chunk_plus_offset(p, nb);
	    set_size_and_pinuse_of_free_chunk(r, rsize);
	    replace_dv(gm, r, rsize);
	  }
	  mem = chunk2mem(p);
	  check_malloced_chunk(gm, mem, nb);
	  goto postaction;
	}

	else if (gm->treemap != 0 && (mem = tmalloc_small(gm, nb)) != 0) {
	  check_malloced_chunk(gm, mem, nb);
	  goto postaction;
	}
      }
    }
    else if (bytes >= MAX_REQUEST)
      nb = MAX_SIZE_T; /* Too big to allocate. Force failure (in sys alloc) */
    else {
      nb = pad_request(bytes);
      if (gm->treemap != 0 && (mem = tmalloc_large(gm, nb)) != 0) {
	check_malloced_chunk(gm, mem, nb);
	goto postaction;
      }
    }

    if (nb <= gm->dvsize) {
      size_t rsize = gm->dvsize - nb;
      mchunkptr p = gm->dv;
      if (rsize >= MIN_CHUNK_SIZE) { /* split dv */
	mchunkptr r = gm->dv = chunk_plus_offset(p, nb);
	gm->dvsize = rsize;
	set_size_and_pinuse_of_free_chunk(r, rsize);
	set_size_and_pinuse_of_inuse_chunk(gm, p, nb);
      }
      else { /* exhaust dv */
	size_t dvs = gm->dvsize;
	gm->dvsize = 0;
	gm->dv = 0;
	set_inuse_and_pinuse(gm, p, dvs);
      }
      mem = chunk2mem(p);
      check_malloced_chunk(gm, mem, nb);
      goto postaction;
    }

    else if (nb < gm->topsize) { /* Split top */
      size_t rsize = gm->topsize -= nb;
      mchunkptr p = gm->top;
      mchunkptr r = gm->top = chunk_plus_offset(p, nb);
      r->head = rsize | PINUSE_BIT;
      set_size_and_pinuse_of_inuse_chunk(gm, p, nb);
      mem = chunk2mem(p);
      check_top_chunk(gm, gm->top);
      check_malloced_chunk(gm, mem, nb);
      goto postaction;
    }

    mem = sys_alloc(gm, nb);

  postaction:
    POSTACTION(gm);
    return mem;
  }

  return 0;
}

void dlfree(void* mem) {
  /*
     Consolidate freed chunks with preceding or succeeding bordering
     free chunks, if they exist, and then place in a bin.  Intermixed
     with special cases for top, dv, mmapped chunks, and usage errors.
  */

  if (mem != 0) {
    mchunkptr p  = mem2chunk(mem);
#if FOOTERS
    mstate fm = get_mstate_for(p);
    if (!ok_magic(fm)) {
      USAGE_ERROR_ACTION(fm, p);
      return;
    }
#else /* FOOTERS */
#define fm gm
#endif /* FOOTERS */
    if (!PREACTION(fm)) {
      check_inuse_chunk(fm, p);
      if (RTCHECK(ok_address(fm, p) && ok_cinuse(p))) {
	size_t psize = chunksize(p);
	mchunkptr next = chunk_plus_offset(p, psize);
	if (!pinuse(p)) {
	  size_t prevsize = p->prev_foot;
	  if ((prevsize & IS_MMAPPED_BIT) != 0) {
	    prevsize &= ~IS_MMAPPED_BIT;
	    psize += prevsize + MMAP_FOOT_PAD;
	    if (CALL_MUNMAP((char*)p - prevsize, psize) == 0)
	      fm->footprint -= psize;
	    goto postaction;
	  }
	  else {
	    mchunkptr prev = chunk_minus_offset(p, prevsize);
	    psize += prevsize;
	    p = prev;
	    if (RTCHECK(ok_address(fm, prev))) { /* consolidate backward */
	      if (p != fm->dv) {
		unlink_chunk(fm, p, prevsize);
	      }
	      else if ((next->head & INUSE_BITS) == INUSE_BITS) {
		fm->dvsize = psize;
		set_free_with_pinuse(p, psize, next);
		goto postaction;
	      }
	    }
	    else
	      goto erroraction;
	  }
	}

	if (RTCHECK(ok_next(p, next) && ok_pinuse(next))) {
	  if (!cinuse(next)) {  /* consolidate forward */
	    if (next == fm->top) {
	      size_t tsize = fm->topsize += psize;
	      fm->top = p;
	      p->head = tsize | PINUSE_BIT;
	      if (p == fm->dv) {
		fm->dv = 0;
		fm->dvsize = 0;
	      }
	      if (should_trim(fm, tsize))
		sys_trim(fm, 0);
	      goto postaction;
	    }
	    else if (next == fm->dv) {
	      size_t dsize = fm->dvsize += psize;
	      fm->dv = p;
	      set_size_and_pinuse_of_free_chunk(p, dsize);
	      goto postaction;
	    }
	    else {
	      size_t nsize = chunksize(next);
	      psize += nsize;
	      unlink_chunk(fm, next, nsize);
	      set_size_and_pinuse_of_free_chunk(p, psize);
	      if (p == fm->dv) {
		fm->dvsize = psize;
		goto postaction;
	      }
	    }
	  }
	  else
	    set_free_with_pinuse(p, psize, next);

	  if (is_small(psize)) {
	    insert_small_chunk(fm, p, psize);
	    check_free_chunk(fm, p);
	  }
	  else {
	    tchunkptr tp = (tchunkptr)p;
	    insert_large_chunk(fm, tp, psize);
	    check_free_chunk(fm, p);
	    if (--fm->release_checks == 0)
	      release_unused_segments(fm);
	  }
	  goto postaction;
	}
      }
    erroraction:
      USAGE_ERROR_ACTION(fm, p);
    postaction:
      POSTACTION(fm);
    }
  }
#if !FOOTERS
#undef fm
#endif /* FOOTERS */
}

void* dlcalloc(size_t n_elements, size_t elem_size) {
  void* mem;
  size_t req = 0;
  if (n_elements != 0) {
    req = n_elements * elem_size;
    if (((n_elements | elem_size) & ~(size_t)0xffff) &&
	(req / n_elements != elem_size))
      req = MAX_SIZE_T; /* force downstream failure on overflow */
  }
  mem = dlmalloc(req);
  if (mem != 0 && calloc_must_clear(mem2chunk(mem)))
    memset(mem, 0, req);
  return mem;
}

void* dlrealloc(void* oldmem, size_t bytes) {
  if (oldmem == 0)
    return dlmalloc(bytes);
#ifdef REALLOC_ZERO_BYTES_FREES
  if (bytes == 0) {
    dlfree(oldmem);
    return 0;
  }
#endif /* REALLOC_ZERO_BYTES_FREES */
  else {
#if ! FOOTERS
    mstate m = gm;
#else /* FOOTERS */
    mstate m = get_mstate_for(mem2chunk(oldmem));
    if (!ok_magic(m)) {
      USAGE_ERROR_ACTION(m, oldmem);
      return 0;
    }
#endif /* FOOTERS */
    return internal_realloc(m, oldmem, bytes);
  }
}

void* dlmemalign(size_t alignment, size_t bytes) {
  return internal_memalign(gm, alignment, bytes);
}

void** dlindependent_calloc(size_t n_elements, size_t elem_size,
				 void* chunks[]) {
  size_t sz = elem_size; /* serves as 1-element array */
  return ialloc(gm, n_elements, &sz, 3, chunks);
}

void** dlindependent_comalloc(size_t n_elements, size_t sizes[],
				   void* chunks[]) {
  return ialloc(gm, n_elements, sizes, 0, chunks);
}

void* dlvalloc(size_t bytes) {
  size_t pagesz;
  ensure_initialization();
  pagesz = mparams.page_size;
  return dlmemalign(pagesz, bytes);
}

void* dlpvalloc(size_t bytes) {
  size_t pagesz;
  ensure_initialization();
  pagesz = mparams.page_size;
  return dlmemalign(pagesz, (bytes + pagesz - SIZE_T_ONE) & ~(pagesz - SIZE_T_ONE));
}

int dlmalloc_trim(size_t pad) {
  ensure_initialization();
  int result = 0;
  if (!PREACTION(gm)) {
    result = sys_trim(gm, pad);
    POSTACTION(gm);
  }
  return result;
}

size_t dlmalloc_footprint(void) {
  return gm->footprint;
}

size_t dlmalloc_max_footprint(void) {
  return gm->max_footprint;
}

#if !NO_MALLINFO
struct mallinfo dlmallinfo(void) {
  return internal_mallinfo(gm);
}
#endif /* NO_MALLINFO */

void dlmalloc_stats() {
  internal_malloc_stats(gm);
}

int dlmallopt(int param_number, int value) {
  return change_mparam(param_number, value);
}

#endif /* !ONLY_MSPACES */

size_t dlmalloc_usable_size(void* mem) {
  if (mem != 0) {
    mchunkptr p = mem2chunk(mem);
    if (cinuse(p))
      return chunksize(p) - overhead_for(p);
  }
  return 0;
}

/* ----------------------------- user mspaces ---------------------------- */

#if MSPACES

static mstate init_user_mstate(char* tbase, size_t tsize) {
  size_t msize = pad_request(sizeof(struct malloc_state));
  mchunkptr mn;
  mchunkptr msp = align_as_chunk(tbase);
  mstate m = (mstate)(chunk2mem(msp));
  memset(m, 0, msize);
  (void)INITIAL_LOCK(&m->mutex);
  msp->head = (msize|PINUSE_BIT|CINUSE_BIT);
  m->seg.base = m->least_addr = tbase;
  m->seg.size = m->footprint = m->max_footprint = tsize;
  m->magic = mparams.magic;
  m->release_checks = MAX_RELEASE_CHECK_RATE;
  m->mflags = mparams.default_mflags;
  m->extp = 0;
  m->exts = 0;
  disable_contiguous(m);
  init_bins(m);
  mn = next_chunk(mem2chunk(m));
  init_top(m, mn, (size_t)((tbase + tsize) - (char*)mn) - TOP_FOOT_SIZE);
  check_top_chunk(m, m->top);
  return m;
}

mspace create_mspace(size_t capacity, int locked) {
  mstate m = 0;
  size_t msize;
  ensure_initialization();
  msize = pad_request(sizeof(struct malloc_state));
  if (capacity < (size_t) -(msize + TOP_FOOT_SIZE + mparams.page_size)) {
    size_t rs = ((capacity == 0)? mparams.granularity :
		 (capacity + TOP_FOOT_SIZE + msize));
    size_t tsize = granularity_align(rs);
    char* tbase = (char*)(CALL_MMAP(tsize));
    if (tbase != CMFAIL) {
      m = init_user_mstate(tbase, tsize);
      m->seg.sflags = IS_MMAPPED_BIT;
      set_lock(m, locked);
    }
  }
  return (mspace)m;
}

mspace create_mspace_with_base(void* base, size_t capacity, int locked) {
  mstate m = 0;
  size_t msize;
  ensure_initialization();
  msize = pad_request(sizeof(struct malloc_state));
  if (capacity > msize + TOP_FOOT_SIZE &&
      capacity < (size_t) -(msize + TOP_FOOT_SIZE + mparams.page_size)) {
    m = init_user_mstate((char*)base, capacity);
    m->seg.sflags = EXTERN_BIT;
    set_lock(m, locked);
  }
  return (mspace)m;
}

int mspace_mmap_large_chunks(mspace msp, int enable) {
  int ret = 0;
  mstate ms = (mstate)msp;
  if (!PREACTION(ms)) {
    if (use_mmap(ms))
      ret = 1;
    if (enable)
      enable_mmap(ms);
    else
      disable_mmap(ms);
    POSTACTION(ms);
  }
  return ret;
}

size_t destroy_mspace(mspace msp) {
  size_t freed = 0;
  mstate ms = (mstate)msp;
  if (ok_magic(ms)) {
    msegmentptr sp = &ms->seg;
    while (sp != 0) {
      char* base = sp->base;
      size_t size = sp->size;
      flag_t flag = sp->sflags;
      sp = sp->next;
      if ((flag & IS_MMAPPED_BIT) && !(flag & EXTERN_BIT) &&
	  CALL_MUNMAP(base, size) == 0)
	freed += size;
    }
  }
  else {
    USAGE_ERROR_ACTION(ms,ms);
  }
  return freed;
}

/*
  mspace versions of routines are near-clones of the global
  versions. This is not so nice but better than the alternatives.
*/


void* mspace_malloc(mspace msp, size_t bytes) {
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
  if (!PREACTION(ms)) {
    void* mem;
    size_t nb;
    if (bytes <= MAX_SMALL_REQUEST) {
      bindex_t idx;
      binmap_t smallbits;
      nb = (bytes < MIN_REQUEST)? MIN_CHUNK_SIZE : pad_request(bytes);
      idx = small_index(nb);
      smallbits = ms->smallmap >> idx;

      if ((smallbits & 0x3U) != 0) { /* Remainderless fit to a smallbin. */
	mchunkptr b, p;
	idx += ~smallbits & 1;       /* Uses next bin if idx empty */
	b = smallbin_at(ms, idx);
	p = b->fd;
	assert(chunksize(p) == small_index2size(idx));
	unlink_first_small_chunk(ms, b, p, idx);
	set_inuse_and_pinuse(ms, p, small_index2size(idx));
	mem = chunk2mem(p);
	check_malloced_chunk(ms, mem, nb);
	goto postaction;
      }

      else if (nb > ms->dvsize) {
	if (smallbits != 0) { /* Use chunk in next nonempty smallbin */
	  mchunkptr b, p, r;
	  size_t rsize;
	  bindex_t i;
	  binmap_t leftbits = (smallbits << idx) & left_bits(idx2bit(idx));
	  binmap_t leastbit = least_bit(leftbits);
	  compute_bit2idx(leastbit, i);
	  b = smallbin_at(ms, i);
	  p = b->fd;
	  assert(chunksize(p) == small_index2size(i));
	  unlink_first_small_chunk(ms, b, p, i);
	  rsize = small_index2size(i) - nb;
	  /* Fit here cannot be remainderless if 4byte sizes */
	  if (SIZE_T_SIZE != 4 && rsize < MIN_CHUNK_SIZE)
	    set_inuse_and_pinuse(ms, p, small_index2size(i));
	  else {
	    set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
	    r = chunk_plus_offset(p, nb);
	    set_size_and_pinuse_of_free_chunk(r, rsize);
	    replace_dv(ms, r, rsize);
	  }
	  mem = chunk2mem(p);
	  check_malloced_chunk(ms, mem, nb);
	  goto postaction;
	}

	else if (ms->treemap != 0 && (mem = tmalloc_small(ms, nb)) != 0) {
	  check_malloced_chunk(ms, mem, nb);
	  goto postaction;
	}
      }
    }
    else if (bytes >= MAX_REQUEST)
      nb = MAX_SIZE_T; /* Too big to allocate. Force failure (in sys alloc) */
    else {
      nb = pad_request(bytes);
      if (ms->treemap != 0 && (mem = tmalloc_large(ms, nb)) != 0) {
	check_malloced_chunk(ms, mem, nb);
	goto postaction;
      }
    }

    if (nb <= ms->dvsize) {
      size_t rsize = ms->dvsize - nb;
      mchunkptr p = ms->dv;
      if (rsize >= MIN_CHUNK_SIZE) { /* split dv */
	mchunkptr r = ms->dv = chunk_plus_offset(p, nb);
	ms->dvsize = rsize;
	set_size_and_pinuse_of_free_chunk(r, rsize);
	set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
      }
      else { /* exhaust dv */
	size_t dvs = ms->dvsize;
	ms->dvsize = 0;
	ms->dv = 0;
	set_inuse_and_pinuse(ms, p, dvs);
      }
      mem = chunk2mem(p);
      check_malloced_chunk(ms, mem, nb);
      goto postaction;
    }

    else if (nb < ms->topsize) { /* Split top */
      size_t rsize = ms->topsize -= nb;
      mchunkptr p = ms->top;
      mchunkptr r = ms->top = chunk_plus_offset(p, nb);
      r->head = rsize | PINUSE_BIT;
      set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
      mem = chunk2mem(p);
      check_top_chunk(ms, ms->top);
      check_malloced_chunk(ms, mem, nb);
      goto postaction;
    }

    mem = sys_alloc(ms, nb);

  postaction:
    POSTACTION(ms);
    return mem;
  }

  return 0;
}

void mspace_free(mspace msp, void* mem) {
  if (mem != 0) {
    mchunkptr p  = mem2chunk(mem);
#if FOOTERS
    mstate fm = get_mstate_for(p);
#else /* FOOTERS */
    mstate fm = (mstate)msp;
#endif /* FOOTERS */
    if (!ok_magic(fm)) {
      USAGE_ERROR_ACTION(fm, p);
      return;
    }
    if (!PREACTION(fm)) {
      check_inuse_chunk(fm, p);
      if (RTCHECK(ok_address(fm, p) && ok_cinuse(p))) {
	size_t psize = chunksize(p);
	mchunkptr next = chunk_plus_offset(p, psize);
	if (!pinuse(p)) {
	  size_t prevsize = p->prev_foot;
	  if ((prevsize & IS_MMAPPED_BIT) != 0) {
	    prevsize &= ~IS_MMAPPED_BIT;
	    psize += prevsize + MMAP_FOOT_PAD;
	    if (CALL_MUNMAP((char*)p - prevsize, psize) == 0)
	      fm->footprint -= psize;
	    goto postaction;
	  }
	  else {
	    mchunkptr prev = chunk_minus_offset(p, prevsize);
	    psize += prevsize;
	    p = prev;
	    if (RTCHECK(ok_address(fm, prev))) { /* consolidate backward */
	      if (p != fm->dv) {
		unlink_chunk(fm, p, prevsize);
	      }
	      else if ((next->head & INUSE_BITS) == INUSE_BITS) {
		fm->dvsize = psize;
		set_free_with_pinuse(p, psize, next);
		goto postaction;
	      }
	    }
	    else
	      goto erroraction;
	  }
	}

	if (RTCHECK(ok_next(p, next) && ok_pinuse(next))) {
	  if (!cinuse(next)) {  /* consolidate forward */
	    if (next == fm->top) {
	      size_t tsize = fm->topsize += psize;
	      fm->top = p;
	      p->head = tsize | PINUSE_BIT;
	      if (p == fm->dv) {
		fm->dv = 0;
		fm->dvsize = 0;
	      }
	      if (should_trim(fm, tsize))
		sys_trim(fm, 0);
	      goto postaction;
	    }
	    else if (next == fm->dv) {
	      size_t dsize = fm->dvsize += psize;
	      fm->dv = p;
	      set_size_and_pinuse_of_free_chunk(p, dsize);
	      goto postaction;
	    }
	    else {
	      size_t nsize = chunksize(next);
	      psize += nsize;
	      unlink_chunk(fm, next, nsize);
	      set_size_and_pinuse_of_free_chunk(p, psize);
	      if (p == fm->dv) {
		fm->dvsize = psize;
		goto postaction;
	      }
	    }
	  }
	  else
	    set_free_with_pinuse(p, psize, next);

	  if (is_small(psize)) {
	    insert_small_chunk(fm, p, psize);
	    check_free_chunk(fm, p);
	  }
	  else {
	    tchunkptr tp = (tchunkptr)p;
	    insert_large_chunk(fm, tp, psize);
	    check_free_chunk(fm, p);
	    if (--fm->release_checks == 0)
	      release_unused_segments(fm);
	  }
	  goto postaction;
	}
      }
    erroraction:
      USAGE_ERROR_ACTION(fm, p);
    postaction:
      POSTACTION(fm);
    }
  }
}

void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size) {
  void* mem;
  size_t req = 0;
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
  if (n_elements != 0) {
    req = n_elements * elem_size;
    if (((n_elements | elem_size) & ~(size_t)0xffff) &&
	(req / n_elements != elem_size))
      req = MAX_SIZE_T; /* force downstream failure on overflow */
  }
  mem = internal_malloc(ms, req);
  if (mem != 0 && calloc_must_clear(mem2chunk(mem)))
    memset(mem, 0, req);
  return mem;
}

void* mspace_realloc(mspace msp, void* oldmem, size_t bytes) {
  if (oldmem == 0)
    return mspace_malloc(msp, bytes);
#ifdef REALLOC_ZERO_BYTES_FREES
  if (bytes == 0) {
    mspace_free(msp, oldmem);
    return 0;
  }
#endif /* REALLOC_ZERO_BYTES_FREES */
  else {
#if FOOTERS
    mchunkptr p  = mem2chunk(oldmem);
    mstate ms = get_mstate_for(p);
#else /* FOOTERS */
    mstate ms = (mstate)msp;
#endif /* FOOTERS */
    if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
      return 0;
    }
    return internal_realloc(ms, oldmem, bytes);
  }
}

void* mspace_memalign(mspace msp, size_t alignment, size_t bytes) {
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
  return internal_memalign(ms, alignment, bytes);
}

void** mspace_independent_calloc(mspace msp, size_t n_elements,
				 size_t elem_size, void* chunks[]) {
  size_t sz = elem_size; /* serves as 1-element array */
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
  return ialloc(ms, n_elements, &sz, 3, chunks);
}

void** mspace_independent_comalloc(mspace msp, size_t n_elements,
				   size_t sizes[], void* chunks[]) {
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
  return ialloc(ms, n_elements, sizes, 0, chunks);
}

int mspace_trim(mspace msp, size_t pad) {
  int result = 0;
  mstate ms = (mstate)msp;
  if (ok_magic(ms)) {
    if (!PREACTION(ms)) {
      result = sys_trim(ms, pad);
      POSTACTION(ms);
    }
  }
  else {
    USAGE_ERROR_ACTION(ms,ms);
  }
  return result;
}

void mspace_malloc_stats(mspace msp) {
  mstate ms = (mstate)msp;
  if (ok_magic(ms)) {
    internal_malloc_stats(ms);
  }
  else {
    USAGE_ERROR_ACTION(ms,ms);
  }
}

size_t mspace_footprint(mspace msp) {
  size_t result = 0;
  mstate ms = (mstate)msp;
  if (ok_magic(ms)) {
    result = ms->footprint;
  }
  else {
    USAGE_ERROR_ACTION(ms,ms);
  }
  return result;
}


size_t mspace_max_footprint(mspace msp) {
  size_t result = 0;
  mstate ms = (mstate)msp;
  if (ok_magic(ms)) {
    result = ms->max_footprint;
  }
  else {
    USAGE_ERROR_ACTION(ms,ms);
  }
  return result;
}


#if !NO_MALLINFO
struct mallinfo mspace_mallinfo(mspace msp) {
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
  }
  return internal_mallinfo(ms);
}
#endif /* NO_MALLINFO */

size_t mspace_usable_size(void* mem) {
  if (mem != 0) {
    mchunkptr p = mem2chunk(mem);
    if (cinuse(p))
      return chunksize(p) - overhead_for(p);
  }
  return 0;
}

int mspace_mallopt(int param_number, int value) {
  return change_mparam(param_number, value);
}

#endif /* MSPACES */

/* -------------------- Alternative MORECORE functions ------------------- */

/*
  Guidelines for creating a custom version of MORECORE:

  * For best performance, MORECORE should allocate in multiples of pagesize.
  * MORECORE may allocate more memory than requested. (Or even less,
      but this will usually result in a malloc failure.)
  * MORECORE must not allocate memory when given argument zero, but
      instead return one past the end address of memory from previous
      nonzero call.
  * For best performance, consecutive calls to MORECORE with positive
      arguments should return increasing addresses, indicating that
      space has been contiguously extended.
  * Even though consecutive calls to MORECORE need not return contiguous
      addresses, it must be OK for malloc'ed chunks to span multiple
      regions in those cases where they do happen to be contiguous.
  * MORECORE need not handle negative arguments -- it may instead
      just return MFAIL when given negative arguments.
      Negative arguments are always multiples of pagesize. MORECORE
      must not misinterpret negative args as large positive unsigned
      args. You can suppress all such calls from even occurring by defining
      MORECORE_CANNOT_TRIM,

  As an example alternative MORECORE, here is a custom allocator
  kindly contributed for pre-OSX macOS.  It uses virtually but not
  necessarily physically contiguous non-paged memory (locked in,
  present and won't get swapped out).  You can use it by uncommenting
  this section, adding some #includes, and setting up the appropriate
  defines above:

      #define MORECORE osMoreCore

  There is also a shutdown routine that should somehow be called for
  cleanup upon program exit.

  #define MAX_POOL_ENTRIES 100
  #define MINIMUM_MORECORE_SIZE  (64 * 1024U)
  static int next_os_pool;
  void *our_os_pools[MAX_POOL_ENTRIES];

  void *osMoreCore(int size)
  {
    void *ptr = 0;
    static void *sbrk_top = 0;

    if (size > 0)
    {
      if (size < MINIMUM_MORECORE_SIZE)
	 size = MINIMUM_MORECORE_SIZE;
      if (CurrentExecutionLevel() == kTaskLevel)
	 ptr = PoolAllocateResident(size + RM_PAGE_SIZE, 0);
      if (ptr == 0)
      {
	return (void *) MFAIL;
      }
      // save ptrs so they can be freed during cleanup
      our_os_pools[next_os_pool] = ptr;
      next_os_pool++;
      ptr = (void *) ((((size_t) ptr) + RM_PAGE_MASK) & ~RM_PAGE_MASK);
      sbrk_top = (char *) ptr + size;
      return ptr;
    }
    else if (size < 0)
    {
      // we don't currently support shrink behavior
      return (void *) MFAIL;
    }
    else
    {
      return sbrk_top;
    }
  }

  // cleanup any allocated memory pools
  // called as last thing before shutting down driver

  void osCleanupMem(void)
  {
    void **ptr;

    for (ptr = our_os_pools; ptr < &our_os_pools[MAX_POOL_ENTRIES]; ptr++)
      if (*ptr)
      {
	 PoolDeallocate(*ptr);
	 *ptr = 0;
      }
  }

*/


/* -----------------------------------------------------------------------
History:
    V2.8.4 (not yet released)
      * Add mspace_mmap_large_chunks; thanks to Jean Brouwers
      * Fix insufficient sys_alloc padding when using 16byte alignment
      * Fix bad error check in mspace_footprint
      * Adaptations for ptmalloc, courtesy of Wolfram Gloger.
      * Reentrant spin locks, courtesy of Earl Chew and others
      * Win32 improvements, courtesy of Niall Douglas and Earl Chew
      * Add NO_SEGMENT_TRAVERSAL and MAX_RELEASE_CHECK_RATE options
      * Extension hook in malloc_state
      * Various small adjustments to reduce warnings on some compilers
      * Various configuration extensions/changes for more platforms. Thanks
	 to all who contributed these.

    V2.8.3 Thu Sep 22 11:16:32 2005  Doug Lea  (dl at gee)
      * Add max_footprint functions
      * Ensure all appropriate literals are size_t
      * Fix conditional compilation problem for some #define settings
      * Avoid concatenating segments with the one provided
	in create_mspace_with_base
      * Rename some variables to avoid compiler shadowing warnings
      * Use explicit lock initialization.
      * Better handling of sbrk interference.
      * Simplify and fix segment insertion, trimming and mspace_destroy
      * Reinstate REALLOC_ZERO_BYTES_FREES option from 2.7.x
      * Thanks especially to Dennis Flanagan for help on these.

    V2.8.2 Sun Jun 12 16:01:10 2005  Doug Lea  (dl at gee)
      * Fix memalign brace error.

    V2.8.1 Wed Jun  8 16:11:46 2005  Doug Lea  (dl at gee)
      * Fix improper #endif nesting in C++
      * Add explicit casts needed for C++

    V2.8.0 Mon May 30 14:09:02 2005  Doug Lea  (dl at gee)
      * Use trees for large bins
      * Support mspaces
      * Use segments to unify sbrk-based and mmap-based system allocation,
	removing need for emulation on most platforms without sbrk.
      * Default safety checks
      * Optional footer checks. Thanks to William Robertson for the idea.
      * Internal code refactoring
      * Incorporate suggestions and platform-specific changes.
	Thanks to Dennis Flanagan, Colin Plumb, Niall Douglas,
	Aaron Bachmann,  Emery Berger, and others.
      * Speed up non-fastbin processing enough to remove fastbins.
      * Remove useless cfree() to avoid conflicts with other apps.
      * Remove internal memcpy, memset. Compilers handle builtins better.
      * Remove some options that no one ever used and rename others.

    V2.7.2 Sat Aug 17 09:07:30 2002  Doug Lea  (dl at gee)
      * Fix malloc_state bitmap array misdeclaration

    V2.7.1 Thu Jul 25 10:58:03 2002  Doug Lea  (dl at gee)
      * Allow tuning of FIRST_SORTED_BIN_SIZE
      * Use PTR_UINT as type for all ptr->int casts. Thanks to John Belmonte.
      * Better detection and support for non-contiguousness of MORECORE.
	Thanks to Andreas Mueller, Conal Walsh, and Wolfram Gloger
      * Bypass most of malloc if no frees. Thanks To Emery Berger.
      * Fix freeing of old top non-contiguous chunk im sysmalloc.
      * Raised default trim and map thresholds to 256K.
      * Fix mmap-related #defines. Thanks to Lubos Lunak.
      * Fix copy macros; added LACKS_FCNTL_H. Thanks to Neal Walfield.
      * Branch-free bin calculation
      * Default trim and mmap thresholds now 256K.

    V2.7.0 Sun Mar 11 14:14:06 2001  Doug Lea  (dl at gee)
      * Introduce independent_comalloc and independent_calloc.
	Thanks to Michael Pachos for motivation and help.
      * Make optional .h file available
      * Allow > 2GB requests on 32bit systems.
      * new WIN32 sbrk, mmap, munmap, lock code from <Walter@GeNeSys-e.de>.
	Thanks also to Andreas Mueller <a.mueller at paradatec.de>,
	and Anonymous.
      * Allow override of MALLOC_ALIGNMENT (Thanks to Ruud Waij for
	helping test this.)
      * memalign: check alignment arg
      * realloc: don't try to shift chunks backwards, since this
	leads to  more fragmentation in some programs and doesn't
	seem to help in any others.
      * Collect all cases in malloc requiring system memory into sysmalloc
      * Use mmap as backup to sbrk
      * Place all internal state in malloc_state
      * Introduce fastbins (although similar to 2.5.1)
      * Many minor tunings and cosmetic improvements
      * Introduce USE_PUBLIC_MALLOC_WRAPPERS, USE_MALLOC_LOCK
      * Introduce MALLOC_FAILURE_ACTION, MORECORE_CONTIGUOUS
	Thanks to Tony E. Bennett <tbennett@nvidia.com> and others.
      * Include errno.h to support default failure action.

    V2.6.6 Sun Dec  5 07:42:19 1999  Doug Lea  (dl at gee)
      * return null for negative arguments
      * Added Several WIN32 cleanups from Martin C. Fong <mcfong at yahoo.com>
	 * Add 'LACKS_SYS_PARAM_H' for those systems without 'sys/param.h'
	  (e.g. WIN32 platforms)
	 * Cleanup header file inclusion for WIN32 platforms
	 * Cleanup code to avoid Microsoft Visual C++ compiler complaints
	 * Add 'USE_DL_PREFIX' to quickly allow co-existence with existing
	   memory allocation routines
	 * Set 'malloc_getpagesize' for WIN32 platforms (needs more work)
	 * Use 'assert' rather than 'ASSERT' in WIN32 code to conform to
	   usage of 'assert' in non-WIN32 code
	 * Improve WIN32 'sbrk()' emulation's 'findRegion()' routine to
	   avoid infinite loop
      * Always call 'fREe()' rather than 'free()'

    V2.6.5 Wed Jun 17 15:57:31 1998  Doug Lea  (dl at gee)
      * Fixed ordering problem with boundary-stamping

    V2.6.3 Sun May 19 08:17:58 1996  Doug Lea  (dl at gee)
      * Added pvalloc, as recommended by H.J. Liu
      * Added 64bit pointer support mainly from Wolfram Gloger
      * Added anonymously donated WIN32 sbrk emulation
      * Malloc, calloc, getpagesize: add optimizations from Raymond Nijssen
      * malloc_extend_top: fix mask error that caused wastage after
	foreign sbrks
      * Add linux mremap support code from HJ Liu

    V2.6.2 Tue Dec  5 06:52:55 1995  Doug Lea  (dl at gee)
      * Integrated most documentation with the code.
      * Add support for mmap, with help from
	Wolfram Gloger (Gloger@lrz.uni-muenchen.de).
      * Use last_remainder in more cases.
      * Pack bins using idea from  colin@nyx10.cs.du.edu
      * Use ordered bins instead of best-fit threshold
      * Eliminate block-local decls to simplify tracing and debugging.
      * Support another case of realloc via move into top
      * Fix error occurring when initial sbrk_base not word-aligned.
      * Rely on page size for units instead of SBRK_UNIT to
	avoid surprises about sbrk alignment conventions.
      * Add mallinfo, mallopt. Thanks to Raymond Nijssen
	(raymond@es.ele.tue.nl) for the suggestion.
      * Add `pad' argument to malloc_trim and top_pad mallopt parameter.
      * More precautions for cases where other routines call sbrk,
	courtesy of Wolfram Gloger (Gloger@lrz.uni-muenchen.de).
      * Added macros etc., allowing use in linux libc from
	H.J. Lu (hjl@gnu.ai.mit.edu)
      * Inverted this history list

    V2.6.1 Sat Dec  2 14:10:57 1995  Doug Lea  (dl at gee)
      * Re-tuned and fixed to behave more nicely with V2.6.0 changes.
      * Removed all preallocation code since under current scheme
	the work required to undo bad preallocations exceeds
	the work saved in good cases for most test programs.
      * No longer use return list or unconsolidated bins since
	no scheme using them consistently outperforms those that don't
	given above changes.
      * Use best fit for very large chunks to prevent some worst-cases.
      * Added some support for debugging

    V2.6.0 Sat Nov  4 07:05:23 1995  Doug Lea  (dl at gee)
      * Removed footers when chunks are in use. Thanks to
	Paul Wilson (wilson@cs.texas.edu) for the suggestion.

    V2.5.4 Wed Nov  1 07:54:51 1995  Doug Lea  (dl at gee)
      * Added malloc_trim, with help from Wolfram Gloger
	(wmglo@Dent.MED.Uni-Muenchen.DE).

    V2.5.3 Tue Apr 26 10:16:01 1994  Doug Lea  (dl at g)

    V2.5.2 Tue Apr  5 16:20:40 1994  Doug Lea  (dl at g)
      * realloc: try to expand in both directions
      * malloc: swap order of clean-bin strategy;
      * realloc: only conditionally expand backwards
      * Try not to scavenge used bins
      * Use bin counts as a guide to preallocation
      * Occasionally bin return list chunks in first scan
      * Add a few optimizations from colin@nyx10.cs.du.edu

    V2.5.1 Sat Aug 14 15:40:43 1993  Doug Lea  (dl at g)
      * faster bin computation & slightly different binning
      * merged all consolidations to one part of malloc proper
	 (eliminating old malloc_find_space & malloc_clean_bin)
      * Scan 2 returns chunks (not just 1)
      * Propagate failure in realloc if malloc returns 0
      * Add stuff to allow compilation on non-ANSI compilers
	  from kpv@research.att.com

    V2.5 Sat Aug  7 07:41:59 1993  Doug Lea  (dl at g.oswego.edu)
      * removed potential for odd address access in prev_chunk
      * removed dependency on getpagesize.h
      * misc cosmetics and a bit more internal documentation
      * anticosmetics: mangled names in macros to evade debugger strangeness
      * tested on sparc, hp-700, dec-mips, rs6000
	  with gcc & native cc (hp, dec only) allowing
	  Detlefs & Zorn comparison study (in SIGPLAN Notices.)

    Trial version Fri Aug 28 13:14:29 1992  Doug Lea  (dl at g.oswego.edu)
      * Based loosely on libg++-1.2X malloc. (It retains some of the overall
	 structure of old version,  but most details differ.)

*/
