#include "git-compat-util.h"
#include "chunk-format.h"
#include "csum-file.h"
#include "cache.h"
#define CHUNK_LOOKUP_WIDTH 12

void write_table_of_contents(struct hashfile *f,
			     uint64_t cur_offset,
			     struct chunk_info *chunks,
			     int nr)
{
	int i;

	/* Add the table of contents to the current offset */
	cur_offset += (nr + 1) * CHUNK_LOOKUP_WIDTH;

	for (i = 0; i < nr; i++) {
		hashwrite_be32(f, chunks[i].id);
		hashwrite_be64(f, cur_offset);

		cur_offset += chunks[i].size;
	}

	/* Trailing entry marks the end of the chunks */
	hashwrite_be32(f, 0);
	hashwrite_be64(f, cur_offset);
}

int write_chunks(struct hashfile *f,
		 struct chunk_info *chunks,
		 int nr,
		 void *data)
{
	int i;

	for (i = 0; i < nr; i++) {
		uint64_t start_offset = f->total + f->offset;
		int result = chunks[i].write_fn(f, data);

		if (result)
			return result;

		if (f->total + f->offset != start_offset + chunks[i].size)
			BUG("expected to write %"PRId64" bytes to chunk %"PRIx32", but wrote %"PRId64" instead",
			    chunks[i].size, chunks[i].id,
			    f->total + f->offset - start_offset);
	}

	return 0;
}

int read_table_of_contents(const unsigned char *mfile,
			   size_t mfile_size,
			   uint64_t toc_offset,
			   int toc_length,
			   struct read_chunk_info *chunks,
			   int nr,
			   void *data)
{
	int i;
	uint32_t chunk_id;
	const unsigned char *table_of_contents = mfile + toc_offset;

	for (i = 0; i < nr; i++)
		chunks[i].found = 0;

	while (toc_length--) {
		int i;
		uint64_t chunk_offset, next_chunk_offset;

		chunk_id = get_be32(table_of_contents);
		chunk_offset = get_be64(table_of_contents + 4);

		if (!chunk_id) {
			error(_("terminating chunk id appears earlier than expected"));
			return 1;
		}

		table_of_contents += CHUNK_LOOKUP_WIDTH;
		next_chunk_offset = get_be64(table_of_contents + 4);

		if (next_chunk_offset < chunk_offset ||
		    next_chunk_offset > mfile_size - the_hash_algo->rawsz) {
			error(_("improper chunk offset(s) %"PRIx64" and %"PRIx64""),
			      chunk_offset, next_chunk_offset);
			return 1;
		}
		for (i = 0; i < nr; i++) {
			if (chunks[i].id == chunk_id) {
				int result;

				if (chunks[i].found) {
					error(_("duplicate chunk ID %"PRIx32" found"),
					      chunk_id);
					return 1;
				}

				chunks[i].found = 1;
				result = chunks[i].read_fn(
						mfile + chunk_offset,
						next_chunk_offset - chunk_offset,
						data);

				if (result)
					return result;
				break;
			}
		}
	}

	chunk_id = get_be32(table_of_contents);
	if (chunk_id) {
		error(_("final chunk has non-zero id %"PRIx32""), chunk_id);
		return 1;
	}

	return 0;
}
