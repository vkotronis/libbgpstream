/*
 * Copyright (C) 2016 The Regents of the University of California.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Shane Alcock <salcock@waikato.ac.nz>
 */

#include "bgpstream_filter_parser.h"
#include "bgpstream_log.h"
#include "bgpstream_filter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *bgpstream_filter_type_to_string(bgpstream_filter_type_t type)
{
  switch (type) {
  case BGPSTREAM_FILTER_TYPE_RECORD_TYPE:
    return "Record Type";
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_MORE:
    return "Prefix (or more specific)";
  case BGPSTREAM_FILTER_TYPE_ELEM_COMMUNITY:
    return "Community";
  case BGPSTREAM_FILTER_TYPE_ELEM_PEER_ASN:
    return "Peer ASN";
  case BGPSTREAM_FILTER_TYPE_PROJECT:
    return "Project";
  case BGPSTREAM_FILTER_TYPE_COLLECTOR:
    return "Collector";
  case BGPSTREAM_FILTER_TYPE_ROUTER:
    return "Router";
  case BGPSTREAM_FILTER_TYPE_ELEM_ASPATH:
    return "AS Path";
  case BGPSTREAM_FILTER_TYPE_ELEM_EXTENDED_COMMUNITY:
    return "Extended Community";
  case BGPSTREAM_FILTER_TYPE_ELEM_IP_VERSION:
    return "IP Version";
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_ANY:
    return "Prefix (of any specificity)";
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_LESS:
    return "Prefix (or less specific)";
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_EXACT:
    return "Prefix (exact match)";
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX:
    return "Prefix (old format)";
  case BGPSTREAM_FILTER_TYPE_ELEM_TYPE:
    return "Element Type";
  }

  return "Unknown filter term ??";
}

static void instantiate_filter(bgpstream_t *bs, bgpstream_filter_item_t *item)
{

  bgpstream_filter_type_t usetype = item->termtype;

  switch (item->termtype) {
  case BGPSTREAM_FILTER_TYPE_RECORD_TYPE:
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_MORE:
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_LESS:
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_ANY:
  case BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_EXACT:
  case BGPSTREAM_FILTER_TYPE_ELEM_COMMUNITY:
  case BGPSTREAM_FILTER_TYPE_ELEM_PEER_ASN:
  case BGPSTREAM_FILTER_TYPE_PROJECT:
  case BGPSTREAM_FILTER_TYPE_COLLECTOR:
  case BGPSTREAM_FILTER_TYPE_ROUTER:
  case BGPSTREAM_FILTER_TYPE_ELEM_ASPATH:
  case BGPSTREAM_FILTER_TYPE_ELEM_IP_VERSION:
  case BGPSTREAM_FILTER_TYPE_ELEM_TYPE:
    bgpstream_log(BGPSTREAM_LOG_FINE, "Added filter for %s", item->value);
    bgpstream_add_filter(bs, usetype, item->value);
    break;

  default:
    bgpstream_log(BGPSTREAM_LOG_ERR,
                  "Implementation of filter type %s is still to come!",
                  bgpstream_filter_type_to_string(item->termtype));
    break;
  }
}

static int bgpstream_parse_filter_term(char *term, fp_state_t *state,
                                       bgpstream_filter_item_t *curr)
{

  /* Painful list of strcmps... */
  /* TODO, can we save some time by checking the first character to rule
   * out most of the terms quickly?? */

  if (strcmp(term, "project") == 0 || strcmp(term, "proj") == 0) {
    /* Project */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a project term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_PROJECT;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "collector") == 0 || strcmp(term, "coll") == 0) {
    /* Collector */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a collector term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_COLLECTOR;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "router") == 0 || strcmp(term, "rout") == 0) {
    /* Router */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a router term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ROUTER;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "type") == 0) {
    /* Type */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a type term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_RECORD_TYPE;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "peer") == 0) {
    /* Peer */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a peer term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PEER_ASN;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "prefix") == 0 || strcmp(term, "pref") == 0) {
    /* prefix */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a prefix term");
    /* XXX is this the best default? */
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_MORE;
    *state = PREFIXEXT;
    return *state;
  }

  if (strcmp(term, "community") == 0 || strcmp(term, "comm") == 0) {
    /* Community */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a community term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_COMMUNITY;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "aspath") == 0 || strcmp(term, "path") == 0) {
    /* AS Path */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an aspath term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_ASPATH;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "extcommunity") == 0 || strcmp(term, "extc") == 0) {
    /* Extended Community */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a extended community term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_EXTENDED_COMMUNITY;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "ipversion") == 0 || strcmp(term, "ipv") == 0) {
    /* IP version */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got a ip version term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_IP_VERSION;
    *state = VALUE;
    return *state;
  }

  if (strcmp(term, "elemtype") == 0) {
    /* Element type */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an element type term");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_TYPE;
    *state = VALUE;
    return *state;
  }

  bgpstream_log(BGPSTREAM_LOG_ERR, "Expected a valid term, got %s", term);
  *state = FAIL;
  return FAIL;
}

static int bgpstream_parse_quotedvalue(char *value, fp_state_t *state,
                                       bgpstream_filter_item_t *curr)
{

  char *endquote = NULL;

  /* Check for end quote */
  endquote = strchr(value, '"');

  if (endquote != NULL) {
    *endquote = '\0';
    *state = ENDVALUE;
  }

  if (strlen(value) == 0)
    return *state;

  if (curr->value == NULL) {
    curr->value = strdup(value);
  } else {
    /* Append this part of the value to whatever we've already got */
    /* +2 = 1 byte for space, 1 byte for null */
    curr->value = realloc(curr->value, strlen(curr->value) + strlen(value) + 2);
    assert(curr->value);
    strncat(curr->value, " ", 1);
    strncat(curr->value, value, strlen(value));
  }

  if (*state == ENDVALUE) {
    /* Create our new filter here? */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Set our quoted value to %s", curr->value);
  }

  return *state;
}

static int bgpstream_parse_value(char *value, fp_state_t *state,
                                 bgpstream_filter_item_t *curr)
{

  /* Check for a quote at the start of the item */
  if (*value == '"') {
    *state = QUOTEDVALUE;
    return bgpstream_parse_quotedvalue(&(value[1]), state, curr);
  }

  /* If no quote, assume a single word value */
  /* XXX How intelligent do we want to be in terms of validating input? */
  curr->value = strdup(value);
  *state = ENDVALUE;

  /* At this point we can probably create our new filter */
  /* XXX this may not be true once we get around to OR support... */
  bgpstream_log(BGPSTREAM_LOG_FINE, "Set our unquoted value to %s", curr->value);

  return *state;
}

static int bgpstream_parse_prefixext(char *ext, fp_state_t *state,
                                     bgpstream_filter_item_t *curr)
{

  assert(curr->termtype == BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_MORE);

  if (strcmp(ext, "any") == 0) {
    /* Any prefix that our prefix belongs to */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an 'any' prefix");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_ANY;
    *state = VALUE;
    return *state;
  }

  if (strcmp(ext, "more") == 0) {
    /* Either match this prefix or any more specific prefixes */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an 'more' prefix");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_MORE;
    *state = VALUE;
    return *state;
  }

  if (strcmp(ext, "less") == 0) {
    /* Either match this prefix or any less specific prefixes */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an 'less' prefix");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_LESS;
    *state = VALUE;
    return *state;
  }

  if (strcmp(ext, "exact") == 0) {
    /* Only match exactly this prefix */
    bgpstream_log(BGPSTREAM_LOG_FINE, "Got an 'exact' prefix");
    curr->termtype = BGPSTREAM_FILTER_TYPE_ELEM_PREFIX_EXACT;
    *state = VALUE;
    return *state;
  }

  /* At this point, assume we're looking at a value instead */
  return bgpstream_parse_value(ext, state, curr);
}

static int bgpstream_parse_endvalue(char *conj, fp_state_t *state,
                                    bgpstream_filter_item_t **curr)
{

  if (*curr) {
    free((*curr)->value);
    free(*curr);
    *curr = NULL;
  }

  /* Check for a valid conjunction */
  if (strcmp(conj, "and") == 0) {
    *state = TERM;
  }

  /* TODO allow 'or', anything else? */

  if (*state != TERM) {
    bgpstream_log(BGPSTREAM_LOG_ERR,
                  "Bad conjunction in bgpstream filter string: %s", conj);
    *state = FAIL;
    return FAIL;
  }

  *curr = (bgpstream_filter_item_t *)calloc(1, sizeof(bgpstream_filter_item_t));

  return *state;
}

int bgpstream_parse_filter_string(bgpstream_t *bs, const char *fstring)
{

  char *tok;
  char *sptr = NULL;
  int ret = 1;

  bgpstream_log(BGPSTREAM_LOG_FINE, "Parsing filter string - %s", fstring);
  bgpstream_filter_item_t *filteritem;
  fp_state_t state = TERM;

  tok = strtok_r((char *)fstring, (char *)" ", &sptr);

  filteritem =
    (bgpstream_filter_item_t *)calloc(1, sizeof(bgpstream_filter_item_t));

  while (tok != NULL) {

    switch (state) {
    case TERM:
      if (bgpstream_parse_filter_term(tok, &state, filteritem) == FAIL) {
        ret = 0;
        goto endparsing;
      }
      break;

    case PREFIXEXT:
      if (bgpstream_parse_prefixext(tok, &state, filteritem) == FAIL) {
        ret = 0;
        goto endparsing;
      }
      if (state == ENDVALUE) {
        instantiate_filter(bs, filteritem);
      }
      break;

    case VALUE:
      if (bgpstream_parse_value(tok, &state, filteritem) == FAIL) {
        ret = 0;
        goto endparsing;
      }
      instantiate_filter(bs, filteritem);
      break;

    case QUOTEDVALUE:
      if (bgpstream_parse_quotedvalue(tok, &state, filteritem) == FAIL) {
        ret = 0;
        goto endparsing;
      }
      if (state == ENDVALUE) {
        instantiate_filter(bs, filteritem);
      }
      break;

    case ENDVALUE:
      if (bgpstream_parse_endvalue(tok, &state, &filteritem) == FAIL) {
        ret = 0;
        goto endparsing;
      }
      break;

    default:
      bgpstream_log(BGPSTREAM_LOG_ERR,
                    "Unexpected BGPStream filter string state: %d", state);
      ret = 0;
      goto endparsing;
    }
    tok = strtok_r(NULL, " ", &sptr);
  }

endparsing:

  if (filteritem) {
    if (filteritem->value) {
      free(filteritem->value);
    }
    free(filteritem);
  }

  bgpstream_log(BGPSTREAM_LOG_FINE, "Finished parsing filter string");
  return ret;
}
