#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "git-zlib.h"
#include "hash.h"
#include "hex.h"
#include "object-file.h"
#include "object.h"
#include "pack.h"
#include "parse-options.h"
#include "parse.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"
#include "write-or-die.h"

#define BLOCK_SIZE 0xffff
static const unsigned char zeros[BLOCK_SIZE];

/*
 * Write data as an uncompressed zlib stream.
 * For data larger than 64KB, writes multiple uncompressed blocks.
 * If data is NULL, writes zeros.
 * Updates the pack checksum context.
 */
static void write_uncompressed_zlib(FILE *f, struct git_hash_ctx *pack_ctx,
				    const void *data, size_t len,
				    const struct git_hash_algo *algo)
{
	unsigned char zlib_header[2] = { 0x78, 0x01 }; /* CMF, FLG */
	unsigned char block_header[5];
	const unsigned char *p = data;
	size_t remaining = len;
	uint32_t adler = 1L; /* adler32 initial value */
	unsigned char adler_buf[4];

	/* Write zlib header */
	fwrite_or_die(f, zlib_header, sizeof(zlib_header));
	algo->update_fn(pack_ctx, zlib_header, 2);

	/* Write uncompressed blocks (max 64KB each) */
	do {
		size_t block_len = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining;
		int is_final = (block_len == remaining);
		const unsigned char *block_data = data ? p : zeros;

		block_header[0] = is_final ? 0x01 : 0x00;
		block_header[1] = block_len & 0xff;
		block_header[2] = (block_len >> 8) & 0xff;
		block_header[3] = block_header[1] ^ 0xff;
		block_header[4] = block_header[2] ^ 0xff;

		fwrite_or_die(f, block_header, sizeof(block_header));
		algo->update_fn(pack_ctx, block_header, 5);

		if (block_len) {
			fwrite_or_die(f, block_data, block_len);
			algo->update_fn(pack_ctx, block_data, block_len);
			adler = adler32(adler, block_data, block_len);
		}

		if (data)
			p += block_len;
		remaining -= block_len;
	} while (remaining > 0);

	/* Write adler32 checksum */
	put_be32(adler_buf, adler);
	fwrite_or_die(f, adler_buf, sizeof(adler_buf));
	algo->update_fn(pack_ctx, adler_buf, 4);
}

/*
 * Write an uncompressed object to the pack file.
 * If `data == NULL`, it is treated like a buffer to NUL bytes.
 * Updates the pack checksum context.
 */
static void write_pack_object(FILE *f, struct git_hash_ctx *pack_ctx,
			      enum object_type type,
			      const void *data, size_t len,
			      struct object_id *oid,
			      const struct git_hash_algo *algo)
{
	unsigned char pack_header[MAX_PACK_OBJECT_HEADER];
	char object_header[32];
	int pack_header_len, object_header_len;
	struct git_hash_ctx ctx;

	/* Write pack object header */
	pack_header_len = encode_in_pack_object_header(pack_header,
						       sizeof(pack_header),
						       type, len);
	fwrite_or_die(f, pack_header, pack_header_len);
	algo->update_fn(pack_ctx, pack_header, pack_header_len);

	/* Write the data as uncompressed zlib */
	write_uncompressed_zlib(f, pack_ctx, data, len, algo);

	algo->init_fn(&ctx);
	object_header_len = format_object_header(object_header,
						 sizeof(object_header),
						 type, len);
	algo->update_fn(&ctx, object_header, object_header_len);
	if (data)
		algo->update_fn(&ctx, data, len);
	else {
		for (size_t i = len / BLOCK_SIZE; i; i--)
			algo->update_fn(&ctx, zeros, BLOCK_SIZE);
		algo->update_fn(&ctx, zeros, len % BLOCK_SIZE);
	}
	algo->final_oid_fn(oid, &ctx);
}

/*
 * Generate a pack file with a single large (>4GB) reachable object.
 *
 * Creates:
 *   1. A large blob (all NUL bytes)
 *   2. A tree containing that blob as "file"
 *   3. A commit using that tree
 *   4. The empty tree
 *   5. A child commit using the empty tree
 *
 * This is useful for testing that Git can handle objects larger than 4GB.
 */
static int generate_pack_with_large_object(const char *path, size_t blob_size,
					   const struct git_hash_algo *algo)
{
	FILE *f = xfopen(path, "wb");
	struct git_hash_ctx pack_ctx;
	unsigned char pack_hash[GIT_MAX_RAWSZ];
	struct object_id blob_oid, tree_oid, commit_oid, empty_tree_oid, final_commit_oid;
	struct strbuf buf = STRBUF_INIT;
	const uint32_t object_count = 5;
	struct pack_header pack_header = {
		.hdr_signature = htonl(PACK_SIGNATURE),
		.hdr_version = htonl(PACK_VERSION),
		.hdr_entries = htonl(object_count),
	};

	algo->init_fn(&pack_ctx);

	/* Write pack header */
	fwrite_or_die(f, &pack_header, sizeof(pack_header));
	algo->update_fn(&pack_ctx, &pack_header, sizeof(pack_header));

	/* 1. Write the large blob */
	write_pack_object(f, &pack_ctx, OBJ_BLOB, NULL, blob_size, &blob_oid, algo);

	/* 2. Write tree containing the blob as "file" */
	strbuf_addf(&buf, "100644 file%c", '\0');
	strbuf_add(&buf, blob_oid.hash, algo->rawsz);
	write_pack_object(f, &pack_ctx, OBJ_TREE, buf.buf, buf.len, &tree_oid, algo);

	/* 3. Write commit using that tree */
	strbuf_reset(&buf);
	strbuf_addf(&buf,
		    "tree %s\n"
		    "author A U Thor <author@example.com> 1234567890 +0000\n"
		    "committer C O Mitter <committer@example.com> 1234567890 +0000\n"
		    "\n"
		    "Large blob commit\n",
		    oid_to_hex(&tree_oid));
	write_pack_object(f, &pack_ctx, OBJ_COMMIT, buf.buf, buf.len, &commit_oid, algo);

	/* 4. Write the empty tree */
	write_pack_object(f, &pack_ctx, OBJ_TREE, "", 0, &empty_tree_oid, algo);

	/* 5. Write final commit using empty tree, with previous commit as parent */
	strbuf_reset(&buf);
	strbuf_addf(&buf,
		    "tree %s\n"
		    "parent %s\n"
		    "author A U Thor <author@example.com> 1234567890 +0000\n"
		    "committer C O Mitter <committer@example.com> 1234567890 +0000\n"
		    "\n"
		    "Empty tree commit\n",
		    oid_to_hex(&empty_tree_oid),
		    oid_to_hex(&commit_oid));
	write_pack_object(f, &pack_ctx, OBJ_COMMIT, buf.buf, buf.len, &final_commit_oid, algo);

	/* Write pack trailer (checksum) */
	algo->final_fn(pack_hash, &pack_ctx);
	fwrite_or_die(f, pack_hash, algo->rawsz);
	if (fclose(f))
		die_errno(_("could not close '%s'"), path);

	strbuf_release(&buf);

	/* Print the final commit OID so caller can set up refs */
	printf("%s\n", oid_to_hex(&final_commit_oid));

	return 0;
}

static int cmd__synthesize__pack(int argc, const char **argv,
				 const char *prefix UNUSED,
				 struct repository *repo)
{
	int non_git;
	int reachable_large = 0;
	const struct git_hash_algo *algo;
	size_t blob_size;
	uintmax_t blob_size_u;
	const char *path;
	const char * const usage[] = {
		"test-tool synthesize pack "
		"--reachable-large <blob-size> <filename>",
		NULL
	};
	struct option options[] = {
		OPT_BOOL(0, "reachable-large", &reachable_large,
			 N_("write a pack with a single reachable large blob")),
		OPT_END()
	};

	setup_git_directory_gently(&non_git);
	repo = the_repository;
	algo = unsafe_hash_algo(repo->hash_algo);

	argc = parse_options(argc, argv, NULL, options, usage,
			     PARSE_OPT_KEEP_ARGV0);
	if (argc != 3 || !reachable_large)
		usage_with_options(usage, options);

	if (!git_parse_unsigned(argv[1], &blob_size_u,
				maximum_unsigned_value_of_type(size_t)))
		die(_("'%s' is not a valid blob size"), argv[1]);
	blob_size = blob_size_u;
	path = argv[2];

	return !!generate_pack_with_large_object(path, blob_size, algo);
}

int cmd__synthesize(int argc, const char **argv)
{
	const char *prefix = NULL;
	char const * const synthesize_usage[] = {
		"test-tool synthesize pack <options>",
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("pack", &fn, cmd__synthesize__pack),
		OPT_END()
	};
	argc = parse_options(argc, argv, prefix, options, synthesize_usage, 0);
	return !!fn(argc, argv, prefix, NULL);
}
