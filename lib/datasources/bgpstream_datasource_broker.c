/*
 * This file is part of bgpstream
 *
 * Copyright (C) 2015 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * All rights reserved.
 *
 * This code has been developed by CAIDA at UC San Diego.
 * For more information, contact bgpstream-info@caida.org
 *
 * This source code is proprietary to the CAIDA group at UC San Diego and may
 * not be redistributed, published or disclosed without prior permission from
 * CAIDA.
 *
 * Report any bugs, questions or comments to bgpstream-info@caida.org
 *
 */

#include "config.h"

#include "bgpstream_datasource_broker.h"
#include "bgpstream_debug.h"
#include "utils.h"
#include "libjsmn/jsmn.h"

#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <wandio.h>

#define URL_BUFLEN 4096

// the max time we will wait between retries to the broker
#define MAX_WAIT_TIME 900

//indicates a fatal error
#define ERR_FATAL -1
// indicates a non-fatal error
#define ERR_RETRY -2

#define APPEND_STR(str)                                                 \
  do {                                                                  \
    size_t len = strlen(str);                                           \
    if(broker_ds->query_url_remaining < len+1)                          \
      {                                                                 \
        goto err;                                                       \
      }                                                                 \
    strncat(broker_ds->query_url_buf, str, broker_ds->query_url_remaining); \
    broker_ds->query_url_remaining -= len;                              \
  } while(0)


struct struct_bgpstream_broker_datasource_t {
  bgpstream_filter_mgr_t * filter_mgr;

  // working space to build query urls
  char query_url_buf[URL_BUFLEN];

  size_t query_url_remaining;

  // pointer to the end of the common query url (for appending last ts info)
  char *query_url_end;

  // have any parameters been added to the url?
  int first_param;

  // time of the last response we got from the broker
  uint32_t last_response_time;

  // the max (file_time + duration) that we have seen
  uint32_t current_window_end;
};

#define AMPORQ                                  \
  do {                                          \
    if (broker_ds->first_param) {               \
      APPEND_STR("?");                          \
      broker_ds->first_param = 0;               \
    } else {                                    \
      APPEND_STR("&");                          \
    }                                           \
  }                                             \
  while(0)

static int json_isnull(const char *json, jsmntok_t *tok) {
  if (tok->type == JSMN_PRIMITIVE &&
      strncmp("null", json + tok->start, tok->end - tok->start) == 0) {
    return 1;
  }
  return 0;
}

static int json_strcmp(const char *json, jsmntok_t *tok, const char *s) {
	if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
			strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 0;
	}
	return -1;
}

// NB: this ONLY replaces \/ with /
static void unescape_url(char *url) {
  char *p = url;

  while(*p != '\0') {
    if (*p == '\\' && *(p+1) == '/') {
      // copy the remainder of the string backward (ugh)
      memmove(p, p+1, strlen(p+1)+1);
    }
    p++;
  }
}

static jsmntok_t *json_skip(jsmntok_t *t)
{
  int i;
  jsmntok_t *s;
  switch(t->type)
    {
    case JSMN_PRIMITIVE:
    case JSMN_STRING:
      t++;
      break;
    case JSMN_OBJECT:
    case JSMN_ARRAY:
      s = t;
      t++; // move onto first key
      for(i=0; i<s->size; i++) {
        t = json_skip(t);
        if (s->type == JSMN_OBJECT) {
          t = json_skip(t);
        }
      }
    }

  return t;
}

#define json_str_assert(js, t, str)             \
  do {                                          \
    if (json_strcmp(js, t, str) != 0) {         \
      goto err;                                 \
    }                                           \
  } while(0)

#define json_type_assert(t, asstype)             \
  do {                                           \
    if (t->type != asstype) {                    \
      goto err;                                  \
    }                                            \
  } while(0)

#define NEXT_TOK t++

#define json_strcpy(dest, t, js)                         \
  do {                                                   \
    memcpy(dest, js+t->start, t->end  - t->start);       \
    dest[t->end - t->start] = '\0';                      \
  } while(0)

#define json_strtoul(dest, t)                                           \
  do {                                                                  \
    char intbuf[20];                                                    \
    char *endptr = NULL;                                                \
    assert(t->end - t->start < 20);                                     \
    strncpy(intbuf, js+t->start, t->end - t->start);                    \
    intbuf[t->end-t->start] = '\0';                                     \
    dest = strtoul(intbuf, &endptr, 10);       \
    if (*endptr != '\0') {                                              \
      goto err;                                                         \
    }                                                                   \
  } while(0)

static int process_json(bgpstream_broker_datasource_t *broker_ds,
                        bgpstream_input_mgr_t *input_mgr,
                        const char *js, jsmntok_t *root_tok, size_t count)
{
  int i, j, k;
  jsmntok_t *t = root_tok+1;

  int arr_len, obj_len;

  int time_set = 0;

  int num_results = 0;

  // per-file info
  char *url = NULL;
  size_t url_len = 0;
  int url_set = 0;
  char collector[BGPSTREAM_UTILS_STR_NAME_LEN];
  int collector_set = 0;
  char project[BGPSTREAM_UTILS_STR_NAME_LEN];
  int project_set = 0;
  char type[BGPSTREAM_UTILS_STR_NAME_LEN];
  int type_set = 0;
  uint32_t initial_time;
  int initial_time_set = 0;
  uint32_t duration;
  int duration_set = 0;

  if (count == 0) {
    fprintf(stderr, "ERROR: Empty JSON response from broker\n");
    return ERR_RETRY;
  }

  if (root_tok->type != JSMN_OBJECT) {
    goto err;
  }

  // iterate over the children of the root object
  for (i=0; i<root_tok->size; i++) {
    // all keys must be strings
    if (t->type != JSMN_STRING) {
      goto err;
    }
    if (json_strcmp(js, t, "time") == 0) {
      NEXT_TOK;
      json_type_assert(t, JSMN_PRIMITIVE);
      json_strtoul(broker_ds->last_response_time, t);
      time_set = 1;
      NEXT_TOK;
    } else if (json_strcmp(js, t, "type") == 0) {
      NEXT_TOK;
      json_str_assert(js, t, "data");
      NEXT_TOK;
    } else if (json_strcmp(js, t, "error") == 0) {
      NEXT_TOK;
      if (json_isnull(js, t) == 0) {  // i.e. there is an error set
        fprintf(stderr, "ERROR: Broker reported an error: %.*s\n",
                t->end - t->start, js+t->start);
        return ERR_FATAL;
      }
      NEXT_TOK;
    } else if (json_strcmp(js, t, "queryParameters") == 0) {
      NEXT_TOK;
      json_type_assert(t, JSMN_OBJECT);
      // skip over this object
      t = json_skip(t);
    } else if (json_strcmp(js, t, "data") == 0) {
      NEXT_TOK;
      json_type_assert(t, JSMN_OBJECT);
      NEXT_TOK;
      json_str_assert(js, t, "dumpFiles");
      NEXT_TOK;
      json_type_assert(t, JSMN_ARRAY);
      arr_len = t->size; // number of dump files
      NEXT_TOK; // first elem in array
      for (j=0; j<arr_len; j++) {
        json_type_assert(t, JSMN_OBJECT);
        obj_len = t->size;
        NEXT_TOK;

        url_set = 0;
        project_set = 0;
        collector_set = 0;
        type_set = 0;
        initial_time_set = 0;
        duration_set = 0;

        for (k=0; k<obj_len; k++) {
          if (json_strcmp(js, t, "urlType") == 0) {
            NEXT_TOK;
            if (json_strcmp(js, t, "simple") != 0) {
              // not yet supported?
              fprintf(stderr, "ERROR: Unsupported URL type '%.*s'\n",
                      t->end - t->start, js+t->start);
              return ERR_FATAL;
            }
            NEXT_TOK;
          } else if (json_strcmp(js, t, "url") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_STRING);
            if (url_len < (t->end - t->start + 1)) {
              url_len = t->end - t->start + 1;
              if ((url = realloc(url, url_len)) == NULL) {
                goto err;
              }
            }
            json_strcpy(url, t, js);
            unescape_url(url);
            url_set = 1;
            NEXT_TOK;
          } else if (json_strcmp(js, t, "project") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_STRING);
            json_strcpy(project, t, js);
            project_set = 1;
            NEXT_TOK;
          } else if (json_strcmp(js, t, "collector") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_STRING);
            json_strcpy(collector, t, js);
            collector_set = 1;
            NEXT_TOK;
          } else if (json_strcmp(js, t, "type") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_STRING);
            json_strcpy(type, t, js);
            type_set = 1;
            NEXT_TOK;
          } else if (json_strcmp(js, t, "initialTime") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_PRIMITIVE);
            json_strtoul(initial_time, t);
            initial_time_set = 1;
            NEXT_TOK;
          } else if (json_strcmp(js, t, "duration") == 0) {
            NEXT_TOK;
            json_type_assert(t, JSMN_PRIMITIVE);
            json_strtoul(duration, t);
            duration_set = 1;
            NEXT_TOK;
          } else {
            fprintf(stderr, "ERROR: Unknown field '%.*s'\n",
                    t->end - t->start, js+t->start);
            goto err;
          }
        }
        // file obj has been completely read
        if (url_set == 0 || project_set == 0 || collector_set == 0 ||
            type_set == 0 || initial_time_set == 0 || duration_set == 0) {
          fprintf(stderr, "ERROR: Invalid dumpFile record\n");
          return ERR_RETRY;
        }
        fprintf(stderr, "----------\n");
        fprintf(stderr, "URL: %s\n", url);
        fprintf(stderr, "Project: %s\n", project);
        fprintf(stderr, "Collector: %s\n", collector);
        fprintf(stderr, "Type: %s\n", type);
        fprintf(stderr, "InitialTime: %"PRIu32"\n", initial_time);
        fprintf(stderr, "Duration: %"PRIu32"\n", duration);

        // do we need to update our current_window_end?
        if (initial_time+duration > broker_ds->current_window_end) {
          broker_ds->current_window_end = (initial_time+duration);
        }

        if (bgpstream_input_mgr_push_sorted_input(input_mgr,
                                                  strdup(url),
                                                  strdup(project),
                                                  strdup(collector),
                                                  strdup(type),
                                                  initial_time,
                                                  duration) <= 0) {
          goto err;
        }

        num_results++;
      }
    }
    // TODO: handle unknown tokens
  }

  if (time_set == 0) {
    goto err;
  }

  free(url);

  return num_results;

 err:
  fprintf(stderr, "ERROR: Invalid JSON response received from broker\n");
  free(url);
  return ERR_RETRY;
}

static int read_json(bgpstream_broker_datasource_t *broker_ds,
                     bgpstream_input_mgr_t *input_mgr,
                     io_t *jsonfile)
{
  jsmn_parser p;
  jsmntok_t *tok;
  size_t tokcount = 128;

  int ret;
  char *js = NULL;
  size_t jslen = 0;
  #define BUFSIZE 1024
  char buf[BUFSIZE];

  // prepare parser
  jsmn_init(&p);

  // allocate some tokens to start
  if ((tok = malloc(sizeof(jsmntok_t) * tokcount)) == NULL) {
    return -1;
  }

  // slurp the whole file into a buffer
  while(1) {
    /* do a read */
    ret = wandio_read(jsonfile, buf, BUFSIZE);
    if (ret < 0) {
      fprintf(stderr, "ERROR: Reading from broker failed\n");
      return -1;
    }
    if (ret == 0) {
      // we're done
      break;
    }
    if ((js = realloc(js, jslen + ret + 1)) == NULL) {
      return -1;
    }
    strncpy(js+jslen, buf, ret);
    jslen += ret;
  }

  again:
  if ((ret = jsmn_parse(&p, js, jslen, tok, tokcount)) < 0) {
    if (ret == JSMN_ERROR_NOMEM) {
      tokcount *= 2;
      if ((tok = realloc(tok, sizeof(jsmntok_t) * tokcount)) == NULL) {
        return -1;
      }
      goto again;
    }
    if (ret == JSMN_ERROR_INVAL) {
      fprintf(stderr, "ERROR: Invalid character in JSON string\n");
      return -1;
    }
    fprintf(stderr, "ERROR: JSON parser returned %d\n", ret);
    return -1;
  }
  return process_json(broker_ds, input_mgr, js, tok, p.toknext);
}

bgpstream_broker_datasource_t *
bgpstream_broker_datasource_create(bgpstream_filter_mgr_t *filter_mgr,
                                   char * broker_url)
{

  bgpstream_debug("\t\tBSDS_BROKER: create broker_ds start");
  bgpstream_broker_datasource_t *broker_ds;

  if ((broker_ds = malloc_zero(sizeof(bgpstream_broker_datasource_t))) == NULL) {
    bgpstream_log_err("\t\tBSDS_BROKER: create broker_ds can't allocate memory");
    goto err;
  }
  if (broker_url == NULL)
    {
      bgpstream_log_err("\t\tBSDS_BROKER: create broker_ds no file provided");
      goto err;
    }
  broker_ds->filter_mgr = filter_mgr;
  broker_ds->first_param = 1;
  broker_ds->query_url_remaining = URL_BUFLEN;
  broker_ds->query_url_buf[0] = '\0';

  // http://bgpstream.caida.org/broker (e.g.)
  APPEND_STR(broker_url);

  // http://bgpstream.caida.org/broker/data?
  APPEND_STR("/data");

  // projects, collectors, bgp_types, and time_intervals are used as filters
  // only if they are provided by the user
  bgpstream_string_filter_t * sf;
  bgpstream_interval_filter_t * tif;

  // projects
  if(filter_mgr->projects != NULL) {
    sf = filter_mgr->projects;
    while(sf != NULL) {
      AMPORQ;
      APPEND_STR("projects[]=");
      APPEND_STR(sf->value);
      sf = sf->next;
    }
  }
  // collectors
  if(filter_mgr->collectors != NULL) {
    sf = filter_mgr->collectors;
    while(sf != NULL) {
      AMPORQ;
      APPEND_STR("collectors[]=");
      APPEND_STR(sf->value);
      sf = sf->next;
    }
  }
  // bgp_types
  if(filter_mgr->bgp_types != NULL) {
    sf = filter_mgr->bgp_types;
    while(sf != NULL) {
      AMPORQ;
      APPEND_STR("types[]=");
      APPEND_STR(sf->value);
      sf = sf->next;
    }
  }

  // time_intervals
  #define BUFLEN 20
  char int_buf[BUFLEN];
  if(filter_mgr->time_intervals != NULL) {
    tif = filter_mgr->time_intervals;

    while(tif != NULL) {
      AMPORQ;
      APPEND_STR("intervals[]=");

      // BEGIN TIME
      if(snprintf(int_buf, BUFLEN, "%"PRIu32, tif->begin_time) >= BUFLEN)
        {
          goto err;
        }
      APPEND_STR(int_buf);
      APPEND_STR(",");

      // END TIME
      if(snprintf(int_buf, BUFLEN, "%"PRIu32, tif->end_time) >= BUFLEN)
        {
          goto err;
        }
      APPEND_STR(int_buf);

      tif = tif->next;
    }
  }

  // grab pointer to the end of the current string to simplify modifying the
  // query later
  broker_ds->query_url_end =
    broker_ds->query_url_buf + strlen(broker_ds->query_url_buf);
  assert(broker_ds->query_url_end == '\0');

  bgpstream_debug("\t\tBSDS_BROKER: create broker_ds end");

  return broker_ds;
 err:
  bgpstream_broker_datasource_destroy(broker_ds);
  return NULL;
}

int
bgpstream_broker_datasource_update_input_queue(bgpstream_broker_datasource_t* broker_ds,
                                               bgpstream_input_mgr_t *input_mgr)
{

  // we need to set two parameters:
  //  - dataAddedSince ("time" from last response we got)
  //  - minInitialTime (max("initialTime"+"duration") of any file we've ever seen)

  #define BUFLEN 20
  char buf[BUFLEN];

  io_t *jsonfile;

  int num_results;

  int attempts = 0;
  int wait_time = 1;

  if (broker_ds->last_response_time > 0) {
    // need to add dataAddedSince
    if (snprintf(buf, BUFLEN, "%"PRIu32, broker_ds->last_response_time)
        >= BUFLEN) {
      return -1;
    }
    AMPORQ;
    APPEND_STR("dataAddedSince=");
    APPEND_STR(buf);
  }

  if (broker_ds->current_window_end > 0) {
    // need to add minInitialTime
    if (snprintf(buf, BUFLEN, "%"PRIu32, broker_ds->current_window_end)
        >= BUFLEN) {
      return -1;
    }
    AMPORQ;
    APPEND_STR("minInitialTime=");
    APPEND_STR(buf);
  }

 retry:
  if (attempts > 0) {
    fprintf(stderr,
            "WARN: Broker request failed, waiting %ds before retry\n",
            wait_time);
    sleep(wait_time);
    if (wait_time < MAX_WAIT_TIME) {
      wait_time *= 2;
    }
  }
  attempts++;

  fprintf(stderr, "Query URL: \"%s\"\n", broker_ds->query_url_buf);

  if ((jsonfile = wandio_create(broker_ds->query_url_buf)) == NULL) {
    fprintf(stderr, "ERROR: Could not open %s for reading\n",
            broker_ds->query_url_buf);
    goto retry;
  }

  if ((num_results = read_json(broker_ds, input_mgr, jsonfile)) == ERR_RETRY) {
    goto retry;
  } else if (num_results == ERR_FATAL) {
    goto err;
  }

  wandio_destroy(jsonfile);

  // reset the variable params
  *broker_ds->query_url_end = '\0';
  return num_results;

 err:
  fprintf(stderr, "ERROR: Fatal error in broker data source\n");
  return -1;
}


void
bgpstream_broker_datasource_destroy(bgpstream_broker_datasource_t* broker_ds)
{
  if(broker_ds == NULL)
    {
      return;
    }

  free(broker_ds);
}