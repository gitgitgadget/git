#ifndef PUBLIC_SYMBOL_EXPORT_H
#define PUBLIC_SYMBOL_EXPORT_H

struct libgit_config_set *libgit_configset_alloc(void);

void libgit_configset_free(struct libgit_config_set *cs);

int libgit_configset_add_file(struct libgit_config_set *cs, const char *filename);

int libgit_configset_get_int(struct libgit_config_set *cs, const char *key, int *dest);

int libgit_configset_get_string(struct libgit_config_set *cs, const char *key, char **dest);

int libgit_configset_get_bool(struct libgit_config_set *cs, const char *key, int *dest);

int libgit_configset_get_ulong(struct libgit_config_set *cs, const char *key, unsigned long *dest);

int libgit_configset_get_pathname(struct libgit_config_set *cs, const char *key, char **dest);

const char *libgit_user_agent(void);

const char *libgit_user_agent_sanitized(void);

#endif /* PUBLIC_SYMBOL_EXPORT_H */
