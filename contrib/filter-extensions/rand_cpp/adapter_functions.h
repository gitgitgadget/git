#ifndef RAND_CPP_ADAPTER_FUNCTIONS_H
#define RAND_CPP_ADAPTER_FUNCTIONS_H

struct object;

uint64_t getnanotime(void);

char *obj_to_hex_oid(struct object *obj);

#endif /* RAND_CPP_ADAPTER_FUNCTIONS_H */
