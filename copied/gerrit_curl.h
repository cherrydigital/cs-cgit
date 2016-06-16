#ifndef _CURL_GERRIT_H_
#define _CURL_GERRIT_H_

// CHERRY
typedef struct _MemoryStruct {
  char *memory;
  size_t size;
} MemoryStruct;

int gerrit_connect(const char *url, MemoryStruct *chunk);
int gerrit_get_project_list(char *value, struct cgit_context *ctx, repo_config_fn repo_config); 
int parse_json(const char *data); 

#endif
