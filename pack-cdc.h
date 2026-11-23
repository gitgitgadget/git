#ifndef PACK_CDC_H
#define PACK_CDC_H

struct cdc_fingerprint_set;

/*
 * Collect a blob's distinct chunk fingerprints. The result does not
 * retain the input buffer and can be released with free(). Return NULL
 * for out-of-range input or if allocation fails.
 */
struct cdc_fingerprint_set *pack_cdc_fingerprint_blob(const void *data,
						      size_t size);

/*
 * Sum the lengths of chunks with distinct matching fingerprints, or return
 * zero if either set is missing. This is only an estimate; callers must
 * still compare the actual bytes when choosing a delta.
 */
size_t pack_cdc_shared_bytes(const struct cdc_fingerprint_set *target,
			     const struct cdc_fingerprint_set *base);

#endif
