#ifndef _SCAN_TREE_H_
#define _SCAN_TREE_H_
// CHERRY
#include "cgit.h"
extern void scan_projects(const char *path, const char *projectsfile, repo_config_fn fn);
extern void scan_tree(const char *path, repo_config_fn fn);
extern int gerrit_scan_projects(const char *path, const char *data, repo_config_fn fn); 

#endif
