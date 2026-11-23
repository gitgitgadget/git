#include "git-compat-util.h"
#include "pack-cdc.h"

#define CDC_MIN_CHUNK (16 * 1024)
#define CDC_AVG_CHUNK (64 * 1024)
#define CDC_MAX_CHUNK (256 * 1024)
#define CDC_MIN_BLOB_SIZE (4 * 1024 * 1024)
#define CDC_MAX_BLOB_SIZE (64 * 1024 * 1024)
#define CDC_STRICT_MASK (CDC_AVG_CHUNK * 2 - 1)
#define CDC_LOOSE_MASK (CDC_AVG_CHUNK / 2 - 1)
#define CDC_FNV_PRIME 1099511628211ULL

/* SplitMix64 output for seed 0x243f6a8885a308d3, indexed by byte. */
static const uint64_t gear[256] = {
	0x2cb0f69f4abea221ULL, 0x9417034723148989ULL, 0xdd555950609dfe03ULL,
	0xdbafb150deb12800ULL, 0x7e789b2e6c442cb6ULL, 0xf41e5636c7e4f8c4ULL,
	0x0959d150f8fba7e4ULL, 0xa97316f13cdb9eeaULL, 0x74cd8258f9520068ULL,
	0x55c74a62e116868bULL, 0xd2f4c799a2023cbdULL, 0xdf98cb79a37b51b9ULL,
	0x396f5885524f3905ULL, 0xaf1d56386ca3b276ULL, 0xa9ffbe6b5104e85aULL,
	0x6bd0c51b9fd533b3ULL, 0x980ce91c50ab4b56ULL, 0x28ac395780fe62c5ULL,
	0x768912e3a6bcedc7ULL, 0x50b3e8c9332c7c88ULL, 0xce3bbfe520bd47daULL,
	0xcba6c8e8e0bb7c4fULL, 0xbf194db8434a346dULL, 0x7d8f2a7b60416d7fULL,
	0x0849d1f6e0e10a5eULL, 0x7654b590d064e22fULL, 0x16d1da9507df3af2ULL,
	0xf63aef1089ea30e4ULL, 0x9ade6673cc6c522bULL, 0x4c75bc274e37087cULL,
	0xd35e12b49f51f27bULL, 0x22ddf2ffcee481eaULL, 0x06007fb13c59a1f1ULL,
	0x8966a38c651ea4daULL, 0x25242f018fc01ac6ULL, 0xa73ec74fa31b717cULL,
	0x7ee0abdd9797d3a2ULL, 0x5c06ff7dc4ac1880ULL, 0x8434e41042c28a7dULL,
	0x770a372d64327351ULL, 0xeed940dad9e9c06dULL, 0x8977e93646524825ULL,
	0xa9897f0a62a51616ULL, 0xa35d4250c53f2b3aULL, 0x4072542a94b9c33eULL,
	0x3154a7a62447e8abULL, 0x686865712a1a245eULL, 0x0fba67727d7b3b98ULL,
	0x0634e2024536912fULL, 0xd9ff52a26cf9881aULL, 0x9435dc0399f932daULL,
	0x18d39fc1af93e7f0ULL, 0x12f7147c1e7f46abULL, 0xdedf66783eddb4a0ULL,
	0x6f75480614554798ULL, 0xe40e95e8ef84bde2ULL, 0xbb41fe601fefb566ULL,
	0x5c3702e4c7bf19f1ULL, 0x8c7d1d0d3d4a8ec5ULL, 0xee779996ba62dccbULL,
	0x80ccb15bf530844bULL, 0xdf56e7dc4d57959cULL, 0x9eb86a81fe90b68eULL,
	0x6a25741fa696fbd3ULL, 0x7009346385a45644ULL, 0x8f4acc8c1520dd73ULL,
	0x75a59d61ae0f8464ULL, 0xd9600a5f4b8b735cULL, 0x90ee70d4c2774058ULL,
	0x8a5f6c4b9a613341ULL, 0xbae94e097390fd42ULL, 0x653727708a8cae7cULL,
	0x54a64593163b976fULL, 0x551fb9261926a565ULL, 0x903b2aad4c38672aULL,
	0x83731d929aa1ff24ULL, 0x48311d2ec01f36edULL, 0x53a5db5b92e313efULL,
	0xd3b8cb608aab8b70ULL, 0x0f022cd022ea0cbfULL, 0xba7e97a12f21baa6ULL,
	0xb895acc1e36f3046ULL, 0x88cb4b1adbf0f0c0ULL, 0xa08f47edd89b430bULL,
	0x4060ccb36efd6c18ULL, 0x0dcf835fb6b9345eULL, 0x38df4ac46ee5762bULL,
	0x986360357932dcbdULL, 0xbdeb8d63741fe7d9ULL, 0x5d23cb0aedffc430ULL,
	0x6a5efe3a842100a4ULL, 0x0d4cc01bf4e09a16ULL, 0x03dbef4217c97212ULL,
	0x3d8ded6c69c8b3acULL, 0x53d290fa4dcee280ULL, 0x00ce706478000997ULL,
	0xbdf7b12c56756763ULL, 0x06c99071719dc103ULL, 0xd5897678e0df3feeULL,
	0x74429d9ac72f7146ULL, 0x9730ae769149cbbaULL, 0x10ec1a636fd6612dULL,
	0x5dc5d9ea650fa766ULL, 0xb360e068cac3adc2ULL, 0xf8df11cb5ce17a0cULL,
	0xa9292bbae2191df9ULL, 0x3f3d169157da4aefULL, 0x41d2dab33367f9dfULL,
	0x95e671eefbd33caeULL, 0xd5bedcacb64a8fa9ULL, 0xe494760f1ba45656ULL,
	0x21b556b8b6ee2c5fULL, 0xa1ed31d3d69b05ccULL, 0x025819f971a39e83ULL,
	0xb9b3379a4081919aULL, 0x550758640bf14a28ULL, 0x151feebb4e040f10ULL,
	0x423490df7adfc8b3ULL, 0x8bae8d6e276c88e4ULL, 0x526dd4f720811612ULL,
	0xffd5fb93b0b2d28cULL, 0xa9abb68f830215a8ULL, 0x1751110c78d039feULL,
	0x103f09c76e08c0b5ULL, 0x2862583ce905324fULL, 0x939829751e945862ULL,
	0xfd2baf95439547eeULL, 0x3f96e3e88a7e3ef0ULL, 0x3db34783d40d6e72ULL,
	0xb2fd49e41fa25861ULL, 0x18d2c928bf0bc4a3ULL, 0x2806ff0a63ce82b4ULL,
	0x86748de3e14404e4ULL, 0xa22ae3b5ff1a68ceULL, 0x316214df224e0d71ULL,
	0xd8fb60f9bcdde6b5ULL, 0x75931e90d5b688cdULL, 0x97974eee0cea70baULL,
	0x3c0e3e31c2286c53ULL, 0x538bc977baa5c994ULL, 0xf384a2908191bd29ULL,
	0x0e28d06838b555d6ULL, 0xe3cf2205411e6d7aULL, 0xedecb325806e77f0ULL,
	0x5b8463e7456b20b8ULL, 0x5569ba971a13cabdULL, 0x97d3d2e344f1e484ULL,
	0x17704ebfa5491f08ULL, 0xd068968795a32b72ULL, 0x7d579c7c04aea72aULL,
	0x056f6c5d6e07d38dULL, 0x8267cc6ec5069efcULL, 0xdf270c1ef21852dfULL,
	0x75f3cfa3ff5b74a8ULL, 0x9453cd41c9093294ULL, 0xad8cc50d02158220ULL,
	0x494a8e68b6811522ULL, 0xfdc2dc1fb526a978ULL, 0xa00d7fb47afa2772ULL,
	0x02a5a6b22b45d376ULL, 0xdb7a320686bd2cbbULL, 0xbb7ec9db8ed84107ULL,
	0xa0419a506cb535efULL, 0x751678b4c82d1e2aULL, 0xd6a0398ca01ef5acULL,
	0xbec9d0e6fd0b27e8ULL, 0x363ed5d997c510eaULL, 0xaa8cfd101861575fULL,
	0xc35f6c57190c3646ULL, 0xaa58edd1230b6282ULL, 0xaee6bb4c99509c3aULL,
	0x6a1e8c62db7b532bULL, 0xd275c05e4924350aULL, 0xdd5c0daa5d4b823eULL,
	0xa9ae10999c1f45daULL, 0xd0778e076a846e20ULL, 0x6f7304aecd9bbf45ULL,
	0x692ab383113c68aeULL, 0x8b0280356f484328ULL, 0x99866efb37b72076ULL,
	0xb5797760c7108ba6ULL, 0x439febc33d5c0ca0ULL, 0xa306a36c73e81d09ULL,
	0xa927b037250bc6b9ULL, 0xdf2bde709a68740bULL, 0xedcd706720f932ccULL,
	0x61a884c301ee6d4eULL, 0x8108084290f3f2efULL, 0x28321ea11485bd62ULL,
	0x969e36e0e6f9b6deULL, 0x3e6b1d5cf28c5483ULL, 0xc72ebc0070076b77ULL,
	0x13d73121a7a448f6ULL, 0x22743fa795feb53aULL, 0x2bd608cca7803150ULL,
	0xcae4b5723d21581cULL, 0x8e70bbb87a85a239ULL, 0xd98023b873b129aeULL,
	0x77b69e4fcfe53920ULL, 0x0508e387973f9b5fULL, 0xbf2966d283c64f11ULL,
	0xaecdf57019e23471ULL, 0x36e7a8e998fe1e04ULL, 0x0780542bb39c8cd9ULL,
	0x4095e66dab7aee65ULL, 0x2086704201a7469eULL, 0x5a5d698442d2e216ULL,
	0xe421106739485e0cULL, 0xea88e48d6eedd5edULL, 0xf8f91dad5142564dULL,
	0x0504199b2e70f466ULL, 0xa0b0e2c6526d6ee5ULL, 0xfb3bef18a0e0c8a9ULL,
	0x197b1a5236d9566bULL, 0xb14e3945730a5bdfULL, 0xb9b7d6906877ea75ULL,
	0xf618a46b8de61fc1ULL, 0x3fb889497a2f1241ULL, 0xb3aeeaf7fefa8bc5ULL,
	0xcbe100a2efd63f9aULL, 0x3556152543cc4204ULL, 0xd9605d470d63ab58ULL,
	0x15545749b38b81b5ULL, 0x22db5baa269e9752ULL, 0x780040e30aa2c9e6ULL,
	0xc180448b0640c9cbULL, 0x6b2a492483c9456eULL, 0xa76cee29e128036cULL,
	0x089f699d6bb0f074ULL, 0x29faf34444846ecaULL, 0xb3c982023f05a58bULL,
	0xe6efc66581e03a5aULL, 0x52939eb64b758485ULL, 0xf9354e3df005a534ULL,
	0xc68b2a012aa99d70ULL, 0xea7d677dc1397e0fULL, 0x1734bd4c86de6e03ULL,
	0x0356a82459388a9fULL, 0xc43aa3ece4266ee2ULL, 0x893bc7d1412eae2dULL,
	0x3aab49744f9b080eULL, 0xed294b9dfc776923ULL, 0xcd6e499b5d4dade2ULL,
	0x9550e1f6c3b36609ULL, 0x2283c0a27f964ef1ULL, 0x3a9760919b276c63ULL,
	0xdec8b25069a70cfbULL, 0x3b5fab4305a819c8ULL, 0x37accf033fb26034ULL,
	0x9c01f1c52e8578ddULL, 0xc810f4676d8701dfULL, 0x6233712c854b1dfcULL,
	0x90fa9224644845d6ULL, 0x9305a3afe347f3d0ULL, 0xd5e66dbd1941872bULL,
	0xe23fa3d2ba84472eULL,
};

struct cdc_fingerprint {
	uint64_t hash;
	uint32_t length;
};

struct cdc_fingerprint_set {
	size_t nr;
	struct cdc_fingerprint entries[FLEX_ARRAY];
};

static int compare_fingerprints(const void *a, const void *b)
{
	const struct cdc_fingerprint *x = a, *y = b;

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	return (x->length > y->length) - (x->length < y->length);
}

struct cdc_fingerprint_set *pack_cdc_fingerprint_blob(const void *buffer,
						      size_t size)
{
	const unsigned char *data = buffer;
	struct cdc_fingerprint_set *set;
	size_t alloc, unique = 0;

	if (!data || size < CDC_MIN_BLOB_SIZE || size > CDC_MAX_BLOB_SIZE)
		return NULL;

	alloc = sizeof(*set) +
		DIV_ROUND_UP(size, CDC_MIN_CHUNK) * sizeof(*set->entries);
	set = malloc(alloc);
	if (!set)
		return NULL;
	set->nr = 0;

	for (size_t start = 0; start < size; ) {
		size_t end = start + CDC_MIN_CHUNK;
		uint64_t cut_hash = 0;
		uint64_t fingerprint = 14695981039346656037ULL;

		/* Include bytes skipped by the boundary search. */
		if (end > size)
			end = size;
		for (size_t i = start; i < end; i++)
			fingerprint = (fingerprint ^ data[i]) * CDC_FNV_PRIME;

		/* Use the same byte for the cut hash and the fingerprint. */
		while (end < size && end - start < CDC_MAX_CHUNK) {
			unsigned char byte = data[end++];
			uint64_t mask;

			cut_hash = (cut_hash << 1) + gear[byte];
			fingerprint = (fingerprint ^ byte) * CDC_FNV_PRIME;
			mask = end - start < CDC_AVG_CHUNK ?
				CDC_STRICT_MASK : CDC_LOOSE_MASK;
			if (!(cut_hash & mask))
				break;
		}

		set->entries[set->nr++] = (struct cdc_fingerprint){
			.hash = fingerprint,
			.length = end - start,
		};
		start = end;
	}

	QSORT(set->entries, set->nr, compare_fingerprints);
	/* Repeated chunks should not inflate a candidate's score. */
	for (size_t i = 0; i < set->nr; i++) {
		if (unique &&
		    !compare_fingerprints(&set->entries[unique - 1],
					  &set->entries[i]))
			continue;
		set->entries[unique++] = set->entries[i];
	}
	set->nr = unique;
	return set;
}

size_t pack_cdc_shared_bytes(const struct cdc_fingerprint_set *target,
			     const struct cdc_fingerprint_set *base)
{
	size_t score = 0, t = 0, b = 0;

	if (!target || !base)
		return 0;

	while (t < target->nr && b < base->nr) {
		int cmp = compare_fingerprints(&target->entries[t],
					       &base->entries[b]);

		if (!cmp)
			score += target->entries[t].length;
		if (cmp <= 0)
			t++;
		if (cmp >= 0)
			b++;
	}
	return score;
}
