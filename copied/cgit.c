/* cgit.c: cgi for the git scm
 *
 * Copyright (C) 2006 Lars Hjemli
 * Copyright (C) 2010-2013 Jason A. Donenfeld <Jason@zx2c4.com>
 *
 * Licensed under GNU General Public License v2
 *   (see COPYING for full license text)
 */

#include "cgit.h"
#include "cache.h"
#include "cmd.h"
#include "configfile.h"
#include "html.h"
#include "ui-shared.h"
#include "ui-stats.h"
#include "ui-blob.h"
#include "ui-summary.h"
#include "scan-tree.h"

/* cherry */
#include "gerrit_curl.h" 
#include <jansson.h>
int gerrit_scan_projects(const char *path, const char *data, repo_config_fn fn);
/* //cherry */


const char *cgit_version = CGIT_VERSION;


static void add_mimetype(const char *name, const char *value)
{
	struct string_list_item *item;

	item = string_list_insert(&ctx.cfg.mimetypes, xstrdup(name));
	item->util = xstrdup(value);
}

static struct cgit_filter *new_filter(const char *cmd, filter_type filtertype)
{
	struct cgit_filter *f;
	int args_size = 0;
	int extra_args;

	if (!cmd || !cmd[0])
		return NULL;

	switch (filtertype) {
		case SOURCE:
		case ABOUT:
			extra_args = 1;
			break;

		case COMMIT:
		default:
			extra_args = 0;
			break;
	}

	f = xmalloc(sizeof(struct cgit_filter));
	f->cmd = xstrdup(cmd);
	args_size = (2 + extra_args) * sizeof(char *);
	f->argv = xmalloc(args_size);
	memset(f->argv, 0, args_size);
	f->argv[0] = f->cmd;
	return f;
}

static void process_cached_repolist(const char *path);

static void repo_config(struct cgit_repo *repo, const char *name, const char *value)
{
	struct string_list_item *item;

	if (!strcmp(name, "name"))
		repo->name = xstrdup(value);
	else if (!strcmp(name, "clone-url"))
		repo->clone_url = xstrdup(value);
	else if (!strcmp(name, "desc"))
		repo->desc = xstrdup(value);
	else if (!strcmp(name, "owner"))
		repo->owner = xstrdup(value);
	else if (!strcmp(name, "defbranch"))
		repo->defbranch = xstrdup(value);
	else if (!strcmp(name, "snapshots"))
		repo->snapshots = ctx.cfg.snapshots & cgit_parse_snapshots_mask(value);
	else if (!strcmp(name, "enable-commit-graph"))
		repo->enable_commit_graph = atoi(value);
	else if (!strcmp(name, "enable-log-filecount"))
		repo->enable_log_filecount = atoi(value);
	else if (!strcmp(name, "enable-log-linecount"))
		repo->enable_log_linecount = atoi(value);
	else if (!strcmp(name, "enable-remote-branches"))
		repo->enable_remote_branches = atoi(value);
	else if (!strcmp(name, "enable-subject-links"))
		repo->enable_subject_links = atoi(value);
	else if (!strcmp(name, "branch-sort")) {
		if (!strcmp(value, "age"))
			repo->branch_sort = 1;
		if (!strcmp(value, "name"))
			repo->branch_sort = 0;
	} else if (!strcmp(name, "commit-sort")) {
		if (!strcmp(value, "date"))
			repo->commit_sort = 1;
		if (!strcmp(value, "topo"))
			repo->commit_sort = 2;
	} else if (!strcmp(name, "max-stats"))
		repo->max_stats = cgit_find_stats_period(value, NULL);
	else if (!strcmp(name, "module-link"))
		repo->module_link= xstrdup(value);
	else if (!prefixcmp(name, "module-link.")) {
		item = string_list_append(&repo->submodules, xstrdup(name + 12));
		item->util = xstrdup(value);
	} else if (!strcmp(name, "section"))
		repo->section = xstrdup(value);
	else if (!strcmp(name, "readme") && value != NULL) {
		if (repo->readme.items == ctx.cfg.readme.items)
			memset(&repo->readme, 0, sizeof(repo->readme));
		string_list_append(&repo->readme, xstrdup(value));
	} else if (!strcmp(name, "logo") && value != NULL)
		repo->logo = xstrdup(value);
	else if (!strcmp(name, "logo-link") && value != NULL)
		repo->logo_link = xstrdup(value);
	else if (ctx.cfg.enable_filter_overrides) {
		if (!strcmp(name, "about-filter"))
			repo->about_filter = new_filter(value, ABOUT);
		else if (!strcmp(name, "commit-filter"))
			repo->commit_filter = new_filter(value, COMMIT);
		else if (!strcmp(name, "source-filter"))
			repo->source_filter = new_filter(value, SOURCE);
	}
}

static void config_cb(const char *name, const char *value)
{
	if (!strcmp(name, "section") || !strcmp(name, "repo.group"))
		ctx.cfg.section = xstrdup(value);
	else if (!strcmp(name, "repo.url"))
		ctx.repo = cgit_add_repo(value);
	else if (ctx.repo && !strcmp(name, "repo.path"))
		ctx.repo->path = trim_end(value, '/');
	else if (ctx.repo && !prefixcmp(name, "repo."))
		repo_config(ctx.repo, name + 5, value);
	else if (!strcmp(name, "readme") && value != NULL)
		string_list_append(&ctx.cfg.readme, xstrdup(value));
	else if (!strcmp(name, "root-title"))
		ctx.cfg.root_title = xstrdup(value);
	else if (!strcmp(name, "root-desc"))
		ctx.cfg.root_desc = xstrdup(value);
	else if (!strcmp(name, "root-readme"))
		ctx.cfg.root_readme = xstrdup(value);
	else if (!strcmp(name, "css"))
		ctx.cfg.css = xstrdup(value);
	else if (!strcmp(name, "favicon"))
		ctx.cfg.favicon = xstrdup(value);
	else if (!strcmp(name, "footer"))
		ctx.cfg.footer = xstrdup(value);
	else if (!strcmp(name, "head-include"))
		ctx.cfg.head_include = xstrdup(value);
	else if (!strcmp(name, "header"))
		ctx.cfg.header = xstrdup(value);
	else if (!strcmp(name, "logo"))
		ctx.cfg.logo = xstrdup(value);
	else if (!strcmp(name, "index-header"))
		ctx.cfg.index_header = xstrdup(value);
	else if (!strcmp(name, "index-info"))
		ctx.cfg.index_info = xstrdup(value);
	else if (!strcmp(name, "logo-link"))
		ctx.cfg.logo_link = xstrdup(value);
	else if (!strcmp(name, "module-link"))
		ctx.cfg.module_link = xstrdup(value);
	else if (!strcmp(name, "strict-export"))
		ctx.cfg.strict_export = xstrdup(value);
	else if (!strcmp(name, "virtual-root")) {
		ctx.cfg.virtual_root = ensure_end(value, '/');
	} else if (!strcmp(name, "nocache"))
		ctx.cfg.nocache = atoi(value);
	else if (!strcmp(name, "noplainemail"))
		ctx.cfg.noplainemail = atoi(value);
	else if (!strcmp(name, "noheader"))
		ctx.cfg.noheader = atoi(value);
	else if (!strcmp(name, "snapshots"))
		ctx.cfg.snapshots = cgit_parse_snapshots_mask(value);
	else if (!strcmp(name, "enable-filter-overrides"))
		ctx.cfg.enable_filter_overrides = atoi(value);
	else if (!strcmp(name, "enable-http-clone"))
		ctx.cfg.enable_http_clone = atoi(value);
	else if (!strcmp(name, "enable-index-links"))
		ctx.cfg.enable_index_links = atoi(value);
	else if (!strcmp(name, "enable-index-owner"))
		ctx.cfg.enable_index_owner = atoi(value);
	else if (!strcmp(name, "enable-commit-graph"))
		ctx.cfg.enable_commit_graph = atoi(value);
	else if (!strcmp(name, "enable-log-filecount"))
		ctx.cfg.enable_log_filecount = atoi(value);
	else if (!strcmp(name, "enable-log-linecount"))
		ctx.cfg.enable_log_linecount = atoi(value);
	else if (!strcmp(name, "enable-remote-branches"))
		ctx.cfg.enable_remote_branches = atoi(value);
	else if (!strcmp(name, "enable-subject-links"))
		ctx.cfg.enable_subject_links = atoi(value);
	else if (!strcmp(name, "enable-tree-linenumbers"))
		ctx.cfg.enable_tree_linenumbers = atoi(value);
	else if (!strcmp(name, "enable-git-config"))
		ctx.cfg.enable_git_config = atoi(value);
	else if (!strcmp(name, "max-stats"))
		ctx.cfg.max_stats = cgit_find_stats_period(value, NULL);
	else if (!strcmp(name, "cache-size"))
		ctx.cfg.cache_size = atoi(value);
	else if (!strcmp(name, "cache-root"))
		ctx.cfg.cache_root = xstrdup(expand_macros(value));
	else if (!strcmp(name, "cache-root-ttl"))
		ctx.cfg.cache_root_ttl = atoi(value);
	else if (!strcmp(name, "cache-repo-ttl"))
		ctx.cfg.cache_repo_ttl = atoi(value);
	else if (!strcmp(name, "cache-scanrc-ttl"))
		ctx.cfg.cache_scanrc_ttl = atoi(value);
	else if (!strcmp(name, "cache-static-ttl"))
		ctx.cfg.cache_static_ttl = atoi(value);
	else if (!strcmp(name, "cache-dynamic-ttl"))
		ctx.cfg.cache_dynamic_ttl = atoi(value);
	else if (!strcmp(name, "case-sensitive-sort"))
		ctx.cfg.case_sensitive_sort = atoi(value);
	else if (!strcmp(name, "about-filter"))
		ctx.cfg.about_filter = new_filter(value, ABOUT);
	else if (!strcmp(name, "commit-filter"))
		ctx.cfg.commit_filter = new_filter(value, COMMIT);
	else if (!strcmp(name, "embedded"))
		ctx.cfg.embedded = atoi(value);
	else if (!strcmp(name, "max-atom-items"))
		ctx.cfg.max_atom_items = atoi(value);
	else if (!strcmp(name, "max-message-length"))
		ctx.cfg.max_msg_len = atoi(value);
	else if (!strcmp(name, "max-repodesc-length"))
		ctx.cfg.max_repodesc_len = atoi(value);
	else if (!strcmp(name, "max-blob-size"))
		ctx.cfg.max_blob_size = atoi(value);
	else if (!strcmp(name, "max-repo-count"))
		ctx.cfg.max_repo_count = atoi(value);
	else if (!strcmp(name, "max-commit-count"))
		ctx.cfg.max_commit_count = atoi(value);
	else if (!strcmp(name, "project-list"))
		ctx.cfg.project_list = xstrdup(expand_macros(value));
	/* CHERRY */
	else if (!strcmp(name, "gerrit-project-list-url"))
		ctx.cfg.gerrit_project_list_url = xstrdup(expand_macros(value));
	else if (!strcmp(name, "gerrit-login-url"))
		ctx.cfg.gerrit_login_url = xstrdup(expand_macros(value));
	else if (!strcmp(name, "gerrit-index-url"))
		ctx.cfg.gerrit_index_url = xstrdup(expand_macros(value));
	else if (!strcmp(name, "gerrit-cgit-url"))
		ctx.cfg.gerrit_cgit_url = xstrdup(expand_macros(value));
	/* //CHERRY */
	else if (!strcmp(name, "scan-path")) {
		if (!ctx.cfg.nocache && ctx.cfg.cache_size) {
			process_cached_repolist(expand_macros(value));
		}
		/* SPIN */
		//else if (ctx.cfg.gerrit_project_list_url) {
		else if( (ctx.cfg.gerrit_project_list_url)  && (ctx.cfg.gerrit_login_url) && (ctx.cfg.gerrit_index_url) && (ctx.cfg.gerrit_cgit_url) ) {

#if MYDEBUG
			fprintf(stderr, "DEBUG get_project_list_url ->%s<-\n", ctx.cfg.gerrit_project_list_url);
#endif
			gerrit_get_project_list(expand_macros(value), &ctx, repo_config);
		}
		/* //SPIN */
		else if (ctx.cfg.project_list) {
			fprintf(stderr,"project_list found\n");
			scan_projects(expand_macros(value), ctx.cfg.project_list, repo_config);
		}
		else {
			scan_tree(expand_macros(value), repo_config);
		}
	}
	else if (!strcmp(name, "scan-hidden-path"))
		ctx.cfg.scan_hidden_path = atoi(value);
	else if (!strcmp(name, "section-from-path"))
		ctx.cfg.section_from_path = atoi(value);
	else if (!strcmp(name, "repository-sort"))
		ctx.cfg.repository_sort = xstrdup(value);
	else if (!strcmp(name, "section-sort"))
		ctx.cfg.section_sort = atoi(value);
	else if (!strcmp(name, "source-filter"))
		ctx.cfg.source_filter = new_filter(value, SOURCE);
	else if (!strcmp(name, "summary-log"))
		ctx.cfg.summary_log = atoi(value);
	else if (!strcmp(name, "summary-branches"))
		ctx.cfg.summary_branches = atoi(value);
	else if (!strcmp(name, "summary-tags"))
		ctx.cfg.summary_tags = atoi(value);
	else if (!strcmp(name, "side-by-side-diffs"))
		ctx.cfg.ssdiff = atoi(value);
	else if (!strcmp(name, "agefile"))
		ctx.cfg.agefile = xstrdup(value);
	else if (!strcmp(name, "mimetype-file"))
		ctx.cfg.mimetype_file = xstrdup(value);
	else if (!strcmp(name, "renamelimit"))
		ctx.cfg.renamelimit = atoi(value);
	else if (!strcmp(name, "remove-suffix"))
		ctx.cfg.remove_suffix = atoi(value);
	else if (!strcmp(name, "robots"))
		ctx.cfg.robots = xstrdup(value);
	else if (!strcmp(name, "clone-prefix"))
		ctx.cfg.clone_prefix = xstrdup(value);
	else if (!strcmp(name, "clone-url"))
		ctx.cfg.clone_url = xstrdup(value);
	else if (!strcmp(name, "local-time"))
		ctx.cfg.local_time = atoi(value);
	else if (!strcmp(name, "commit-sort")) {
		if (!strcmp(value, "date"))
			ctx.cfg.commit_sort = 1;
		if (!strcmp(value, "topo"))
			ctx.cfg.commit_sort = 2;
	} else if (!strcmp(name, "branch-sort")) {
		if (!strcmp(value, "age"))
			ctx.cfg.branch_sort = 1;
		if (!strcmp(value, "name"))
			ctx.cfg.branch_sort = 0;
	} else if (!prefixcmp(name, "mimetype."))
		add_mimetype(name + 9, value);
	else if (!strcmp(name, "include"))
		parse_configfile(expand_macros(value), config_cb);
}

static void querystring_cb(const char *name, const char *value)
{
	if (!value)
		value = "";

	if (!strcmp(name,"r")) {
		ctx.qry.repo = xstrdup(value);
		ctx.repo = cgit_get_repoinfo(value);
	} else if (!strcmp(name, "p")) {
		ctx.qry.page = xstrdup(value);
	} else if (!strcmp(name, "url")) {
		if (*value == '/')
			value++;
		ctx.qry.url = xstrdup(value);
		cgit_parse_url(value);
	} else if (!strcmp(name, "qt")) {
		ctx.qry.grep = xstrdup(value);
	} else if (!strcmp(name, "q")) {
		ctx.qry.search = xstrdup(value);
	} else if (!strcmp(name, "h")) {
		ctx.qry.head = xstrdup(value);
		ctx.qry.has_symref = 1;
	} else if (!strcmp(name, "id")) {
		ctx.qry.sha1 = xstrdup(value);
		ctx.qry.has_sha1 = 1;
	} else if (!strcmp(name, "id2")) {
		ctx.qry.sha2 = xstrdup(value);
		ctx.qry.has_sha1 = 1;
	} else if (!strcmp(name, "ofs")) {
		ctx.qry.ofs = atoi(value);
	} else if (!strcmp(name, "path")) {
		ctx.qry.path = trim_end(value, '/');
	} else if (!strcmp(name, "name")) {
		ctx.qry.name = xstrdup(value);
	} else if (!strcmp(name, "mimetype")) {
		ctx.qry.mimetype = xstrdup(value);
	} else if (!strcmp(name, "s")) {
		ctx.qry.sort = xstrdup(value);
	} else if (!strcmp(name, "showmsg")) {
		ctx.qry.showmsg = atoi(value);
	} else if (!strcmp(name, "period")) {
		ctx.qry.period = xstrdup(value);
	} else if (!strcmp(name, "ss")) {
		ctx.qry.ssdiff = atoi(value);
		ctx.qry.has_ssdiff = 1;
	} else if (!strcmp(name, "all")) {
		ctx.qry.show_all = atoi(value);
	} else if (!strcmp(name, "context")) {
		ctx.qry.context = atoi(value);
	} else if (!strcmp(name, "ignorews")) {
		ctx.qry.ignorews = atoi(value);
	}
}

static void prepare_context(struct cgit_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->cfg.agefile = "info/web/last-modified";
	ctx->cfg.nocache = 0;
	ctx->cfg.cache_size = 0;
	ctx->cfg.cache_dynamic_ttl = 5;
	ctx->cfg.cache_max_create_time = 5;
	ctx->cfg.cache_repo_ttl = 5;
	ctx->cfg.cache_root = CGIT_CACHE_ROOT;
	ctx->cfg.cache_root_ttl = 5;
	ctx->cfg.cache_scanrc_ttl = 15;
	ctx->cfg.cache_static_ttl = -1;
	ctx->cfg.case_sensitive_sort = 1;
	ctx->cfg.branch_sort = 0;
	ctx->cfg.commit_sort = 0;
	ctx->cfg.css = "/cgit.css";
	ctx->cfg.logo = "/cgit.png";
	ctx->cfg.local_time = 0;
	ctx->cfg.enable_http_clone = 1;
	ctx->cfg.enable_index_owner = 1;
	ctx->cfg.enable_tree_linenumbers = 1;
	ctx->cfg.enable_git_config = 0;
	ctx->cfg.max_repo_count = 50;
	ctx->cfg.max_commit_count = 50;
	ctx->cfg.max_lock_attempts = 5;
	ctx->cfg.max_msg_len = 80;
	ctx->cfg.max_repodesc_len = 80;
	ctx->cfg.max_blob_size = 0;
	ctx->cfg.max_stats = 0;
	ctx->cfg.project_list = NULL;
	ctx->cfg.renamelimit = -1;
	ctx->cfg.remove_suffix = 0;
	ctx->cfg.robots = "index, nofollow";
	ctx->cfg.root_title = "Git repository browser";
	ctx->cfg.root_desc = "a fast webinterface for the git dscm";
	ctx->cfg.scan_hidden_path = 0;
	ctx->cfg.script_name = CGIT_SCRIPT_NAME;
	ctx->cfg.section = "";
	ctx->cfg.repository_sort = "name";
	ctx->cfg.section_sort = 1;
	ctx->cfg.summary_branches = 10;
	ctx->cfg.summary_log = 10;
	ctx->cfg.summary_tags = 10;
	ctx->cfg.max_atom_items = 10;
	ctx->cfg.ssdiff = 0;
	ctx->env.cgit_config = getenv("CGIT_CONFIG");
	ctx->env.http_host = getenv("HTTP_HOST");
	ctx->env.https = getenv("HTTPS");
	ctx->env.no_http = getenv("NO_HTTP");
	ctx->env.path_info = getenv("PATH_INFO");
	ctx->env.query_string = getenv("QUERY_STRING");
	ctx->env.request_method = getenv("REQUEST_METHOD");
	ctx->env.script_name = getenv("SCRIPT_NAME");
	ctx->env.server_name = getenv("SERVER_NAME");
	ctx->env.server_port = getenv("SERVER_PORT");
	ctx->page.mimetype = "text/html";
	ctx->page.charset = PAGE_ENCODING;
	ctx->page.filename = NULL;
	ctx->page.size = 0;
	ctx->page.modified = time(NULL);
	ctx->page.expires = ctx->page.modified;
	ctx->page.etag = NULL;
	memset(&ctx->cfg.mimetypes, 0, sizeof(struct string_list));
	if (ctx->env.script_name)
		ctx->cfg.script_name = xstrdup(ctx->env.script_name);
	if (ctx->env.query_string)
		ctx->qry.raw = xstrdup(ctx->env.query_string);
	if (!ctx->env.cgit_config)
		ctx->env.cgit_config = CGIT_CONFIG;
}

struct refmatch {
	char *req_ref;
	char *first_ref;
	int match;
};

static int find_current_ref(const char *refname, const unsigned char *sha1,
			    int flags, void *cb_data)
{
	struct refmatch *info;

	info = (struct refmatch *)cb_data;
	if (!strcmp(refname, info->req_ref))
		info->match = 1;
	if (!info->first_ref)
		info->first_ref = xstrdup(refname);
	return info->match;
}

static void free_refmatch_inner(struct refmatch *info)
{
	if (info->first_ref)
		free(info->first_ref);
}

static char *find_default_branch(struct cgit_repo *repo)
{
	struct refmatch info;
	char *ref;

	info.req_ref = repo->defbranch;
	info.first_ref = NULL;
	info.match = 0;
	for_each_branch_ref(find_current_ref, &info);
	if (info.match)
		ref = info.req_ref;
	else
		ref = info.first_ref;
	if (ref)
		ref = xstrdup(ref);
	free_refmatch_inner(&info);

	return ref;
}

static char *guess_defbranch(void)
{
	const char *ref;
	unsigned char sha1[20];

	ref = resolve_ref_unsafe("HEAD", sha1, 0, NULL);
	if (!ref || prefixcmp(ref, "refs/heads/"))
		return "master";
	return xstrdup(ref + 11);
}
/* The caller must free filename and ref after calling this. */
static inline void parse_readme(const char *readme, char **filename, char **ref, struct cgit_repo *repo)
{
	const char *colon;

	*filename = NULL;
	*ref = NULL;

	if (!readme || !readme[0])
		return;

	/* Check if the readme is tracked in the git repo. */
	colon = strchr(readme, ':');
	if (colon && strlen(colon) > 1) {
		/* If it starts with a colon, we want to use
		 * the default branch */
		if (colon == readme && repo->defbranch)
			*ref = xstrdup(repo->defbranch);
		else
			*ref = xstrndup(readme, colon - readme);
		readme = colon + 1;
	}

	/* Prepend repo path to relative readme path unless tracked. */
	if (!(*ref) && readme[0] != '/')
		*filename = fmtalloc("%s/%s", repo->path, readme);
	else
		*filename = xstrdup(readme);
}
static void choose_readme(struct cgit_repo *repo)
{
	int found;
	char *filename, *ref;
	struct string_list_item *entry;

	if (!repo->readme.nr)
		return;

	found = 0;
	for_each_string_list_item(entry, &repo->readme) {
		parse_readme(entry->string, &filename, &ref, repo);
		if (!filename) {
			free(filename);
			free(ref);
			continue;
		}
		/* If there's only one item, we skip the possibly expensive
		 * selection process. */
		if (repo->readme.nr == 1) {
			found = 1;
			break;
		}
		if (ref) {
			if (cgit_ref_path_exists(filename, ref, 1)) {
				found = 1;
				break;
			}
		}
		else if (!access(filename, R_OK)) {
			found = 1;
			break;
		}
		free(filename);
		free(ref);
	}
	repo->readme.strdup_strings = 1;
	string_list_clear(&repo->readme, 0);
	repo->readme.strdup_strings = 0;
	if (found)
		string_list_append(&repo->readme, filename)->util = ref;
}

static int prepare_repo_cmd(struct cgit_context *ctx)
{
	unsigned char sha1[20];
	int nongit = 0;
	int rc;

	/* The path to the git repository. */
	setenv("GIT_DIR", ctx->repo->path, 1);

	/* Do not look in /etc/ for gitconfig and gitattributes. */
	setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
	setenv("GIT_ATTR_NOSYSTEM", "1", 1);
	unsetenv("HOME");
	unsetenv("XDG_CONFIG_HOME");

	/* Setup the git directory and initialize the notes system. Both of these
	 * load local configuration from the git repository, so we do them both while
	 * the HOME variables are unset. */
	setup_git_directory_gently(&nongit);
	init_display_notes(NULL);

	if (nongit) {
		const char *name = ctx->repo->name;
		rc = errno;
		ctx->page.title = fmtalloc("%s - %s", ctx->cfg.root_title,
						"config error");
		ctx->repo = NULL;
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
		cgit_print_error("Failed to open %s: %s", name, rc ? strerror(rc) : "Not a valid git repository");
		cgit_print_docend();
		return 1;
	}
	ctx->page.title = fmtalloc("%s - %s", ctx->repo->name, ctx->repo->desc);

	if (!ctx->repo->defbranch)
		ctx->repo->defbranch = guess_defbranch();

	if (!ctx->qry.head) {
		ctx->qry.nohead = 1;
		ctx->qry.head = find_default_branch(ctx->repo);
	}

	if (!ctx->qry.head) {
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
		cgit_print_error("Repository seems to be empty");
		cgit_print_docend();
		return 1;
	}

	if (get_sha1(ctx->qry.head, sha1)) {
		char *tmp = xstrdup(ctx->qry.head);
		ctx->qry.head = ctx->repo->defbranch;
		ctx->page.status = 404;
		ctx->page.statusmsg = "Not found";
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
		cgit_print_error("Invalid branch: %s", tmp);
		cgit_print_docend();
		return 1;
	}
	sort_string_list(&ctx->repo->submodules);
	cgit_prepare_repo_env(ctx->repo);
	choose_readme(ctx->repo);
	return 0;
}

static void process_request(void *cbdata)
{
	struct cgit_context *ctx = cbdata;
	struct cgit_cmd *cmd;

	cmd = cgit_get_cmd(ctx);
	if (!cmd) {
		ctx->page.title = "cgit error";
		ctx->page.status = 404;
		ctx->page.statusmsg = "Not found";
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
		cgit_print_error("Invalid request");
		cgit_print_docend();
		return;
	}

	if (!ctx->cfg.enable_http_clone && cmd->is_clone) {
		html_status(404, "Not found", 0);
		return;
	}

	/* If cmd->want_vpath is set, assume ctx->qry.path contains a "virtual"
	 * in-project path limit to be made available at ctx->qry.vpath.
	 * Otherwise, no path limit is in effect (ctx->qry.vpath = NULL).
	 */
	ctx->qry.vpath = cmd->want_vpath ? ctx->qry.path : NULL;

	if (cmd->want_repo && !ctx->repo) {
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
		cgit_print_error("No repository selected");
		cgit_print_docend();
		return;
	}

	if (ctx->repo && prepare_repo_cmd(ctx))
		return;

	if (cmd->want_layout) {
		cgit_print_http_headers(ctx);
		cgit_print_docstart(ctx);
		cgit_print_pageheader(ctx);
	}

	cmd->fn(ctx);

	if (cmd->want_layout)
		cgit_print_docend();
}

static int cmp_repos(const void *a, const void *b)
{
	const struct cgit_repo *ra = a, *rb = b;
	return strcmp(ra->url, rb->url);
}

static char *build_snapshot_setting(int bitmap)
{
	const struct cgit_snapshot_format *f;
	struct strbuf result = STRBUF_INIT;

	for (f = cgit_snapshot_formats; f->suffix; f++) {
		if (f->bit & bitmap) {
			if (result.len)
				strbuf_addch(&result, ' ');
			strbuf_addstr(&result, f->suffix);
		}
	}
	return strbuf_detach(&result, NULL);
}

static char *get_first_line(char *txt)
{
	char *t = xstrdup(txt);
	char *p = strchr(t, '\n');
	if (p)
		*p = '\0';
	return t;
}

static void print_repo(FILE *f, struct cgit_repo *repo)
{
	struct string_list_item *item;
	fprintf(f, "repo.url=%s\n", repo->url);
	fprintf(f, "repo.name=%s\n", repo->name);
	fprintf(f, "repo.path=%s\n", repo->path);
	if (repo->owner)
		fprintf(f, "repo.owner=%s\n", repo->owner);
	if (repo->desc) {
		char *tmp = get_first_line(repo->desc);
		fprintf(f, "repo.desc=%s\n", tmp);
		free(tmp);
	}
	for_each_string_list_item(item, &repo->readme) {
		if (item->util)
			fprintf(f, "repo.readme=%s:%s\n", (char *)item->util, item->string);
		else
			fprintf(f, "repo.readme=%s\n", item->string);
	}
	if (repo->defbranch)
		fprintf(f, "repo.defbranch=%s\n", repo->defbranch);
	if (repo->module_link)
		fprintf(f, "repo.module-link=%s\n", repo->module_link);
	if (repo->section)
		fprintf(f, "repo.section=%s\n", repo->section);
	if (repo->clone_url)
		fprintf(f, "repo.clone-url=%s\n", repo->clone_url);
	fprintf(f, "repo.enable-commit-graph=%d\n",
	        repo->enable_commit_graph);
	fprintf(f, "repo.enable-log-filecount=%d\n",
	        repo->enable_log_filecount);
	fprintf(f, "repo.enable-log-linecount=%d\n",
	        repo->enable_log_linecount);
	if (repo->about_filter && repo->about_filter != ctx.cfg.about_filter)
		fprintf(f, "repo.about-filter=%s\n", repo->about_filter->cmd);
	if (repo->commit_filter && repo->commit_filter != ctx.cfg.commit_filter)
		fprintf(f, "repo.commit-filter=%s\n", repo->commit_filter->cmd);
	if (repo->source_filter && repo->source_filter != ctx.cfg.source_filter)
		fprintf(f, "repo.source-filter=%s\n", repo->source_filter->cmd);
	if (repo->snapshots != ctx.cfg.snapshots) {
		char *tmp = build_snapshot_setting(repo->snapshots);
		fprintf(f, "repo.snapshots=%s\n", tmp ? tmp : "");
		free(tmp);
	}
	if (repo->max_stats != ctx.cfg.max_stats)
		fprintf(f, "repo.max-stats=%s\n",
		        cgit_find_stats_periodname(repo->max_stats));
	if (repo->logo)
		fprintf(f, "repo.logo=%s\n", repo->logo);
	if (repo->logo_link)
		fprintf(f, "repo.logo-link=%s\n", repo->logo_link);
	fprintf(f, "repo.enable-remote-branches=%d\n", repo->enable_remote_branches);
	fprintf(f, "repo.enable-subject-links=%d\n", repo->enable_subject_links);
	if (repo->branch_sort == 1)
		fprintf(f, "repo.branch-sort=age\n");
	if (repo->commit_sort) {
		if (repo->commit_sort == 1)
			fprintf(f, "repo.commit-sort=date\n");
		else if (repo->commit_sort == 2)
			fprintf(f, "repo.commit-sort=topo\n");
	}
	fprintf(f, "\n");
}

static void print_repolist(FILE *f, struct cgit_repolist *list, int start)
{
	int i;

	for (i = start; i < list->count; i++)
		print_repo(f, &list->repos[i]);
}

/* Scan 'path' for git repositories, save the resulting repolist in 'cached_rc'
 * and return 0 on success.
 */
static int generate_cached_repolist(const char *path, const char *cached_rc)
{
	struct strbuf locked_rc = STRBUF_INIT;
	int result = 0;
	int idx;
	FILE *f;

	strbuf_addf(&locked_rc, "%s.lock", cached_rc);
	f = fopen(locked_rc.buf, "wx");
	if (!f) {
		/* Inform about the error unless the lockfile already existed,
		 * since that only means we've got concurrent requests.
		 */
		result = errno;
		if (result != EEXIST)
			fprintf(stderr, "[cgit] Error opening %s: %s (%d)\n",
				locked_rc.buf, strerror(result), result);
		goto out;
	}
	idx = cgit_repolist.count;
	if (ctx.cfg.project_list)
		scan_projects(path, ctx.cfg.project_list, repo_config);
	else
		scan_tree(path, repo_config);
	print_repolist(f, &cgit_repolist, idx);
	if (rename(locked_rc.buf, cached_rc))
		fprintf(stderr, "[cgit] Error renaming %s to %s: %s (%d)\n",
			locked_rc.buf, cached_rc, strerror(errno), errno);
	fclose(f);
out:
	strbuf_release(&locked_rc);
	return result;
}

static void process_cached_repolist(const char *path)
{
	struct stat st;
	struct strbuf cached_rc = STRBUF_INIT;
	time_t age;
	unsigned long hash;
	hash = hash_str(path);
	if (ctx.cfg.project_list)
		hash += hash_str(ctx.cfg.project_list);
	strbuf_addf(&cached_rc, "%s/rc-%8lx", ctx.cfg.cache_root, hash);

	if (stat(cached_rc.buf, &st)) {
		/* Nothing is cached, we need to scan without forking. And
		 * if we fail to generate a cached repolist, we need to
		 * invoke scan_tree manually.
		 */
		if (generate_cached_repolist(path, cached_rc.buf)) {
			if (ctx.cfg.project_list)
				scan_projects(path, ctx.cfg.project_list,
					      repo_config);
			else
				scan_tree(path, repo_config);
		}
		goto out;
	}

	parse_configfile(cached_rc.buf, config_cb);

	/* If the cached configfile hasn't expired, lets exit now */
	age = time(NULL) - st.st_mtime;
	if (age <= (ctx.cfg.cache_scanrc_ttl * 60))
		goto out;

	/* The cached repolist has been parsed, but it was old. So lets
	 * rescan the specified path and generate a new cached repolist
	 * in a child-process to avoid latency for the current request.
	 */
	if (fork())
		goto out;

	exit(generate_cached_repolist(path, cached_rc.buf));
out:
	strbuf_release(&cached_rc);
}

static void cgit_parse_args(int argc, const char **argv)
{
	int i;
	int scan = 0;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--cache=", 8)) {
			ctx.cfg.cache_root = xstrdup(argv[i] + 8);
		}
		if (!strcmp(argv[i], "--nocache")) {
			ctx.cfg.nocache = 1;
		}
		if (!strcmp(argv[i], "--nohttp")) {
			ctx.env.no_http = "1";
		}
		if (!strncmp(argv[i], "--query=", 8)) {
			ctx.qry.raw = xstrdup(argv[i] + 8);
		}
		if (!strncmp(argv[i], "--repo=", 7)) {
			ctx.qry.repo = xstrdup(argv[i] + 7);
		}
		if (!strncmp(argv[i], "--page=", 7)) {
			ctx.qry.page = xstrdup(argv[i] + 7);
		}
		if (!strncmp(argv[i], "--head=", 7)) {
			ctx.qry.head = xstrdup(argv[i] + 7);
			ctx.qry.has_symref = 1;
		}
		if (!strncmp(argv[i], "--sha1=", 7)) {
			ctx.qry.sha1 = xstrdup(argv[i] + 7);
			ctx.qry.has_sha1 = 1;
		}
		if (!strncmp(argv[i], "--ofs=", 6)) {
			ctx.qry.ofs = atoi(argv[i] + 6);
		}
		if (!strncmp(argv[i], "--scan-tree=", 12) ||
		    !strncmp(argv[i], "--scan-path=", 12)) {
			/* HACK: the global snapshot bitmask defines the
			 * set of allowed snapshot formats, but the config
			 * file hasn't been parsed yet so the mask is
			 * currently 0. By setting all bits high before
			 * scanning we make sure that any in-repo cgitrc
			 * snapshot setting is respected by scan_tree().
			 * BTW: we assume that there'll never be more than
			 * 255 different snapshot formats supported by cgit...
			 */
			ctx.cfg.snapshots = 0xFF;
			scan++;
			scan_tree(argv[i] + 12, repo_config);
		}
	}
	if (scan) {
		qsort(cgit_repolist.repos, cgit_repolist.count,
			sizeof(struct cgit_repo), cmp_repos);
		print_repolist(stdout, &cgit_repolist, 0);
		exit(0);
	}
}

static int calc_ttl()
{
	if (!ctx.repo)
		return ctx.cfg.cache_root_ttl;

	if (!ctx.qry.page)
		return ctx.cfg.cache_repo_ttl;

	if (ctx.qry.has_symref)
		return ctx.cfg.cache_dynamic_ttl;

	if (ctx.qry.has_sha1)
		return ctx.cfg.cache_static_ttl;

	return ctx.cfg.cache_repo_ttl;
}

int main(int argc, const char **argv)
{
	const char *path;
	int err, ttl;

	prepare_context(&ctx);
	cgit_repolist.length = 0;
	cgit_repolist.count = 0;
	cgit_repolist.repos = NULL;

	cgit_parse_args(argc, argv);
	parse_configfile(expand_macros(ctx.env.cgit_config), config_cb);
	ctx.repo = NULL;
	http_parse_querystring(ctx.qry.raw, querystring_cb);


	/* If virtual-root isn't specified in cgitrc, lets pretend
	 * that virtual-root equals SCRIPT_NAME, minus any possibly
	 * trailing slashes.
	 */
	if (!ctx.cfg.virtual_root && ctx.cfg.script_name)
		ctx.cfg.virtual_root = ensure_end(ctx.cfg.script_name, '/');

	/* If no url parameter is specified on the querystring, lets
	 * use PATH_INFO as url. This allows cgit to work with virtual
	 * urls without the need for rewriterules in the webserver (as
	 * long as PATH_INFO is included in the cache lookup key).
	 */
	path = ctx.env.path_info;
	if (!ctx.qry.url && path) {
		if (path[0] == '/')
			path++;
		ctx.qry.url = xstrdup(path);
		if (ctx.qry.raw) {
			char *newqry = fmtalloc("%s?%s", path, ctx.qry.raw);
			free(ctx.qry.raw);
			ctx.qry.raw = newqry;
		} else
			ctx.qry.raw = xstrdup(ctx.qry.url);
		cgit_parse_url(ctx.qry.url);
	}

	ttl = calc_ttl();
	ctx.page.expires += ttl * 60;
	if (ctx.env.request_method && !strcmp(ctx.env.request_method, "HEAD"))
		ctx.cfg.nocache = 1;
	if (ctx.cfg.nocache)
		ctx.cfg.cache_size = 0;
	err = cache_process(ctx.cfg.cache_size, ctx.cfg.cache_root,
			    ctx.qry.raw, ttl, process_request, &ctx);
	if (err)
		cgit_print_error("Error processing page: %s (%d)",
				 strerror(err), err);
	return err;
}
