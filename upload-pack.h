#ifndef UPLOAD_PACK_H
#define UPLOAD_PACK_H

/* Remember to update object flag allocation in object.h */
#define THEY_HAVE	(1u << 11)
#define OUR_REF		(1u << 12)
#define WANTED		(1u << 13)
#define COMMON_KNOWN	(1u << 14)

#define SHALLOW		(1u << 16)
#define NOT_SHALLOW	(1u << 17)
#define CLIENT_SHALLOW	(1u << 18)
#define HIDDEN_REF	(1u << 19)

#define ALL_FLAGS (THEY_HAVE | OUR_REF | WANTED | COMMON_KNOWN | SHALLOW | \
		NOT_SHALLOW | CLIENT_SHALLOW | HIDDEN_REF)

void upload_pack(const int advertise_refs, const int stateless_rpc,
		 const int timeout);

struct repository;
struct packet_reader;
int upload_pack_v2(struct repository *r, struct packet_reader *request);

struct strbuf;
int upload_pack_advertise(struct repository *r,
			  struct strbuf *value);

#endif /* UPLOAD_PACK_H */
