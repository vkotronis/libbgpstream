/*
 * bgpcorsaro
 *
 * Chiara Orsini, CAIDA, UC San Diego
 * corsaro-info@caida.org
 *
 * Copyright (C) 2015 The Regents of the University of California.
 *
 * This file is part of bgpcorsaro.
 *
 * bgpcorsaro is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bgpcorsaro is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with bgpcorsaro.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "utils.h"

#include "routingtables.h"
#include "routingtables_int.h"


/** When the Quagga process starts dumping the 
 *  RIB (at time t0), not all of the previous update
 *  messages have been processed, in other words
 *  there is a backlog queue of update that has not
 *  been processed yet, when the updates in this queue 
 *  refer to timestamps before the RIB, then considering
 *  the RIB state as the most updated leads to wrong conclusions,
 *  as well as the installation of stale routes in the routing table.
 *  To prevent this case, we say that: if an update message applied
 *  to our routing table is older than the timestamp of the UC RIB
 *  and the update happened within ROUTINGTABLES_RIB_BACKLOG_TIME from
 *  the RIB start, then the update message is the one which is
 *  considered the more consistent (and therefore it should remain
 *  in the routing table after the end_of_rib process). */
#define ROUTINGTABLES_RIB_BACKLOG_TIME 60

/** If a peer does not receive any data for
 *  ROUTINGTABLES_MAX_INACTIVE_TIME and it is not
 *  in the RIB, then it is considered UNKNOWN */ 
#define ROUTINGTABLES_MAX_INACTIVE_TIME 3600


/** ROUTINGTABLES_LOCAL_*_ASN is a set of constants 
 *  that is used to give special meaning to the origin
 *  AS field, all the values above ROUTINGTABLES_RESERVED_ASN_START
 *  are part of IANA reserved space for AS numbers, therefore
 *  no valid origin should be confused with these constants
 *  (unless an attacker actually uses them to forge the path).
 *  Ref: http://www.iana.org/assignments/as-numbers/as-numbers.xhtml
 */
#define ROUTINGTABLES_RESERVED_ASN_START BGPWATCHER_VIEW_ASN_NOEXPORT_START
#define ROUTINGTABLES_LOCAL_ORIGIN_ASN   ROUTINGTABLES_RESERVED_ASN_START + 0
#define ROUTINGTABLES_CONFSET_ORIGIN_ASN ROUTINGTABLES_RESERVED_ASN_START + 1
#define ROUTINGTABLES_DOWN_ORIGIN_ASN    ROUTINGTABLES_RESERVED_ASN_START + 2


/** string buffer to contain prefixes and ip addresses 
 *  used for debugging purposes only */
static char buffer[INET6_ADDRSTRLEN+3];


/* ========== PRIVATE FUNCTIONS ========== */

static char *
graphite_safe(char *p)
{
  if(p == NULL)
    {
      return p;
    }

  char *r = p;
  while(*p != '\0')
    {
      if(*p == '.')
	{
	  *p = '-';
	}
      if(*p == '*')
	{
	  *p = '-';
	}
      p++;
    }
  return r;
}

static uint32_t
get_wall_time_now()
{
  struct timeval tv;
  gettimeofday_wrap(&tv);
  return tv.tv_sec;
}

static int filter_ff_peers(bgpwatcher_view_iter_t *iter)
{
  return (
          (bgpwatcher_view_iter_peer_get_pfx_cnt(iter,
                                                BGPSTREAM_ADDR_VERSION_IPV4,
                                                BGPWATCHER_VIEW_FIELD_ACTIVE)
           >= ((rt_view_data_t *)bgpwatcher_view_get_user(bgpwatcher_view_iter_get_view(iter)))->ipv4_fullfeed_th) ||
          (bgpwatcher_view_iter_peer_get_pfx_cnt(iter,
                                                 BGPSTREAM_ADDR_VERSION_IPV6,
                                                 BGPWATCHER_VIEW_FIELD_ACTIVE)
           >= ((rt_view_data_t *)bgpwatcher_view_get_user(bgpwatcher_view_iter_get_view(iter)))->ipv6_fullfeed_th));
}


/** Returns the origin AS when the origin AS number
 *  numeric, it returns 65535 when the origin is
 *  either a set or a confederation 
 *
 *  @param aspath a pointer to a RIB or ANNOUNCEMENT aspath
 *  @return the origin AS number
 */
static uint32_t
get_origin_asn(bgpstream_as_path_t *aspath)
{
  uint32_t asn = 0;
  bgpstream_as_path_seg_t *seg =
    bgpstream_as_path_get_origin_seg(aspath);

  if(seg == NULL)
    {
      asn = 0; /* empty path */
    }
  else if(seg->type == BGPSTREAM_AS_PATH_SEG_ASN)
    {
      asn = ((bgpstream_as_path_seg_asn_t*)seg)->asn;
    }
  else
    {
      /* use a reserved AS number to indicate
       * a set/confederation */
      asn = ROUTINGTABLES_CONFSET_ORIGIN_ASN;
    }
  if(asn == 0)
    {
      asn = ROUTINGTABLES_LOCAL_ORIGIN_ASN;
    }
  return asn;
}

static perpfx_perpeer_info_t *
perpfx_perpeer_info_create()
{
  perpfx_perpeer_info_t *pfxpeeri = (perpfx_perpeer_info_t *) malloc_zero(sizeof(perpfx_perpeer_info_t));
  if(pfxpeeri != NULL)
    {
      pfxpeeri->bgp_time_last_ts = 0;
      pfxpeeri->bgp_time_uc_delta_ts = 0;
      pfxpeeri->uc_origin_asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;
      pfxpeeri->announcements = 0;
      pfxpeeri->withdrawals = 0;
    }
  return pfxpeeri;
}


static void
perpeer_info_destroy(void *p)
{
  if(p != NULL)
    {
      if(((perpeer_info_t *)p)->announcing_ases != NULL)
        {
          bgpstream_id_set_destroy(((perpeer_info_t *)p)->announcing_ases);
          ((perpeer_info_t *)p)->announcing_ases = NULL;
        }
      if(((perpeer_info_t *)p)->announced_v4_pfxs != NULL)
        {
          bgpstream_ipv4_pfx_set_destroy(((perpeer_info_t *)p)->announced_v4_pfxs);
          ((perpeer_info_t *)p)->announced_v4_pfxs = NULL;
        }
      if(((perpeer_info_t *)p)->withdrawn_v4_pfxs != NULL)
        {
          bgpstream_ipv4_pfx_set_destroy(((perpeer_info_t *)p)->withdrawn_v4_pfxs);
          ((perpeer_info_t *)p)->withdrawn_v4_pfxs = NULL;
        }
      if(((perpeer_info_t *)p)->announced_v6_pfxs != NULL)
        {
          bgpstream_ipv6_pfx_set_destroy(((perpeer_info_t *)p)->announced_v6_pfxs);
          ((perpeer_info_t *)p)->announced_v6_pfxs = NULL;
        }
      if(((perpeer_info_t *)p)->withdrawn_v6_pfxs != NULL)
        {
          bgpstream_ipv6_pfx_set_destroy(((perpeer_info_t *)p)->withdrawn_v6_pfxs);
          ((perpeer_info_t *)p)->withdrawn_v6_pfxs = NULL;
        }
      free(p);
    }
}


/* default: all ts are 0, while the peer state is BGPSTREAM_ELEM_PEERSTATE_UNKNOWN */
static perpeer_info_t *
perpeer_info_create(routingtables_t *rt, collector_t * c,
                    uint32_t peer_id)
{
  char ip_str[INET6_ADDRSTRLEN];
  uint8_t v = 0;
  perpeer_info_t *p;
  if((p = (perpeer_info_t *) malloc_zero(sizeof(perpeer_info_t))) == NULL)
    {
      fprintf(stderr, "Error: can't create per-peer info\n");
      goto err;
    }

  strcpy(p->collector_str,c->collector_str);

  bgpstream_peer_sig_t *sg = bgpstream_peer_sig_map_get_sig(rt->peersigns, peer_id);

  if(sg->peer_ip_addr.version == BGPSTREAM_ADDR_VERSION_IPV4)
    {
      v = 4;
    }
  else
    {
      if(sg->peer_ip_addr.version == BGPSTREAM_ADDR_VERSION_IPV6)
        {
          v = 6;
        }
    }
  
  if(bgpstream_addr_ntop(ip_str, INET6_ADDRSTRLEN, (bgpstream_ip_addr_t *) &sg->peer_ip_addr) == NULL)
    {
      fprintf(stderr, "Warning: could not print peer ip address \n");
    }
  graphite_safe(ip_str);  
  if(snprintf(p->peer_str, BGPSTREAM_UTILS_STR_NAME_LEN,
              "peer_asn.%"PRIu32".ipv%"PRIu8"_peer.__IP_%s", sg->peer_asnumber, v, ip_str) >= BGPSTREAM_UTILS_STR_NAME_LEN)
    {
      fprintf(stderr, "Warning: could not print peer signature: truncated output\n");
    }
  p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_UNKNOWN;
  p->bgp_time_ref_rib_start = 0;
  p->bgp_time_ref_rib_end = 0;
  p->bgp_time_uc_rib_start = 0;
  p->bgp_time_uc_rib_end = 0;
  p->last_ts = 0;
  p->rib_positive_mismatches_cnt = 0;
  p->rib_negative_mismatches_cnt = 0;
  p->metrics_generated = 0;

  if((p->announcing_ases = bgpstream_id_set_create()) == NULL)
    {
      fprintf(stderr, "Error: Could not create announcing ASes (for peer)\n");
      goto err;
    }
  
  if((p->announced_v4_pfxs = bgpstream_ipv4_pfx_set_create()) == NULL)
    {
      fprintf(stderr, "Error: Could not create announced ipv4 prefix (for peer)\n");
      goto err;
    }

  if((p->withdrawn_v4_pfxs = bgpstream_ipv4_pfx_set_create()) == NULL)
    {
      fprintf(stderr, "Error: Could not create withdrawn ipv4 prefix (for peer)\n");
      goto err;
    }

  if((p->announced_v6_pfxs = bgpstream_ipv6_pfx_set_create()) == NULL)
    {
      fprintf(stderr, "Error: Could not create announced ipv6 prefix (for peer)\n");
      goto err;
    }

  if((p->withdrawn_v6_pfxs = bgpstream_ipv6_pfx_set_create()) == NULL)
    {
      fprintf(stderr, "Error: Could not create withdrawn ipv6 prefix (for peer)\n");
      goto err;
    }
  
  return p;
 err:
  perpeer_info_destroy(p);
  return NULL;
}


static void
destroy_collector_data(collector_t * c)
{
  if(c != NULL)
    {
      if(c->collector_peerids != NULL)
        {
          kh_destroy(peer_id_set, c->collector_peerids);
        }
      c->collector_peerids = NULL;

      if(c->active_ases != NULL)
        {
          bgpstream_id_set_destroy(c->active_ases);
        }
      c->active_ases = NULL;

    }
}


static collector_t *
get_collector_data(routingtables_t *rt, char *project, char *collector)
{
  khiter_t k;
  int khret;
  collector_t c_data;
  
  /* create new collector-related structures if it is the first time
   * we see it */
  if((k = kh_get(collector_data, rt->collectors, collector))
     == kh_end(rt->collectors))
    {

      /* collector data initialization (all the fields needs to be */
      /* explicitely initialized */
      
      char project_name[BGPSTREAM_UTILS_STR_NAME_LEN];
      strncpy(project_name, project, BGPSTREAM_UTILS_STR_NAME_LEN);
      graphite_safe(project_name);  

      char collector_name[BGPSTREAM_UTILS_STR_NAME_LEN];
      strncpy(collector_name, collector, BGPSTREAM_UTILS_STR_NAME_LEN);
      graphite_safe(collector_name);  

      if(snprintf(c_data.collector_str , BGPSTREAM_UTILS_STR_NAME_LEN,
                  "%s.%s", project_name, collector_name) >= BGPSTREAM_UTILS_STR_NAME_LEN)
        {
          fprintf(stderr, "Warning: could not print collector signature: truncated output\n");
        }
      
      if((c_data.collector_peerids = kh_init(peer_id_set)) == NULL)
        {
          goto err;
        }

      if((c_data.active_ases = bgpstream_id_set_create()) == NULL)
        {
          goto err;
        }
      
      c_data.bgp_time_last = 0;
      c_data.wall_time_last = 0;
      c_data.bgp_time_ref_rib_dump_time = 0;
      c_data.bgp_time_ref_rib_start_time = 0;
      c_data.bgp_time_uc_rib_dump_time = 0;
      c_data.bgp_time_uc_rib_start_time = 0;
      c_data.state = ROUTINGTABLES_COLLECTOR_STATE_UNKNOWN;
      c_data.active_peers_cnt = 0;
      c_data.valid_record_cnt = 0;
      c_data.corrupted_record_cnt = 0;
      c_data.empty_record_cnt = 0;
      c_data.publish_flag = 0;
      
      collector_generate_metrics(rt, &c_data);

      /* insert key,value in map */
      k = kh_put(collector_data, rt->collectors, strdup(collector), &khret);      
      kh_val(rt->collectors,k) = c_data;
      
    }
  
  return &kh_val(rt->collectors,k);
  
 err:
  destroy_collector_data(&c_data);
  return NULL;
}



/** Stop the under construction process
 *  @note: this function does not deactivate the peer-pfx fields, 
 *  the peer may be active */
static void
stop_uc_process(routingtables_t *rt, collector_t *c)
{  
  perpeer_info_t *p;
  perpfx_perpeer_info_t *pp;
  
  for(bgpwatcher_view_iter_first_pfx_peer(rt->iter, 0,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_pfx_peer(rt->iter);
      bgpwatcher_view_iter_next_pfx_peer(rt->iter))
    {
      
      /* check if the current field refers to a peer to reset */      
      if(kh_get(peer_id_set, c->collector_peerids, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)) !=
         kh_end(c->collector_peerids))
        {
          /* the peer belongs to the collector's peers */
          pp = bgpwatcher_view_iter_pfx_peer_get_user(rt->iter);
          pp->bgp_time_uc_delta_ts = 0;
          pp->uc_origin_asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;              
          if(bgpwatcher_view_iter_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_INACTIVE)
            {
              bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, ROUTINGTABLES_DOWN_ORIGIN_ASN);
              pp->bgp_time_last_ts = 0;
            }
        }
    }

  /* reset all the uc information for the peers */
  for(bgpwatcher_view_iter_first_peer(rt->iter, BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_peer(rt->iter);
      bgpwatcher_view_iter_next_peer(rt->iter))
    {
      /* check if the current field refers to a peer to reset */
      if(kh_get(peer_id_set, c->collector_peerids, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)) !=
         kh_end(c->collector_peerids))
        {
          p = bgpwatcher_view_iter_peer_get_user(rt->iter);
          p->bgp_time_uc_rib_start = 0;
          p->bgp_time_uc_rib_end = 0;
        }
    }

  /* reset all the uc information for the  collector */
  c->bgp_time_uc_rib_dump_time = 0;
  c->bgp_time_uc_rib_start_time = 0;
}

/** Reset all the pfxpeer data associated with the
 *  provided peer id 
 *  @note: this is the function to call when putting a peer down*/
static void
reset_peerpfxdata(routingtables_t *rt,
                  bgpstream_peer_id_t peer_id, uint8_t reset_uc)
{
  perpfx_perpeer_info_t *pp;
  if(bgpwatcher_view_iter_seek_peer(rt->iter,
                                    peer_id,
                                    BGPWATCHER_VIEW_FIELD_ALL_VALID) == 1)
    {
      for(bgpwatcher_view_iter_first_pfx_peer(rt->iter, 0,
                                              BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                              BGPWATCHER_VIEW_FIELD_ALL_VALID);
          bgpwatcher_view_iter_has_more_pfx_peer(rt->iter);
          bgpwatcher_view_iter_next_pfx_peer(rt->iter))
        {
          if(bgpwatcher_view_iter_peer_get_peer_id(rt->iter) == peer_id)
            {
              bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, ROUTINGTABLES_DOWN_ORIGIN_ASN);
              pp = bgpwatcher_view_iter_pfx_peer_get_user(rt->iter);
              pp->bgp_time_last_ts = 0;
              if(reset_uc)
                {
                  pp->bgp_time_uc_delta_ts = 0;
                  pp->uc_origin_asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;
                }
              bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
            }
        }
      bgpwatcher_view_iter_seek_peer(rt->iter,
                                    peer_id,
                                     BGPWATCHER_VIEW_FIELD_ALL_VALID);
    }  
}

static int
end_of_valid_rib(routingtables_t *rt, collector_t *c)
{
  perpeer_info_t *p;
  perpfx_perpeer_info_t *pp;
  bgpstream_pfx_t *pfx;

  /** Read the entire collector RIB and update the items according to
   *  timestamps (either promoting the RIB UC data, or maintaining 
   *  (the current state) based on the comparison with the UC RIB */
  for(bgpwatcher_view_iter_first_pfx_peer(rt->iter, 0,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_pfx_peer(rt->iter);
      bgpwatcher_view_iter_next_pfx_peer(rt->iter))
    {
      p = bgpwatcher_view_iter_peer_get_user(rt->iter);
      pfx = bgpwatcher_view_iter_pfx_get_pfx(rt->iter);
      
      /* check if the current field refers to a peer involved
       * in the rib process  */
     if(kh_get(peer_id_set, c->collector_peerids, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)) !=
        kh_end(c->collector_peerids)  &&
        p->bgp_time_uc_rib_start != 0)
        {
          pp = bgpwatcher_view_iter_pfx_peer_get_user(rt->iter);

          /* if the RIB timestamp is greater than the last updated time in the current
           * state, AND  the update did not happen within ROUTINGTABLES_RIB_BACKLOG_TIME seconds before
           * the beginning of the RIB (if that is so, the update message may be still buffered
           * in the quagga process), then the RIB has more updated data than our state */
          if(pp->bgp_time_uc_delta_ts + p->bgp_time_uc_rib_start > pp->bgp_time_last_ts &&
             !(pp->bgp_time_last_ts > p->bgp_time_uc_rib_start - ROUTINGTABLES_RIB_BACKLOG_TIME))
            {
              if(pp->uc_origin_asn != ROUTINGTABLES_DOWN_ORIGIN_ASN)
                {

                  /* if the prefix was set (that's why we look for ts!= 0)
                   * inactive in the previous state and now it is in the rib */
                  if(pp->bgp_time_last_ts != 0 &&
                     bgpwatcher_view_iter_pfx_peer_get_orig_asn(rt->iter) == ROUTINGTABLES_DOWN_ORIGIN_ASN)
                    {
                      p->rib_negative_mismatches_cnt++;
                      fprintf(stderr, "Warning - missed announcement: %s @ %s  last state: %"PRIu32" rib: %"PRIu32" \n",
                              bgpstream_pfx_snprintf(buffer, INET6_ADDRSTRLEN+3, pfx),
                              p->peer_str,
                              pp->bgp_time_last_ts, pp->bgp_time_uc_delta_ts + p->bgp_time_uc_rib_start);

                    }

                  pp->bgp_time_last_ts = pp->bgp_time_uc_delta_ts + p->bgp_time_uc_rib_start;
                  bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, pp->uc_origin_asn);
              
                  bgpwatcher_view_iter_activate_peer(rt->iter);
                  p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED;
                  p->bgp_time_ref_rib_start = p->bgp_time_uc_rib_start;
                  p->bgp_time_ref_rib_end = p->bgp_time_uc_rib_end;              
                  bgpwatcher_view_iter_pfx_activate_peer(rt->iter);
                }
              else
                {
                  /* the last modification of the current pfx is before the current uc rib
                   * but the prefix is not in the uc rib: therefore we deactivate the field
                   * (it may be already inactive) */
                  if(bgpwatcher_view_iter_pfx_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_ACTIVE)
                    {
                      p->rib_positive_mismatches_cnt++;
                      fprintf(stderr, "Warning - missed withdrawal: %s  last state: %"PRIu32" rib: %"PRIu32" \n",
                              bgpstream_pfx_snprintf(buffer, INET6_ADDRSTRLEN+3, pfx),
                              pp->bgp_time_last_ts, pp->bgp_time_uc_delta_ts + p->bgp_time_uc_rib_start);

                    }
                  pp->bgp_time_last_ts = 0;
                  bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, ROUTINGTABLES_DOWN_ORIGIN_ASN);
                  bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
                }
               
            }
          else
            {
              /* if an update is more recent than the uc information, or if 
               * the last update message was applied just ROUTINGTABLES_RIB_BACKLOG_TIME 
               * before the RIB dumping process startedm then
               * we decide to keep this data and activate the field if it
               * is an announcement */
              if(bgpwatcher_view_iter_pfx_peer_get_orig_asn(rt->iter) != ROUTINGTABLES_DOWN_ORIGIN_ASN)
                {
                  
                  bgpwatcher_view_iter_activate_peer(rt->iter);
                  p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED;
                  p->bgp_time_ref_rib_start = p->bgp_time_uc_rib_start;
                  p->bgp_time_ref_rib_end = p->bgp_time_uc_rib_end;
                  bgpwatcher_view_iter_pfx_activate_peer(rt->iter);
                }
            }
          /* reset uc fields anyway */
          pp->bgp_time_uc_delta_ts = 0;
          pp->uc_origin_asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;
        }

    }

  
  /* reset all the uc information for the peers and check if
   * some peers disappeared from the routing table (i.e., if some active
   * peers are not in this RIB, then it means they went down in between
   * the previous RIB and this RIB  and we have to deactivate them */
  for(bgpwatcher_view_iter_first_peer(rt->iter, BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_peer(rt->iter);
      bgpwatcher_view_iter_next_peer(rt->iter))
    {
      /* check if the current field refers to a peer that belongs to
       * the current collector */
      if(kh_get(peer_id_set, c->collector_peerids, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)) !=
         kh_end(c->collector_peerids))
        {
          p = bgpwatcher_view_iter_peer_get_user(rt->iter);

          /* if the uc rib start was never touched it means
           * that this peer was not part of the RIB and, therefore,
           * if it claims to be active, we deactivate it */
          if(p->bgp_time_uc_rib_start == 0 &&
             p->last_ts < c->bgp_time_last - ROUTINGTABLES_MAX_INACTIVE_TIME)
            {
              if(p->bgp_fsm_state == BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED)
                {
                  p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_UNKNOWN;
                  reset_peerpfxdata(rt, bgpwatcher_view_iter_peer_get_peer_id(rt->iter), 0);
                  bgpwatcher_view_iter_deactivate_peer(rt->iter);
                }
            }
          else
            {
              /* if the peer was actively involved in the uc process
               * we reset its variables */
              p->bgp_time_uc_rib_start = 0;
              p->bgp_time_uc_rib_end = 0;
            }
        }
    }

  c->publish_flag = 1;
  
  /* reset all the uc information for the  collector */
  c->bgp_time_ref_rib_dump_time = c->bgp_time_uc_rib_dump_time;
  c->bgp_time_ref_rib_start_time = c->bgp_time_uc_rib_start_time;
  c->bgp_time_uc_rib_dump_time = 0;
  c->bgp_time_uc_rib_start_time = 0;
  
  return 0;
}


static void
update_prefix_peer_stats(perpfx_perpeer_info_t *pp, bgpstream_elem_t *elem)

{
  if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
    {
      pp->announcements++;
    }
  else
    {
      pp->withdrawals++;
    }
}

static void
update_peer_stats(perpeer_info_t *p, bgpstream_elem_t *elem, uint32_t asn)
{
  if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
    {
      bgpstream_id_set_insert(p->announcing_ases, asn);
      if(elem->prefix.address.version == BGPSTREAM_ADDR_VERSION_IPV4)
        {
          bgpstream_ipv4_pfx_set_insert(p->announced_v4_pfxs, (bgpstream_ipv4_pfx_t *) &elem->prefix);
          return;
        }
      if(elem->prefix.address.version == BGPSTREAM_ADDR_VERSION_IPV6)
        {
          bgpstream_ipv6_pfx_set_insert(p->announced_v6_pfxs, (bgpstream_ipv6_pfx_t *) &elem->prefix);
          return;
        }        
    }
  else
    {
      if(elem->prefix.address.version == BGPSTREAM_ADDR_VERSION_IPV4)
        {
          bgpstream_ipv4_pfx_set_insert(p->withdrawn_v4_pfxs, (bgpstream_ipv4_pfx_t *) &elem->prefix);
          return;
        }
      if(elem->prefix.address.version == BGPSTREAM_ADDR_VERSION_IPV6)
        {
          bgpstream_ipv6_pfx_set_insert(p->withdrawn_v6_pfxs, (bgpstream_ipv6_pfx_t *) &elem->prefix);
          return;
        }
    }
}

/** Apply an announcement update or a withdrawal update
 *  @param peer_id peer affected by the update
 *  @param asn     origin 
 *  @return 0 if it finishes correctly, < 0 if something
 *          went wrong
 *
 *  Prerequisites: 
 *  the peer exists and it is either active or inactive
 *  the current iterator points at the right peer
 *  the update time >= collector->bgp_time_ref_rib_start_time
 */
static int
apply_prefix_update(routingtables_t *rt, collector_t *c, bgpstream_peer_id_t peer_id,
                    bgpstream_elem_t *elem, uint32_t ts)
{

  assert(peer_id);
  assert(peer_id == bgpwatcher_view_iter_peer_get_peer_id(rt->iter));

  perpeer_info_t *p = bgpwatcher_view_iter_peer_get_user(rt->iter);
  
  perpfx_perpeer_info_t *pp = NULL;  

  uint32_t asn = 0;

  /* populate correctly the asn if it is an announcement */
  if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
    {
      asn = get_origin_asn(elem->aspath);
      p->pfx_announcements_cnt++;      
    }
  else
    {
      asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;
      p->pfx_withdrawals_cnt++;
    }

  update_peer_stats(p, elem, asn);
  
  if(bgpwatcher_view_iter_seek_pfx_peer(rt->iter, (bgpstream_pfx_t *) &elem->prefix, peer_id, 
                                        BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                        BGPWATCHER_VIEW_FIELD_ALL_VALID) == 0)
    {
      /* the prefix-peer does not exist, therefore we 
       * create a new empty structure to populate */
      if(bgpwatcher_view_iter_add_pfx_peer(rt->iter,
                                           (bgpstream_pfx_t *) &elem->prefix,
                                           peer_id, asn) != 0)
        {
          fprintf(stderr, "bgpwatcher_view_iter_add_pfx_peer fails\n");
          return -1;
        }
      /* when we create a new pfx peer this has to be inactive */
      bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
    }

  if((pp = (perpfx_perpeer_info_t *)bgpwatcher_view_iter_pfx_peer_get_user(rt->iter)) == NULL)
    {
      pp = perpfx_perpeer_info_create();
      bgpwatcher_view_iter_pfx_peer_set_user(rt->iter, pp);
    }      
      
  if(ts < pp->bgp_time_last_ts)
    {
      /* the update is old and it does not change the state */
      return 0;
    }

  /* the ts received is more recent than the information in the pfx-peer
   * we update both ts and asn */
  pp->bgp_time_last_ts = ts;
  bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, asn);
  update_prefix_peer_stats(pp,elem);
  
  if(bgpwatcher_view_iter_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_ACTIVE)
    {
      /* the announcement moved the pfx-peer state from inactive to active */
      if(bgpwatcher_view_iter_pfx_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_INACTIVE &&
         elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
        {      
          bgpwatcher_view_iter_pfx_activate_peer(rt->iter);
          return 0;
        }

      /* the withdrawal moved the pfx-peer state from active to inactive */
      if(bgpwatcher_view_iter_pfx_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_ACTIVE &&
         elem->type == BGPSTREAM_ELEM_TYPE_WITHDRAWAL)
        {
          bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
          return 0;
        }

      /* no state change required */
      return 0;
    }  
  else
    {
      if(bgpwatcher_view_iter_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_INACTIVE)
        {
          /* if the peer is inactive, all if its pfx-peers must be inactive */
          assert(bgpwatcher_view_iter_pfx_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_INACTIVE);

          if(p->bgp_fsm_state == BGPSTREAM_ELEM_PEERSTATE_UNKNOWN)
            {
              
              if(p->bgp_time_uc_rib_start != 0)
                {
                  /* case 1: the peer is inactive because its state is unknown and there is
                   * an under construction process going on: 
                   * the peer remains inactive, the information already inserted
                   * in the pfx-peer (pp) will be used when the uc rib becomes active,
                   * while the pfx-peer remains inactive */
                  return 0;
                }
              else
                {
                  /* case 2: the peer is inactive because its state is unknown and there is
                   * no under construction process going on 
                   * the peer remains inactive, the information already inserted
                   * in the pfx-peer (pp) needs to be reset (as well as the stats, we only
                   * take into account stats on updates that we apply)
                   * while the pfx-peer remains inactive */                               
                  pp->bgp_time_last_ts = 0;
                  bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, ROUTINGTABLES_DOWN_ORIGIN_ASN);
                  if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
                    {
                      pp->announcements--;
                    }
                  else
                    {
                      pp->withdrawals--;
                    }
                  return 0;
                }
            }             
          else
            {
              /* case 3: the peer is inactive because its fsm state went down,
               * if we receive a new update we assume the state is established 
               * and the peer is up again  */              
              bgpwatcher_view_iter_activate_peer(rt->iter);
              p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED;
              p->bgp_time_ref_rib_start = ts;
              p->bgp_time_ref_rib_end = ts;
              if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
                {
                  /* the pfx-peer goes active only if we received an announcement */
                  bgpwatcher_view_iter_pfx_activate_peer(rt->iter);
                }
              return 0;
            }
                          
        }
      else
        {
          /* a peer must exist (no matter if active or inactive)
           * before entering this function*/
          assert(0);
        }             
    }
  
  return 0;
}



static int
apply_state_update(routingtables_t *rt, collector_t * c, bgpstream_peer_id_t peer_id,
                   bgpstream_elem_peerstate_t new_state, uint32_t ts)
{

  assert(peer_id);
  assert(peer_id == bgpwatcher_view_iter_peer_get_peer_id(rt->iter));
  perpeer_info_t *p = bgpwatcher_view_iter_peer_get_user(rt->iter);

  p->state_messages_cnt++;
  
  uint8_t reset_uc = 0;
  
  if(p->bgp_fsm_state == BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED &&
     new_state != BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED)
    {
      /* the peer is active and we receive a peer down message */
      p->bgp_fsm_state = new_state;
      p->bgp_time_ref_rib_start = ts;
      p->bgp_time_ref_rib_end = ts;
      reset_uc = 0;
      /* check whether the state message affects the uc process */
      if(ts >= p->bgp_time_uc_rib_start)
        {
          reset_uc = 1;
          p->bgp_time_uc_rib_start = 0;
          p->bgp_time_uc_rib_end = 0;
        }
      /* reset all peer pfx data associated with the peer */
      reset_peerpfxdata(rt, peer_id, reset_uc);
      bgpwatcher_view_iter_deactivate_peer(rt->iter);
    }      
  else
    {
      if(p->bgp_fsm_state != BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED &&
         new_state == BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED)
        {
          /* the peer is inactive and we receive a peer up message */
          p->bgp_fsm_state = new_state;
          p->bgp_time_ref_rib_start = ts;
          p->bgp_time_ref_rib_end = ts;
          bgpwatcher_view_iter_activate_peer(rt->iter);

        }
      else
        {
          if(p->bgp_fsm_state != new_state)
            {
              /* if the new state does not change the peer active/inactive status,
               * update the FSM state anyway */
              p->bgp_fsm_state = new_state;
              p->bgp_time_ref_rib_start = ts;
              p->bgp_time_ref_rib_end = ts;         
            }
        }
    }

 
   if(p->bgp_fsm_state == BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED)
    {
      assert(bgpwatcher_view_iter_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_ACTIVE);
    }
  else
    {
      assert(bgpwatcher_view_iter_peer_get_state(rt->iter) == BGPWATCHER_VIEW_FIELD_INACTIVE);
    }

  return 0;
}


static int
apply_rib_message(routingtables_t *rt, collector_t * c, bgpstream_peer_id_t peer_id,
                  bgpstream_elem_t *elem, uint32_t ts)
{
  
  assert(peer_id);
  assert(peer_id == bgpwatcher_view_iter_peer_get_peer_id(rt->iter));

  perpeer_info_t *p = bgpwatcher_view_iter_peer_get_user(rt->iter);
  perpfx_perpeer_info_t *pp = NULL;

  if(p->bgp_time_uc_rib_start == 0)
    {
      /* first rib message for this peer */
      p->bgp_time_uc_rib_start = ts;
    }  
  p->bgp_time_uc_rib_end = ts;  
  p->rib_messages_cnt++;

  if(bgpwatcher_view_iter_seek_pfx_peer(rt->iter, (bgpstream_pfx_t *) &elem->prefix, peer_id, 
                                        BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                        BGPWATCHER_VIEW_FIELD_ALL_VALID) == 0)
    {
      /* the prefix-peer does not exist, therefore we 
       * create a new empty structure to populate */
      if(bgpwatcher_view_iter_add_pfx_peer(rt->iter,
                                           (bgpstream_pfx_t *) &elem->prefix,
                                           peer_id,
                                           ROUTINGTABLES_DOWN_ORIGIN_ASN) != 0)
        {
          fprintf(stderr, "bgpwatcher_view_iter_add_pfx_peer fails\n");
          return -1;
        }
      /* when we create a new pfx peer this has to be inactive */
      bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
    }

  if((pp = (perpfx_perpeer_info_t *)bgpwatcher_view_iter_pfx_peer_get_user(rt->iter)) == NULL)
    {
      pp = perpfx_perpeer_info_create();
      bgpwatcher_view_iter_pfx_peer_set_user(rt->iter, pp);
    }      

  /* we update only the uc part of the pfx-peer */
  pp->bgp_time_uc_delta_ts = ts - p->bgp_time_uc_rib_start;
  pp->uc_origin_asn = get_origin_asn(elem->aspath);
  
  return 0;
}


static void
update_collector_state(routingtables_t *rt,
                       collector_t *c,
                       bgpstream_record_t *record)
{
  /** we update the bgp_time_last and every ROUTINGTABLES_COLLECTOR_WALL_UPDATE_FR 
   *  seconds we also update the last wall time */
  if(record->attributes.record_time > c->bgp_time_last)
    {
      if(record->attributes.record_time >
         (c->bgp_time_last + ROUTINGTABLES_COLLECTOR_WALL_UPDATE_FR))
        {
          c->wall_time_last = get_wall_time_now();
        }
      c->bgp_time_last = record->attributes.record_time;      
    }

  /** we update the status of the collector based on the state of its peers 
   * a collector is in an unknown state if all of its peers
   * are in an unknown state, it is down if all of its peers 
   * states are either down or unknown, it is up if at least
   * one peer is up */

  perpeer_info_t *p;  
  uint8_t unknown = 1;

  c->active_peers_cnt = 0;
  
  for(bgpwatcher_view_iter_first_peer(rt->iter, BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_peer(rt->iter);
      bgpwatcher_view_iter_next_peer(rt->iter))
    {
      if(kh_get(peer_id_set, c->collector_peerids,
                bgpwatcher_view_iter_peer_get_peer_id(rt->iter)) !=
         kh_end(c->collector_peerids))
        {
          switch(bgpwatcher_view_iter_peer_get_state(rt->iter))
            {
            case BGPWATCHER_VIEW_FIELD_ACTIVE:
              c->active_peers_cnt++;
              break; 
            case BGPWATCHER_VIEW_FIELD_INACTIVE:
              p = bgpwatcher_view_iter_peer_get_user(rt->iter);
              if(p->bgp_fsm_state != BGPSTREAM_ELEM_PEERSTATE_UNKNOWN)
                {
                  unknown = 0;
                }
              break;
            default:
              /* a valid peer cannot be in state invalid */
              assert(0);
            }
        }
    }

  if(c->active_peers_cnt)
    {
      c->state = ROUTINGTABLES_COLLECTOR_STATE_UP;
    }
  else
    {
      if(unknown == 1)
        {
          c->state = ROUTINGTABLES_COLLECTOR_STATE_UNKNOWN;
        }
      else
        {
          c->state = ROUTINGTABLES_COLLECTOR_STATE_DOWN;
        }
    }
  return;
}


/* debug static int peerscount = 0; */
static int
collector_process_valid_bgpinfo(routingtables_t *rt,
                                collector_t *c,
                                bgpstream_record_t *record)
{
  bgpstream_elem_t *elem;
  bgpstream_peer_id_t peer_id;
  perpeer_info_t *p;
  bgpstream_as_path_iter_t pi;
  bgpstream_as_path_seg_t *seg;

  int khret;
  khiter_t k;

  /* prepare the current collector for a new rib file
   * if that is the case */           
  if(record->attributes.dump_type == BGPSTREAM_RIB)
    {
      /* start a new RIB construction process if there is a 
       * new START message */
      if(record->dump_pos == BGPSTREAM_DUMP_START)
        {
          /* if there is already another under construction 
           * process going on, then we have to reset the process */
          if(c->bgp_time_uc_rib_dump_time != 0)
            {
              stop_uc_process(rt, c);
            }
          c->bgp_time_uc_rib_dump_time  = record->attributes.dump_time;
          c->bgp_time_uc_rib_start_time = record->attributes.record_time;
        }
      /* we process RIB information (ALL of them: start,middle,end)
       * only if there is an under construction process that refers
       * to the same RIB dump */
      if(record->attributes.dump_time != c->bgp_time_uc_rib_dump_time)
        {
          return 0;
        }        
    }

  while((elem = bgpstream_record_get_next_elem(record)) != NULL)
    {

      /* see https://trac.caida.org/hijacks/wiki/ASpaths for more details */
      
      if(elem->type == BGPSTREAM_ELEM_TYPE_RIB || elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT)
        {
          /* we do not maintain status for prefixes announced locally by the collector */
          if(bgpstream_as_path_get_len(elem->aspath) == 0)
            {
              continue;
            }

          /* in order to avoid to maintain status for route servers, we only accept 
           * reachability information from external BGP sessions that do prepend their
           * peer AS number */
          bgpstream_as_path_iter_reset(&pi);
          seg = bgpstream_as_path_get_next_seg(elem->aspath, &pi);
          if(seg->type == BGPSTREAM_AS_PATH_SEG_ASN && ((bgpstream_as_path_seg_asn_t *) seg)->asn != elem->peer_asnumber)
            {
              continue;
            }
        }
      
      /* get the peer id or create a new peer with state inactive
       * (if it did not exist already) */
      if((peer_id = bgpwatcher_view_iter_add_peer(rt->iter,
                                                  record->attributes.dump_collector,
                                                  (bgpstream_ip_addr_t *) &elem->peer_address,
                                                  elem->peer_asnumber)) == 0)
        {
          return -1;                 
        }

      if((p = (perpeer_info_t *)bgpwatcher_view_iter_peer_get_user(rt->iter)) == NULL)
        {
          p = perpeer_info_create(rt, c, peer_id);
          bgpwatcher_view_iter_peer_set_user(rt->iter,p);
        }
      p->last_ts = record->attributes.record_time;
      
      /* insert the peer id in the collector peer ids set */
      if((k = kh_get(peer_id_set, c->collector_peerids, peer_id)) == kh_end(c->collector_peerids))
        {
          k = kh_put(peer_id_set, c->collector_peerids, peer_id, &khret);
        }

      /* processs each elem based on the type */
      if(elem->type == BGPSTREAM_ELEM_TYPE_ANNOUNCEMENT || elem->type == BGPSTREAM_ELEM_TYPE_WITHDRAWAL)
        {
          /* update involving a single prefix */
          if(apply_prefix_update(rt, c, peer_id, elem,
                                 record->attributes.record_time) != 0)
            {
              return -1;
            }
        }
      else
        {
          if(elem->type == BGPSTREAM_ELEM_TYPE_PEERSTATE)
            {
              /* update involving an entire peer */
              if(apply_state_update(rt, c, peer_id, elem->new_state,
                                    record->attributes.record_time) != 0)
                {
                  return -1;
                }
            }
          else
            {
              if(elem->type == BGPSTREAM_ELEM_TYPE_RIB)
                {
                  /* apply the rib message */
                  if(apply_rib_message(rt, c, peer_id, elem,
                                       record->attributes.record_time) != 0)
                    {
                      return -1;
                    }
                }
              else
                {
                  /* bgpstream bug: an elem type of a valid record cannot be UNKNOWN */
                  assert(0);            
                }
            }
        }            
    }

  /* if we just processed the end of a rib file */
  if(record->attributes.dump_type == BGPSTREAM_RIB &&
     record->dump_pos == BGPSTREAM_DUMP_END)
    {
      /* promote the current uc information to active
       * information and reset the uc info */
      end_of_valid_rib(rt, c);
    }
  
  return 0;
}

static int
collector_process_corrupted_message(routingtables_t *rt,
                                    collector_t *c,
                                    bgpstream_record_t *record)
{
  khiter_t k;
  bgpstream_peer_id_t peer_id;  
  perpeer_info_t *p;
  perpfx_perpeer_info_t *pp;
  
  /* list of peers whose current active rib is affected by the
   * corrupted message */
  bgpstream_id_set_t *cor_affected = bgpstream_id_set_create();
  /* list of peers whose current under construction rib is affected by the
   * corrupted message */
  bgpstream_id_set_t *cor_uc_affected = bgpstream_id_set_create();
  
  /* get all the peers that belong to the current collector */    
  for(k = kh_begin(c->collector_peerids); k != kh_end(c->collector_peerids); ++k)
    {
      if(kh_exist(c->collector_peerids, k))
	{
          peer_id = kh_key(c->collector_peerids, k);
          bgpwatcher_view_iter_seek_peer(rt->iter, peer_id, BGPWATCHER_VIEW_FIELD_ALL_VALID);
          p = bgpwatcher_view_iter_peer_get_user(rt->iter);
          assert(p);
          
          /* save all the peers affected by the corrupted record */    
          if(p->bgp_time_ref_rib_start != 0 && record->attributes.record_time >= p->bgp_time_ref_rib_start)
            {
              bgpstream_id_set_insert(cor_affected, peer_id);
            }

          /* save all the peers whose under construction process is
           * affected by the corrupted record */    
          if(p->bgp_time_uc_rib_start != 0 && record->attributes.record_time >= p->bgp_time_uc_rib_start)
            {
              bgpstream_id_set_insert(cor_uc_affected, peer_id);
            }
        }
    }

  /* @note: in principle is possible for the under construction process to be affected
   * by the corrupted record without the active information being affected. That's why
   * we check the verify the impact of the corrupted record (and deal with it) treating
   * the active and uc information of a prefix peer separately */
  

  /* update all the prefix-peer information */
  for(bgpwatcher_view_iter_first_pfx_peer(rt->iter, 0,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID,
                                          BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_pfx_peer(rt->iter);
      bgpwatcher_view_iter_next_pfx_peer(rt->iter))
    {      
      pp = bgpwatcher_view_iter_pfx_peer_get_user(rt->iter);

      if(bgpstream_id_set_exists(cor_affected, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)))
        {
          if(pp->bgp_time_last_ts !=0 && pp->bgp_time_last_ts <= record->attributes.record_time)
            {
              /* reset the active information if the active state is affected */
              pp->bgp_time_last_ts = 0;
              bgpwatcher_view_iter_pfx_peer_set_orig_asn(rt->iter, ROUTINGTABLES_DOWN_ORIGIN_ASN);
              bgpwatcher_view_iter_pfx_deactivate_peer(rt->iter);
            }
        }
        
      if(bgpstream_id_set_exists(cor_uc_affected, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)))
        {
          /* reset the uc information if the under construction process is affected */
          pp->bgp_time_uc_delta_ts = 0;
          pp->uc_origin_asn = ROUTINGTABLES_DOWN_ORIGIN_ASN;          
        }      
    }

  /* update all the peer information */
  for(bgpwatcher_view_iter_first_peer(rt->iter, BGPWATCHER_VIEW_FIELD_ALL_VALID);
      bgpwatcher_view_iter_has_more_peer(rt->iter);
      bgpwatcher_view_iter_next_peer(rt->iter))
    {
      p = bgpwatcher_view_iter_peer_get_user(rt->iter);
      
      if(bgpstream_id_set_exists(cor_affected, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)))
        {          
          p->bgp_fsm_state = BGPSTREAM_ELEM_PEERSTATE_UNKNOWN;
          p->bgp_time_ref_rib_start = 0;
          p->bgp_time_ref_rib_end = 0;
          bgpwatcher_view_iter_deactivate_peer(rt->iter);
        }
      if(bgpstream_id_set_exists(cor_uc_affected, bgpwatcher_view_iter_peer_get_peer_id(rt->iter)))
        {
          p->bgp_time_uc_rib_start = 0;
          p->bgp_time_uc_rib_end = 0;
        }
    }

  bgpstream_id_set_destroy(cor_affected);
  bgpstream_id_set_destroy(cor_uc_affected);
  
  return 0;
}


#ifdef WITH_BGPWATCHER
int
routingtables_send_view(routingtables_t *rt)
{
  return bgpwatcher_client_send_view(rt->watcher_client, rt->view, filter_ff_peers);
}
#endif


/* ========== PUBLIC FUNCTIONS ========== */

routingtables_t *routingtables_create(char *plugin_name, timeseries_t *timeseries)
{  
  routingtables_t *rt = (routingtables_t *)malloc_zero(sizeof(routingtables_t));
  if(rt == NULL)
    {
      goto err;
    }

  if((rt->peersigns = bgpstream_peer_sig_map_create() ) == NULL)
    {
      goto err;
    }

  if((rt->view = bgpwatcher_view_create_shared(rt->peersigns,
                                               free /* view user destructor */,                                               
                                               perpeer_info_destroy /* peer user destructor */,
                                               NULL /* pfx destructor */,
                                               free /* pfxpeer user destructor */)) == NULL)
    {
      goto err;
    }

  if((rt->iter = bgpwatcher_view_iter_create(rt->view)) == NULL)
   {
      goto err;
    }

  rt->timeseries = timeseries;

  if((rt->kp = timeseries_kp_init(rt->timeseries, 1)) == NULL)
    {
      fprintf(stderr, "Error: Could not create timeseries key package\n");
      goto err;
    }

  if((rt->collectors = kh_init(collector_data)) == NULL)
    {
      goto err;
    }

  strcpy(rt->plugin_name, plugin_name);
  
  // set the metric prefix string to the default value
  routingtables_set_metric_prefix(rt,
                                  ROUTINGTABLES_DEFAULT_METRIC_PFX);
  rt->metrics_output_on = 1;
  // set the ff thresholds to their default values
  rt->ipv4_fullfeed_th = ROUTINGTABLES_DEFAULT_IPV4_FULLFEED_THR;
  rt->ipv6_fullfeed_th = ROUTINGTABLES_DEFAULT_IPV6_FULLFEED_THR;

  rt_view_data_t *view_data = (rt_view_data_t *) malloc_zero(sizeof(rt_view_data_t));
  if(view_data == NULL)
    {
      goto err;
    }
  view_data->ipv4_fullfeed_th = rt->ipv4_fullfeed_th;
  view_data->ipv6_fullfeed_th = rt->ipv6_fullfeed_th;
  bgpwatcher_view_set_user(rt->view, view_data);
  
  rt->bgp_time_interval_start = 0;
  rt->bgp_time_interval_end = 0;
  rt->wall_time_interval_start = 0;

  
#ifdef WITH_BGPWATCHER
  rt->watcher_tx_on = 0;
  rt->watcher_client = NULL;
  rt->tables_mask = 0;
#endif  

  return rt;

 err:
  fprintf(stderr, "routingtables_create failed\n");
  routingtables_destroy(rt);
  return NULL;
}

bgpwatcher_view_t *routingtables_get_view_ptr(routingtables_t *rt)
{
  return rt->view;
}

void routingtables_set_metric_prefix(routingtables_t *rt,
                                     char *metric_prefix)
{
  if(metric_prefix == NULL ||
     strlen(metric_prefix)-1 > ROUTINGTABLES_METRIC_PFX_LEN)
    {
      fprintf(stderr,
              "Warning: could not set metric prefix, using default %s \n",
              ROUTINGTABLES_DEFAULT_METRIC_PFX);
      strcpy(rt->metric_prefix, ROUTINGTABLES_DEFAULT_METRIC_PFX);
      return;
    }
  strcpy(rt->metric_prefix, metric_prefix);
}

char *routingtables_get_metric_prefix(routingtables_t *rt)
{
  return &rt->metric_prefix[0];
}

void routingtables_turn_metric_output_off(routingtables_t *rt)
{
  rt->metrics_output_on = 0;
}


#ifdef WITH_BGPWATCHER
int routingtables_activate_watcher_tx(routingtables_t *rt,
                                      char *client_name,
                                      char *server_uri)
{

  if((rt->watcher_client = bgpwatcher_client_init(0 /* no interests */,
                                                  BGPWATCHER_PRODUCER_INTENT_PREFIX /* peers and pfxs*/
                                                  )) == NULL)
    {
      fprintf(stderr,
              "Error: could not initialize bgpwatcher client\n");
      return -1;
    }

  if(server_uri != NULL &&
     bgpwatcher_client_set_server_uri(rt->watcher_client, server_uri) != 0)
    {
      goto err;
    }

  if(client_name != NULL &&
     bgpwatcher_client_set_identity(rt->watcher_client, client_name) != 0)
    {
      fprintf(stderr,
              "Warning: could not set client identity to %s, using random ID\n",
              client_name);
    }
  
  if(bgpwatcher_client_start(rt->watcher_client) != 0)
    {
      fprintf(stderr,
              "Error: cannot start bgpwatcher client \n");
      goto err;
    }
  
  rt->watcher_tx_on = 1;

  return 0;

    err:
  if(rt->watcher_client != NULL)
    {
      bgpwatcher_client_perr(rt->watcher_client);
      bgpwatcher_client_free(rt->watcher_client);
    }
    rt->watcher_tx_on = 0;
    rt->watcher_client = NULL;
    return -1; 
}
#endif

void routingtables_set_fullfeed_threshold(routingtables_t *rt,
                                         bgpstream_addr_version_t ip_version,
                                         uint32_t threshold)
{
  rt_view_data_t *view_data = (rt_view_data_t *)bgpwatcher_view_get_user(rt->view);
  switch(ip_version)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      rt->ipv4_fullfeed_th = threshold;
      view_data->ipv4_fullfeed_th = rt->ipv4_fullfeed_th;        
      break;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      rt->ipv6_fullfeed_th = threshold;
      view_data->ipv6_fullfeed_th = rt->ipv6_fullfeed_th;       
      break;
    default:
      /* programming error */
      assert(0);      
    }
}

void routingtables_activate_partial_feed_tx(routingtables_t *rt)
{
  rt_view_data_t *view_data = (rt_view_data_t *)bgpwatcher_view_get_user(rt->view);
  view_data->ipv4_fullfeed_th = 0;        
  view_data->ipv6_fullfeed_th = 0;       
}

int routingtables_get_fullfeed_threshold(routingtables_t *rt,
                                         bgpstream_addr_version_t ip_version)
{
 switch(ip_version)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      return rt->ipv4_fullfeed_th;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      return rt->ipv6_fullfeed_th;
    default:
      /* programming error */
      assert(0);      
    }
 return -1;
}

int routingtables_interval_start(routingtables_t *rt,
                                 int start_time)
{
  rt->bgp_time_interval_start = (uint32_t) start_time;
  rt->wall_time_interval_start = get_wall_time_now();
  /* setting the time of the view */
  bgpwatcher_view_set_time(rt->view, rt->bgp_time_interval_start);
  return 0;
}

int routingtables_interval_end(routingtables_t *rt,
                               int end_time)
{
  rt->bgp_time_interval_end = (uint32_t) end_time;

#ifdef WITH_BGPWATCHER
  if(rt->watcher_tx_on)
    {
      routingtables_send_view(rt);
    }
#endif

  uint32_t time_now = get_wall_time_now();
  uint32_t elapsed_time = time_now - rt->wall_time_interval_start;
  fprintf(stderr, "Interval [%"PRIu32", %"PRIu32"] processed in %"PRIu32"s\n",
          rt->bgp_time_interval_start, rt->bgp_time_interval_end, elapsed_time);

  if(rt->metrics_output_on)
    {
      routingtables_dump_metrics(rt, time_now);
    }

  return 0;
}

int routingtables_process_record(routingtables_t *rt,
                                 bgpstream_record_t *record)
{
  int ret = 0;
  collector_t *c;
  
  /* get a pointer to the current collector data, if no data
   * exists yet, a new structure will be created */
  if((c = get_collector_data(rt,
                             record->attributes.dump_project,
                             record->attributes.dump_collector)) == NULL)
    {
      return -1;
    }

  /* if a record refer to a time prior to the current reference time,
   * then we discard it, unless we are in the process of building a
   * new rib, in that case we check the time against the uc starting
   * time and if it is a prior record we discard it */
  if(record->attributes.record_time < c->bgp_time_ref_rib_start_time)
    {
      if(c->bgp_time_uc_rib_dump_time != 0)
        {
          if(record->attributes.record_time < c->bgp_time_ref_rib_start_time)
            {
              return 0;
            }
        }
    }

  switch(record->status)
    {
    case BGPSTREAM_RECORD_STATUS_VALID_RECORD:
      ret = collector_process_valid_bgpinfo(rt, c, record);
      c->valid_record_cnt++;
      break;
    case BGPSTREAM_RECORD_STATUS_CORRUPTED_SOURCE:
    case BGPSTREAM_RECORD_STATUS_CORRUPTED_RECORD:
      ret = collector_process_corrupted_message(rt, c, record);
      c->corrupted_record_cnt++;
      break;
    case BGPSTREAM_RECORD_STATUS_FILTERED_SOURCE:
    case BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE:
      /** An empty or filtered source does not change the current
       *  state of a collector, however we update the last_ts
       *  observed */
      if(record->attributes.record_time < c->bgp_time_last)
        {
          c->bgp_time_last = record->attributes.record_time;
        }
      c->empty_record_cnt++;
      break;
    default:
      /* programming error */
      assert(0);
    }
  
  update_collector_state(rt, c, record);

  /* fprintf(stderr, "Processed %s record %ld\n", */
  /*         c->collector_str, */
  /*         record->attributes.record_time); */

  return ret;
}

void routingtables_destroy(routingtables_t *rt)
{
  khiter_t k;
  if(rt != NULL)
    {
      if(rt->collectors != NULL)
        {
          for (k = kh_begin(rt->collectors);
               k != kh_end(rt->collectors); ++k)
            {          
              if (kh_exist(rt->collectors, k))
                {
                  /* deallocating value dynamic memory */
                  destroy_collector_data(&kh_val(rt->collectors, k));
                  /* deallocating string dynamic memory */
                  free(kh_key(rt->collectors, k));
                }
            }   
          kh_destroy(collector_data, rt->collectors );
          rt->collectors = NULL;    
        }
      
      if(rt->iter != NULL)
        {
          bgpwatcher_view_iter_destroy(rt->iter);
          rt->iter = NULL;
        }
            
      if(rt->view != NULL)
        {
          bgpwatcher_view_destroy(rt->view);
          rt->view =NULL;
        }
              
      if(rt->peersigns != NULL)
        {
          bgpstream_peer_sig_map_destroy(rt->peersigns);
          rt->peersigns = NULL;
        }

      if(rt->kp != NULL)
        {
          timeseries_kp_free(&rt->kp);
          rt->kp = NULL;
        }

#ifdef WITH_BGPWATCHER
      if(rt->watcher_client != NULL)
        {
          bgpwatcher_client_stop(rt->watcher_client);
	  bgpwatcher_client_perr(rt->watcher_client);
	  bgpwatcher_client_free(rt->watcher_client);
          rt->watcher_client = NULL;
        }
#endif

      free(rt);
    }
}

