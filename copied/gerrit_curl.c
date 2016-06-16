#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <jansson.h>

#include "scan-tree.h"
#include "cgit.h"
#include "gerrit_curl.h"

size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  MemoryStruct *mem = (MemoryStruct *)userp;

  mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL) {
    /* out of memory! */
    fprintf(stderr, "not enough memory (realloc returned NULL)\n");
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}
size_t WriteMemoryCallback2(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  MemoryStruct *mem = (MemoryStruct *)userp;

  mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL) {
    /* out of memory! */
    fprintf(stderr, "not enough memory (realloc returned NULL)\n");
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

int get_list(struct cgit_context *ctx,char *remote_user,  MemoryStruct *chunk) {
	CURLcode res;
	CURL *curl;
	curl = curl_easy_init();
	struct curl_slist * headers = NULL;
	int ret = 0;
	headers = curl_slist_append(headers, remote_user);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, ctx->cfg.gerrit_project_list_url);
	//curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_COOKIE,getenv("HTTP_COOKIE") );
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
	res = curl_easy_perform(curl);
	if(res != CURLE_OK) {
		fprintf(stderr, "error: get_login curl_easy_perform() failed: %s url: %s\n", curl_easy_strerror(res), ctx->cfg.gerrit_project_list_url);
		ret = -1;
	}
	else {
#ifdef MYDEBUG
		fprintf(stderr, "DEBUG curl_easy_perform data:%s\n", chunk->memory);
#endif
		if( strcmp(chunk->memory, "Unauthorized") == 0) {
			ret = 1;
		}
	}
    curl_easy_cleanup(curl);
	return ret;	
}

int get_login(struct cgit_context *ctx, char *remote_user, MemoryStruct *chunk) {
	CURLcode res;
	CURL *curl;
	curl = curl_easy_init();
	struct curl_slist * headers=NULL;
	headers = curl_slist_append(headers, remote_user);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, ctx->cfg.gerrit_login_url);
	//curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_COOKIE,getenv("HTTP_COOKIE") );
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
	curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
	//curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteMemoryCallback2);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, (void *)chunk);
	res = curl_easy_perform(curl);
#if MYDEBUG
	fprintf(stderr,"DEBUG: login response head ----> \n%s\n",chunk->memory);
	fprintf(stderr,"DEBUG: <-- login response head \n"); 
#endif
	int ret = 0;
	if(res != CURLE_OK) {
		fprintf(stderr, "error: get_login curl_easy_perform() failed: %s url: %s\n", curl_easy_strerror(res), ctx->cfg.gerrit_login_url);
	  ret = -1;
	}
	else {	
		char * line =  NULL;
		line = strtok(chunk->memory, "\n");
		int cookcnt = 0;
		char gerrit_cookie_name[] = "Set-Cookie: GerritAccount=";
		while(line != NULL) {
#ifdef MYDEBUG
			fprintf(stderr,"DEBUG line by line ----> %s\n",line);
#endif
			int ret2 = memcmp(line, gerrit_cookie_name ,sizeof(gerrit_cookie_name) -1 ); 
			if(ret2 == 0) {
#ifdef MYDEBUG
				fprintf(stderr,"DEBUG Gerrit Cookie found...\n");
#endif
				htmlf("%s\n", line);
				cookcnt++;
			}
			line = strtok(NULL,"\n");
		}
#ifdef MYDEBUG
		fprintf(stderr,"DEBUG: cookcnt:%d\n",cookcnt);
#endif
		if(cookcnt == 0) {
			ret = -1;
		}
	}
    curl_easy_cleanup(curl);

	return ret;
}
int gerrit_get_project_list(char *value, struct cgit_context *ctx, repo_config_fn repo_config) {
#ifdef MYDEBUG
  fprintf(stderr, "DEBUG gerrit_get_project_list start\n");
  fprintf(stderr,"DEBUG: REQUEST_URI:%s\n", getenv("REQUEST_URI"));
  fprintf(stderr, "DEBUG url:%s\n", ctx->cfg.gerrit_project_list_url);
#endif
  char * tmp_remote_user = getenv("REMOTE_USER");
  if( tmp_remote_user == NULL ) {
    tmp_remote_user = getenv("HTTP_X_FORWARDED_USER");
  }

  if( tmp_remote_user == NULL) {
    fprintf(stderr,"REMOET_USER or HTTP_X_FORWARDED_USER is NULL exit...\n");
    return -1;
  }
  char * remote_user = (char *)malloc( strlen(tmp_remote_user) + 1000);
  sprintf(remote_user,"REMOTE_USER: %s", tmp_remote_user );

#ifdef MYDEBUG
	fprintf(stderr,"DEBUG COOKIE:%s\n", getenv("HTTP_COOKIE")); 
#endif
	MemoryStruct chunk;
	chunk.memory = (char *)malloc(1);
	chunk.size = 0;
	
	/*	
	MemoryStruct chunk2;
	chunk2.memory = (char *)malloc(1);
	chunk2.size = 0;
	*/	

	bool exitflag = false;
	int ret = get_list(ctx, remote_user, &chunk);
	if( ret == 0) {
		const char * strjson = strstr((const char *)chunk.memory, "{");
		if( strjson == NULL) {
			fprintf(stderr,"error: failed to get json string\n");
		}
		gerrit_scan_projects(value, strjson, repo_config);
	}
	else if( ret == 1) {
		htmlf("%s\n", "Content-Type: text/html;charset=utf-8");
		htmlf("%s\n", "Content-Length: 0");
		//int ret2 = get_login(ctx, remote_user, &chunk2);
		int ret2 = get_login(ctx, remote_user, &chunk);
		if(ret2 == 0) {
#ifdef MYDEBUG
			fprintf(stderr, "DEBUG going to ctx->cfg.gerrit_cgit_url: %s\n", ctx->cfg.gerrit_cgit_url);
#endif
			htmlf("%s\n", "Referer: cgit-redirect");
			htmlf("%s%s\n", "Location: ",ctx->cfg.gerrit_cgit_url);
		}
		else {
#ifdef MYDEBUG
			fprintf(stderr, "DEBUG going to ctx->cfg.gerrit_index_url : %s\n", ctx->cfg.gerrit_index_url);
#endif
			htmlf("%s%s\n", "Location: ", ctx->cfg.gerrit_index_url);
		}
		htmlf("\n");
		exitflag  = true;
	}
	else if(ret == -1) {
	}

	//cleanup
	free(remote_user);
	free(chunk.memory);
	//free(chunk2.memory);


	if(exitflag) {
		exit(0);
	}
	return 0;
}


int parse_json(const char *data) {
    json_error_t error;
    json_t * root = json_loads(data, 0, &error);
     
	if(!root) {
		fprintf(stderr, "error: on line %d: %s\n", error.line, error.text);
		return 1;
	}

    const char *key;
    json_t *value;

    void *iter = json_object_iter(root);
    while(iter)
    {
        key = json_object_iter_key(iter);
        value = json_object_iter_value(iter);
        fprintf(stderr, "CHERRY key:%s\n", key);
        if(json_is_object(value)) {
        }
        iter = json_object_iter_next(root, iter);
    } 
	return 0;
}

