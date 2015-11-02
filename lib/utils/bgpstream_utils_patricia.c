/*
 * This file is part of bgpstream
 *
 * CAIDA, UC San Diego
 * bgpstream-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This software is heavily based on software developed by
 *
 * Dave Plonka <plonka@doit.wisc.edu>
 *
 * This product includes software developed by the University of Michigan,
 * Merit Network, Inc., and their contributors.
 *
 * This file had been called "radix.c" in the MRT sources.
 *
 * I renamed it to "patricia.c" since it's not an implementation of a general
 * radix trie.  Also I pulled in various requirements from "prefix.c" and
 * "demo.c" so that it could be used as a standalone API.
 */


#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>


#include "utils.h"
#include "bgpstream_utils_patricia.h"
#include "bgpstream_utils_pfx.h"

/* for debug purposes */
/* DEBUG static char buffer[1024]; */


#define BGPSTREAM_PATRICIA_MAXBITS 128

#define BIT_TEST(f, b)  ((f) & (b))


static int
comp_with_mask (void *addr, void *dest, u_int mask)
{

    if ( /* mask/8 == 0 || */ memcmp (addr, dest, mask / 8) == 0) {
	int n = mask / 8;
	int m = ((-1) << (8 - (mask % 8)));

	if (mask % 8 == 0 || (((u_char *)addr)[n] & m) == (((u_char *)dest)[n] & m))
	    return (1);
    }
    return (0);
}


struct bgpstream_patricia_node {

  /* flag if this node used */
  u_int bit;

  /* who we are in patricia tree */
  bgpstream_pfx_storage_t prefix;

  /* left and right children */
  bgpstream_patricia_node_t *l;
  bgpstream_patricia_node_t *r;

  /* parent node */
  bgpstream_patricia_node_t *parent;

  /* pointer to user data */
  void *user;

};


struct bgpstream_patricia_tree {

  /* IPv4 tree */
  bgpstream_patricia_node_t *head4;

  /* IPv6 tree */
  bgpstream_patricia_node_t *head6;

  /* Number of nodes per tree */
  uint64_t ipv4_active_nodes;
  uint64_t ipv6_active_nodes;

  /** Pointer to a function that destroys the user structure
   *  in the bgpstream_patricia_node_t structure */
  bgpstream_patricia_tree_destroy_user_t *node_user_destructor;
};


static unsigned char *bgpstream_pfx_get_first_byte(bgpstream_pfx_t *pfx)
{
  switch(pfx->address.version)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      return (unsigned char *) &((bgpstream_ipv4_pfx_t *)pfx)->address.ipv4.s_addr;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      return (unsigned char *) &((bgpstream_ipv6_pfx_t *)pfx)->address.ipv6.s6_addr[0];
    default:
      assert(0);
    }
  return NULL;
}


static bgpstream_patricia_node_t *bgpstream_patricia_node_create(bgpstream_patricia_tree_t *pt,
                                                                 bgpstream_pfx_t *pfx)
{
  bgpstream_patricia_node_t *node;

  assert(pfx);
  assert(pfx->mask_len <= BGPSTREAM_PATRICIA_MAXBITS);
  assert(pfx->address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN);

  if((node = malloc_zero(sizeof(bgpstream_patricia_node_t))) == NULL)
    {
      return NULL;
    }

  if(pfx->address.version == BGPSTREAM_ADDR_VERSION_IPV4)
    {
      pt->ipv4_active_nodes++;
    }
  else
    {
      pt->ipv6_active_nodes++;
    }

  node->prefix = *((bgpstream_pfx_storage_t *) pfx);
  node->parent = NULL;
  node->bit = pfx->mask_len;
  node->l = NULL;
  node->r = NULL;
  return node;
}

static bgpstream_patricia_node_t *bgpstream_patricia_gluenode_create()
{
  bgpstream_patricia_node_t *node;

  if((node = malloc_zero(sizeof(bgpstream_patricia_node_t))) == NULL)
    {
      return NULL;
    }
  node->prefix.address.version = BGPSTREAM_ADDR_VERSION_UNKNOWN;
  node->parent = NULL;
  node->bit = 0;
  node->l = NULL;
  node->r = NULL;
  return node;
}

static bgpstream_patricia_node_t *bgpstream_patricia_get_head(bgpstream_patricia_tree_t *pt,
                                                              bgpstream_addr_version_t v)
{
  switch(v)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      return pt->head4;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      return pt->head6;
    default:
      assert(0);
    }
  return NULL;
}

static void bgpstream_patricia_set_head(bgpstream_patricia_tree_t *pt, bgpstream_addr_version_t v,
                                        bgpstream_patricia_node_t *n)
{
  switch(v)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      pt->head4 = n;
      break;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      pt->head6 = n;
      break;
    default:
      assert(0);
    }
}


bgpstream_patricia_tree_t *bgpstream_patricia_tree_create(bgpstream_patricia_tree_destroy_user_t *bspt_user_destructor)
{
  bgpstream_patricia_tree_t *pt = NULL;
  if((pt = malloc_zero(sizeof(bgpstream_patricia_tree_t))) == NULL)
    {
      return NULL;
    }
  pt->head4 = NULL;
  pt->head6 = NULL;
  pt->ipv4_active_nodes = 0;
  pt->ipv6_active_nodes = 0;
  pt->node_user_destructor = bspt_user_destructor;
  return pt;
}


bgpstream_patricia_node_t *bgpstream_patricia_tree_insert(bgpstream_patricia_tree_t *pt, bgpstream_pfx_t *pfx)
{
  assert(pt);
  assert(pfx);
  assert(pfx->mask_len <= BGPSTREAM_PATRICIA_MAXBITS);
  assert(pfx->address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN);

  /* DEBUG   char buffer[1024];
   * bgpstream_pfx_snprintf(buffer, 1024, pfx); */

  bgpstream_patricia_node_t *new_node = NULL;
  bgpstream_addr_version_t v = pfx->address.version;

  /* if Patricia Tree is empty, then insert new node */
  if(bgpstream_patricia_get_head(pt,v) == NULL)
    {
      if((new_node = bgpstream_patricia_node_create(pt, pfx)) == NULL)
        {
          fprintf(stderr, "Error creating pt node\n");
          return NULL;
        }
      /* attach first node in Tree */
      bgpstream_patricia_set_head(pt, v, new_node);
      /* DEBUG       fprintf(stderr, "Adding %s to HEAD\n", buffer); */
      return new_node;
    }

  /* Prepare data for Patricia Tree navigation */

  bgpstream_patricia_node_t *node_it = bgpstream_patricia_get_head(pt, v);

  uint8_t bitlen = pfx->mask_len;
  unsigned char *addr = bgpstream_pfx_get_first_byte(pfx);

  /* navigate Patricia Tree till we:
   * - reach the end of the tree (i.e. next node_it is null)
   * - the current node has the same mask length (or greater) and
   *   it contains a valid prefix (i.e. it is not a glue node)
   * */
  while (node_it->bit < bitlen || node_it->prefix.address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      if (node_it->bit < BGPSTREAM_PATRICIA_MAXBITS &&
          BIT_TEST (addr[node_it->bit >> 3], 0x80 >> (node_it->bit & 0x07)))
        {
          /* no more nodes on the right, exit from loop */
          if (node_it->r == NULL)
            {
              break;
            }
          /* patricia_lookup: take right at node->bit */
          node_it = node_it->r;
        }
      else
        {
          /* no more nodes on the left, exit from loop */
          if (node_it->l == NULL)
            {
              break;
            }
          /* patricia_lookup: take left at node->bit */
          node_it = node_it->l;
        }
      assert (node_it);
    }

  /*  node_it->prefix is the prefix we stopped at */
  unsigned char *test_addr = bgpstream_pfx_get_first_byte((bgpstream_pfx_t *) &node_it->prefix);


  /* find the first bit different */
  uint8_t check_bit;
  uint8_t differ_bit;
  int i, j, r;
  check_bit = (node_it->bit < bitlen) ? node_it->bit: bitlen;
  differ_bit = 0;
  for (i = 0; i*8 < check_bit; i++)
    {
      if ((r = (addr[i] ^ test_addr[i])) == 0)
        {
          differ_bit = (i + 1) * 8;
          continue;
	}
      /* I know the better way, but for now */
      for (j = 0; j < 8; j++)
        {
          if (BIT_TEST (r, (0x80 >> j)))
            {
              break;
            }
	}
      /* must be found */
      assert (j < 8);
      differ_bit = i * 8 + j;
      break;
    }

  if (differ_bit > check_bit)
    {
      differ_bit = check_bit;
    }

  bgpstream_patricia_node_t *parent;
  bgpstream_patricia_node_t  *glue_node;


  /* go back up till we find the right parent **I.E.??** */
  parent = node_it->parent;
  while (parent && parent->bit >= differ_bit) {
    node_it = parent;
    parent = node_it->parent;
  }


  if (differ_bit == bitlen && node_it->bit == bitlen)
    {
      /* check the node contains a valid prefix,
       * i.e. it is not a glue node */
      if (node_it->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
        {
          /* Exact node found */
          /* DEBUG  fprintf(stderr, "Prefix %s already in tree\n", buffer); */
          return node_it;
        }
      /* otherwise replace the info in the glue node with proper
       * prefix information */
      node_it->prefix = *((bgpstream_pfx_storage_t *) pfx);
      /* patricia_lookup: new node #1 (glue mod) */
      /* DEBUG fprintf(stderr, "Using %s to replace a GLUE node\n", buffer); */
      return node_it;
    }

  /* Create a new node */
  new_node = bgpstream_patricia_node_create(pt, pfx);

  /* Insert the new node in the Patricia Tree: CHILD */
  if (node_it->bit == differ_bit)
    {
      /* appending the new node as a child of node_it */
      new_node->parent = node_it;
      if (node_it->bit < BGPSTREAM_PATRICIA_MAXBITS &&
          BIT_TEST (addr[node_it->bit >> 3], 0x80 >> (node_it->bit & 0x07)))
        {
          assert (node_it->r == NULL);
          node_it->r = new_node;
	}
      else
        {
          assert(node_it->l == NULL);
          node_it->l = new_node;
	}
      /* patricia_lookup: new_node #2 (child) */
      /* DEBUG  fprintf(stderr, "Adding %s as a CHILD node\n", buffer); */
      return new_node;
    }


  /* Insert the new node in the Patricia Tree: PARENT */
  if (bitlen == differ_bit)
    {
      /* attaching the new node as a parent of node_it */
      if (bitlen < BGPSTREAM_PATRICIA_MAXBITS &&
          BIT_TEST (test_addr[bitlen >> 3], 0x80 >> (bitlen & 0x07)))
        {
          new_node->r = node_it;
	}
      else
        {
          new_node->l = node_it;
	}
      new_node->parent = node_it->parent;
      if (node_it->parent == NULL)
        {
          assert(bgpstream_patricia_get_head(pt, v) == node_it);
          bgpstream_patricia_set_head(pt, v, new_node);
	}
      else
        {
          if (node_it->parent->r == node_it)
            {
              node_it->parent->r = new_node;
            }
          else
            {
              node_it->parent->l = new_node;
            }
        }
      node_it->parent = new_node;
      /* patricia_lookup: new_node #3 (parent) */
      /* DEBUG fprintf(stderr, "Adding %s as a PARENT node\n", buffer); */
      return new_node;
    }
  else
    {
      /* Insert the new node in the Patricia Tree: CREATE A GLUE NODE AND APPEND TO IT*/

      glue_node = bgpstream_patricia_gluenode_create();

      glue_node->bit = differ_bit;
      glue_node->parent = node_it->parent;

      if (differ_bit < BGPSTREAM_PATRICIA_MAXBITS &&
          BIT_TEST (addr[differ_bit >> 3], 0x80 >> (differ_bit & 0x07)))
        {
          glue_node->r = new_node;
          glue_node->l = node_it;
	}
      else
        {
          glue_node->r = node_it;
          glue_node->l = new_node;
        }
      new_node->parent = glue_node;

      if (node_it->parent == NULL)
        {
          assert(bgpstream_patricia_get_head(pt, v) == node_it);
          bgpstream_patricia_set_head(pt, v, glue_node);
	}
      else
        {
          if (node_it->parent->r == node_it)
            {
              node_it->parent->r = glue_node;
            }
          else {
	    node_it->parent->l = glue_node;
          }
        }
      node_it->parent = glue_node;
      /* "patricia_lookup: new_node #4 (glue+node) */
      /* DEBUG fprintf(stderr, "Adding %s as a CHILD of a NEW GLUE node\n", buffer); */
      return new_node;
    }

  /* DEBUG   fprintf(stderr, "Adding %s as a ??\n", buffer); */
  return new_node;
}


void *bgpstream_patricia_tree_get_user(bgpstream_patricia_node_t *node)
{
  return node->user;
}


int bgpstream_patricia_tree_set_user(bgpstream_patricia_tree_t *pt, bgpstream_patricia_node_t *node, void *user)
{
  if(node->user == user)
    {
      return 0;
    }
  if(node->user != NULL && pt->node_user_destructor != NULL)
    {
      pt->node_user_destructor(node->user);
    }
  node->user = user;
  return 1;

}


uint8_t bgpstream_patricia_tree_get_pfx_overlap_info(bgpstream_patricia_tree_t *pt,
                                                     bgpstream_pfx_t *pfx)
{

  bgpstream_patricia_node_t *n = bgpstream_patricia_tree_search_exact(pt, pfx);
  if(n != NULL)
    {
      return bgpstream_patricia_tree_get_node_overlap_info(pt, n);
    }
  else
    {
      /* we basically simulate an insertion, to understand whether the prefix
       * would overlap or not */
      n = bgpstream_patricia_tree_insert(pt, pfx);
      uint8_t mask = bgpstream_patricia_tree_get_node_overlap_info(pt, n);
      bgpstream_patricia_tree_remove_node(pt, n);
      return mask;
    }
  return 0;
}


void bgpstream_patricia_tree_remove(bgpstream_patricia_tree_t *pt, bgpstream_pfx_t *pfx)
{
  bgpstream_patricia_tree_remove_node(pt, bgpstream_patricia_tree_search_exact(pt, pfx));
}


void bgpstream_patricia_tree_remove_node(bgpstream_patricia_tree_t *pt, bgpstream_patricia_node_t *node)
{
  assert(pt);
  if(node == NULL)
    {
      return;
    }

  bgpstream_addr_version_t v = node->prefix.address.version;
  bgpstream_patricia_node_t *parent;
  bgpstream_patricia_node_t *child;

  uint64_t *num_active_node = &pt->ipv4_active_nodes;;
  if(node->prefix.address.version == BGPSTREAM_ADDR_VERSION_IPV6)
    {
      num_active_node = &pt->ipv6_active_nodes;
    }

  /* we do not allow for explicit removal of glue nodes */
  if(v == BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      return;
    }

  /* if node has both children */
  if(node->r != NULL && node->l != NULL)
    {
      /* if it is a glue node, there is nothing to remove,
       * if it is node with a valid prefix, then it becomes a glue node
       */
      if(node->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
        {
          node->prefix.address.version = BGPSTREAM_ADDR_VERSION_UNKNOWN;
        }
      /* node data remains, unless we decide to pass a destroy function somewehere */
      /* node->user = NULL; */
      /* DEBUG fprintf(stderr, "Removing node with both children\n"); */
      return;
    }

  /* if node has no children */
  if (node->r == NULL && node->l == NULL)
     {
       parent = node->parent;
       free(node);
       (*num_active_node) = (*num_active_node) - 1;

       /* removing head of tree */
       if (parent == NULL)
         {
           assert(node == bgpstream_patricia_get_head(pt, v));
           bgpstream_patricia_set_head(pt, v, NULL);
           /* DEBUG fprintf(stderr, "Removing head (that had no children)\n"); */
           return;
         }

       /* check if the node was the right or the left child */
       if (parent->r == node)
         {
           parent->r = NULL;
           child = parent->l;
         }
       else
         {
           assert (parent->l == node);
           parent->l = NULL;
           child = parent->r;
         }

       /* if the current parent was a valid prefix, return */
	if (parent->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
          {
            /* DEBUG fprintf(stderr, "Removing node with no children\n"); */
	    return;
          }

	/* otherwise it makes no sense to have a glue node
         * with only one child, the parent has to be removed */

	if (parent->parent == NULL)
          { /* if the parent parent is the head, then attach
             * the only child directly */
            assert(parent == bgpstream_patricia_get_head(pt, v));
            bgpstream_patricia_set_head(pt, v, child);
          }
	else
          {
            if (parent->parent->r == parent)
              { /* if the parent is a right child */
                parent->parent->r = child;
              }
            else
              { /* if the parent is a left child */
                assert (parent->parent->l == parent);
                parent->parent->l = child;
              }
	}
        /* the child parent, is now the grand-parent */
	child->parent = parent->parent;
        free(parent);
	return;
    }

  /* if node has only one child */
  if (node->r)
    {
      child = node->r;
    }
  else
    {
      assert (node->l);
      child = node->l;
    }
  /* the child parent, is now the grand-parent */
  parent = node->parent;
  child->parent = parent;

  free(node);
  (*num_active_node) = (*num_active_node) - 1;

  if (parent == NULL)
    { /* if the parent is the head, then attach
       * the only child directly */
      assert(parent == bgpstream_patricia_get_head(pt, v));
      bgpstream_patricia_set_head(pt, v, child);
      return;
    }
  else
    {
      /* attach child node to the correct parent child pointer */
      if(parent->r == node)
        { /* if node was a right child */
          parent->r = child;
        }
      else
        { /* if node was a left child */
          assert(parent->l == node);
          parent->l = child;
        }
    }

}


bgpstream_patricia_node_t *bgpstream_patricia_tree_search_exact(bgpstream_patricia_tree_t *pt, bgpstream_pfx_t *pfx)
{
  assert(pt);
  assert(pfx);
  assert(pfx->mask_len <= BGPSTREAM_PATRICIA_MAXBITS);
  assert(pfx->address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN);

  bgpstream_addr_version_t v = pfx->address.version;

  /* if Patricia Tree is empty*/
  if(bgpstream_patricia_get_head(pt,v) == NULL)
    {
      return NULL;
    }

  bgpstream_patricia_node_t *node_it = bgpstream_patricia_get_head(pt, v);
  uint8_t bitlen = pfx->mask_len;
  unsigned char *addr = bgpstream_pfx_get_first_byte(pfx);

  while (node_it->bit < bitlen)
    {
      if( BIT_TEST (addr[node_it->bit >> 3], 0x80 >> (node_it->bit & 0x07)))
        {
          /* patricia_lookup: take right at node->bit */
          node_it = node_it->r;
        }
      else
        {
          /* patricia_lookup: take left at node->bit */
          node_it = node_it->l;
        }
      	if (node_it == NULL)
          {
            return NULL;
          }
    }

  /* if we passed the right mask, or if we stopped at a glue node, then
   * no exact match found */
  if (node_it->bit > bitlen || node_it->prefix.address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      return NULL;
    }

  assert (node_it->bit == bitlen);
  /* compare the prefixes bit by bit
   * TODO: consider replacing with bgpstream_pfx_storage_equal */
  if (comp_with_mask(bgpstream_pfx_get_first_byte((bgpstream_pfx_t *) &node_it->prefix),
                     bgpstream_pfx_get_first_byte(pfx), bitlen))
    {
      /* exact match found */
      return node_it;
    }
  return NULL;
}


uint64_t bgpstream_patricia_prefix_count(bgpstream_patricia_tree_t *pt, bgpstream_addr_version_t v)
{
  switch(v)
    {
    case BGPSTREAM_ADDR_VERSION_IPV4:
      return pt->ipv4_active_nodes;
    case BGPSTREAM_ADDR_VERSION_IPV6:
      return pt->ipv6_active_nodes;
    default:
      return 0;
    }
  return 0;
}


static uint64_t bgpstream_patricia_tree_count_subnets(bgpstream_patricia_node_t *node, uint64_t subnet_size)
{
  if(node == NULL)
    {
      return 0;
    }
  /* if the node is a glue node, then the /subnet_size subnets are the sum of the
   * /24 subnets contained in its left and right subtrees */
  if(node->prefix.address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      /* if the glue node is already a /subnet_size, then just return 1 (even though
       * the subnetworks below could be a non complete /subnet_size */
      if(node->bit >= subnet_size)
        {
          return 1;
        }
      else
        {
          return bgpstream_patricia_tree_count_subnets(node->l, subnet_size) + \
            bgpstream_patricia_tree_count_subnets(node->r, subnet_size);
        }
    }
  else
    {
      /* otherwise we just count the subnet for the given network and return
       * we don't need to go deeper in the tree (everything else beyond this
       * point is covered */

      /* compute how many /subnet_size are in this prefix */
      if(node->prefix.mask_len >= subnet_size)
        {
          return 1;
        }
      else
        {
          uint8_t diff = subnet_size - node->prefix.mask_len;
          if(diff == 64)
            {
              return UINT64_MAX;
            }
          else
            {
              return (uint64_t) 1 << diff;
            }
        }
    }
}


uint64_t bgpstream_patricia_tree_count_24subnets(bgpstream_patricia_tree_t *pt)
{
  return bgpstream_patricia_tree_count_subnets(pt->head4, 24);
}


uint64_t bgpstream_patricia_tree_count_64subnets(bgpstream_patricia_tree_t *pt)
{
  return bgpstream_patricia_tree_count_subnets(pt->head6, 64);
}

/* depth = 1 -> Recursively find all more specifics,
 * depth = 0 -> find the first later of more specifics */
static bgpstream_patricia_tree_result_t **
bgpstream_patricia_tree_add_more_specifics(bgpstream_patricia_tree_result_t **last_result,
                                           bgpstream_patricia_node_t *node, const uint8_t depth)
{
  if(node == NULL)
    {
      return last_result;
    }
  /* if it is a node containing a real prefix, then copy the address to a new result node */
  if(node->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      if((*last_result = (bgpstream_patricia_tree_result_t *) malloc_zero(sizeof(bgpstream_patricia_tree_result_t))) == NULL)
        {
          fprintf(stderr, "Error: could not allocate memory for results\n");
          assert(0);
        }
      (*last_result)->node = node;
      (*last_result)->next = NULL;
      last_result = &((*last_result)->next);
      /* DEBUG       bgpstream_pfx_snprintf(buffer, 1024, (bgpstream_pfx_t *) &node->prefix);
       * fprintf(stderr, "More specific found: %s\n", buffer); */
      if(depth == 0)
        { /* if 0 we stop at the first layer */
          return last_result;
        }
    }

  /* using pre-order */
  last_result = bgpstream_patricia_tree_add_more_specifics(last_result, node->l, depth);
  last_result = bgpstream_patricia_tree_add_more_specifics(last_result, node->r, depth);

  return last_result;
}


bgpstream_patricia_tree_result_t *bgpstream_patricia_tree_get_more_specifics(bgpstream_patricia_tree_t *pt,
                                                                             bgpstream_patricia_node_t *node)
{
  bgpstream_patricia_tree_result_t *result = NULL;
  bgpstream_patricia_tree_result_t **r = &result;

  if(node != NULL)
    { /* we do not return the node itself */
      r = bgpstream_patricia_tree_add_more_specifics(r, node->l, 1);
      r = bgpstream_patricia_tree_add_more_specifics(r, node->r, 1);
    }
  return result;
}


bgpstream_patricia_tree_result_t *bgpstream_patricia_tree_get_less_specifics(bgpstream_patricia_tree_t *pt,
                                                                             bgpstream_patricia_node_t *node)
{
  if(node == NULL)
    {
      return NULL;
    }

  bgpstream_patricia_tree_result_t *result = NULL;
  bgpstream_patricia_tree_result_t **last_result = &result;

  /* we do not return the node itself */
  bgpstream_patricia_node_t *node_it = node->parent;
  while(node_it != NULL)
    {
      if(node_it->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
        {
          if((*last_result = (bgpstream_patricia_tree_result_t *) malloc_zero(sizeof(bgpstream_patricia_tree_result_t))) == NULL)
            {
              fprintf(stderr, "Error: could not allocate memory for results\n");
              assert(0);
            }
          (*last_result)->node = node_it;
          (*last_result)->next = NULL;
          last_result = &((*last_result)->next);
          /* DEBUG  bgpstream_pfx_snprintf(buffer, 1024, (bgpstream_pfx_t *) &node_it->prefix);
           *          fprintf(stderr, "Less specific found: %s\n", buffer); */
        }
      node_it = node_it->parent;
    }
  return result;
}


bgpstream_patricia_tree_result_t *bgpstream_patricia_tree_get_minimum_coverage(bgpstream_patricia_tree_t *pt,
                                                                               bgpstream_addr_version_t v)
{
  bgpstream_patricia_node_t *head = bgpstream_patricia_get_head(pt,v);
  bgpstream_patricia_tree_result_t *result = NULL;
  bgpstream_patricia_tree_result_t **r = &result;
  /* we stop at the first layer, hence depth = 0 */
  bgpstream_patricia_tree_add_more_specifics(r, head, 0);
  return result;
}

static int bgpstream_patricia_tree_find_more_specific(bgpstream_patricia_node_t *node)
{
  if(node == NULL)
    {
      return 0;
    }
  /* if it is a node containing a glue node, then we have to search for other cases */
  if(node->prefix.address.version == BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      if(bgpstream_patricia_tree_find_more_specific(node->l) == 0)
        {
          if(bgpstream_patricia_tree_find_more_specific(node->r) == 0)
            {
              return 0;
            }
        }
    }
  /* if it is a node containing a real prefix, we are done */
  return 1;

}


uint8_t bgpstream_patricia_tree_get_node_overlap_info(bgpstream_patricia_tree_t *pt,
                                                      bgpstream_patricia_node_t *node)
{
  uint8_t mask = 0;

  bgpstream_patricia_node_t *node_it = node->parent;
  while(node_it != NULL)
    {
      if(node_it->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
        {
          /* one more specific found */
          mask = mask | BGPSTREAM_PATRICIA_LESS_SPECIFICS;
          break;
        }
      node_it = node_it->parent;
    }

  if(node_it != NULL)
    { /* we do not consider the node itself */
      /* if one of the subtree return 1 we can avoid the other */
      if(bgpstream_patricia_tree_find_more_specific(node->l) == 1)
        {
          mask = mask | BGPSTREAM_PATRICIA_MORE_SPECIFICS;
        }
      else
        {
          if(bgpstream_patricia_tree_find_more_specific(node->r) == 1)
            {
              mask = mask | BGPSTREAM_PATRICIA_MORE_SPECIFICS;
            }
        }
    }
  return mask;
}



void bgpstream_patricia_tree_result_destroy(bgpstream_patricia_tree_result_t **result)
{
  assert(result != NULL);
  bgpstream_patricia_tree_result_t *v = *result;
  bgpstream_patricia_tree_result_t *next;
  while(v != NULL)
    {
      next = v->next;
      free(v);
      v = next;
    }
  *result = NULL;
}


static void bgpstream_patricia_tree_merge_tree(bgpstream_patricia_tree_t *dst, bgpstream_patricia_node_t *node)
{
  if(node == NULL)
    {
      return;
    }
  /* Add the current node, if it is not a glue node */
  if(node->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      bgpstream_patricia_tree_insert(dst, (bgpstream_pfx_t *) &node->prefix);
    }
  /* Recursively add left and right node */
  bgpstream_patricia_tree_merge_tree(dst, node->l);
  bgpstream_patricia_tree_merge_tree(dst, node->r);
}


void bgpstream_patricia_tree_merge(bgpstream_patricia_tree_t *dst, const bgpstream_patricia_tree_t *src)
{
  assert(dst);
  if(src == NULL)
    {
      return;
    }
  /* Merge IPv4 */
  bgpstream_patricia_tree_merge_tree(dst, src->head4);
  /* Merge IPv6 */
  bgpstream_patricia_tree_merge_tree(dst, src->head6);
}


static void bgpstream_patricia_tree_print_tree(bgpstream_patricia_node_t *node)
{
  if(node == NULL)
    {
      return;
    }
  bgpstream_patricia_tree_print_tree(node->l);

  char buffer[1024];

  /* if node is not a glue node, print the prefix */
  if(node->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      memset(buffer, ' ', sizeof(char)*node->prefix.mask_len);
      bgpstream_pfx_snprintf(buffer+node->prefix.mask_len, 1024, (bgpstream_pfx_t *) &node->prefix);
      fprintf(stdout, "%s\n", buffer);
    }

  bgpstream_patricia_tree_print_tree(node->r);
}


void bgpstream_patricia_tree_print(bgpstream_patricia_tree_t *pt)
{
  bgpstream_patricia_tree_print_tree(pt->head4);
  bgpstream_patricia_tree_print_tree(pt->head6);
}


void bgpstream_patricia_tree_print_results(bgpstream_patricia_tree_result_t *result)
{
  bgpstream_patricia_tree_result_t *next = result;
  char buffer[1024];
  while(next != NULL)
    {
      bgpstream_pfx_snprintf(buffer, 1024, (bgpstream_pfx_t *) &next->node->prefix);
      fprintf(stdout, "%s\n", buffer);
      next = next->next;
    }
}


bgpstream_pfx_t *bgpstream_patricia_tree_get_pfx(bgpstream_patricia_node_t *node)
{
  assert(node);
  if(node->prefix.address.version != BGPSTREAM_ADDR_VERSION_UNKNOWN)
    {
      return (bgpstream_pfx_t *) &node->prefix;
    }
  return NULL;
}


static void bgpstream_patricia_tree_destroy_tree(bgpstream_patricia_tree_t *pt, bgpstream_patricia_node_t *head)
{
  if(head != NULL)
    {
      bgpstream_patricia_node_t *l = head->l;
      bgpstream_patricia_node_t *r = head->r;
      bgpstream_patricia_tree_destroy_tree(pt, l);
      bgpstream_patricia_tree_destroy_tree(pt, r);
      if(head->user != NULL && pt->node_user_destructor != NULL)
        {
          pt->node_user_destructor(head->user);
        }
      free(head);
    }
}


void bgpstream_patricia_tree_clear(bgpstream_patricia_tree_t *pt)
{
  assert(pt);

  bgpstream_patricia_tree_destroy_tree(pt, pt->head4);
  pt->ipv4_active_nodes = 0;
  pt->head4 = NULL;

  bgpstream_patricia_tree_destroy_tree(pt, pt->head6);
  pt->ipv6_active_nodes = 0;
  pt->head6 = NULL;
}


void bgpstream_patricia_tree_destroy(bgpstream_patricia_tree_t *pt)
{
  if(pt != NULL)
    {
      bgpstream_patricia_tree_clear(pt);
      free(pt);
    }
}
