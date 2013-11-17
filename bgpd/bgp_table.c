/* BGP routing table
   Copyright (C) 1998, 2001 Kunihiro Ishiguro

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the Free
Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

#include <zebra.h>

#include "prefix.h"
#include "memory.h"
#include "sockunion.h"
#include "vty.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_peer.h"
#include "bgpd/bgp_table.h"

static void bgp_node_delete (struct bgp_node *);
static void bgp_table_free (struct bgp_table *);
static void bgp_table_check(const struct bgp_table *table) ;

struct bgp_table *
bgp_table_init (afi_t afi, safi_t safi)
{
  struct bgp_table *rt;

  rt = XCALLOC (MTYPE_BGP_TABLE, sizeof (struct bgp_table));

  bgp_table_lock(rt);
  rt->type = BGP_TABLE_MAIN;
  rt->afi = afi;
  rt->safi = safi;

  return rt;
}

void
bgp_table_lock (struct bgp_table *rt)
{
  rt->lock++;
}

void
bgp_table_unlock (struct bgp_table *rt)
{
  assert (rt->lock > 0);
  rt->lock--;

  if (rt->lock == 0)
    bgp_table_free (rt);
}

void
bgp_table_finish (struct bgp_table **rt)
{
  if (*rt != NULL)
    {
      bgp_table_unlock(*rt);
      *rt = NULL;
    }
}

static struct bgp_node * bgp_node_calloc (void) ;
static void bgp_node_free (struct bgp_node *rn) ;

/* Allocate new route node with prefix set.
 */
static struct bgp_node *
bgp_node_set (struct bgp_table *table, struct prefix *prefix)
{
  struct bgp_node *node;

  node = bgp_node_calloc ();            /* Returns zeriozed node        */

  prefix_copy (&node->p, prefix);
  node->table = table;

  return node;
}

/* Free route table.
 */
static void
bgp_table_free (struct bgp_table *rt)
{
  struct bgp_node *tmp_node;
  struct bgp_node *node;

  if (rt == NULL)
    return;

  node = rt->top;

  /* Bulk deletion of nodes remaining in this table.  This function is not
     called until workers have completed their dependency on this table.
     A final bgp_unlock_node() will not be called for these nodes. */
  while (node)
    {
      if (node->l_left)
        {
          node = node->l_left;
          continue;
        }

      if (node->l_right)
        {
          node = node->l_right;
          continue;
        }

      qassert(  (node->u.info    == NULL)
             && (node->adj_outs  == NULL)
             && (node->adj_ins   == NULL)
             && (!node->on_wq) ) ;

      tmp_node = node;
      node = node->parent;

      tmp_node->table->count--;
      tmp_node->lock = 0;  /* to cause assert if unlocked after this */

      bgp_node_free (tmp_node);

      if (node != NULL)
        {
          if (node->l_left == tmp_node)
            node->l_left = NULL;
          else
            node->l_right = NULL;
        }
      else
        {
          break;
        }
    }

  assert (rt->count == 0);

  if (rt->owner)
    {
      bgp_peer_unlock (rt->owner);
      rt->owner = NULL;
    }

  rt->lock = -54321 ;
  XFREE (MTYPE_BGP_TABLE, rt);
  return;
}

/* Utility mask array. */
static const u_char maskbit[] =
{
  0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff
};

/* Common prefix route genaration. */
static void
route_common (struct prefix *n, struct prefix *p, struct prefix *new)
{
  int i;
  u_char diff;
  u_char mask;

  u_char *np = (u_char *)&n->u.prefix;
  u_char *pp = (u_char *)&p->u.prefix;
  u_char *newp = (u_char *)&new->u.prefix;

  for (i = 0; i < p->prefixlen / 8; i++)
    {
      if (np[i] == pp[i])
        newp[i] = np[i];
      else
        break;
    }

  new->prefixlen = i * 8;

  if (new->prefixlen != p->prefixlen)
    {
      diff = np[i] ^ pp[i];
      mask = 0x80;
      while (new->prefixlen < p->prefixlen && !(mask & diff))
        {
          mask >>= 1;
          new->prefixlen++;
        }
      newp[i] = np[i] & maskbit[new->prefixlen % 8];
    }
}

static void
set_link (struct bgp_node *node, struct bgp_node *new)
{
  unsigned int bit = prefix_bit (&new->p.u.prefix, node->p.prefixlen);

  node->link[bit] = new;
  new->parent = node;
}

/* Lock node. */
struct bgp_node *
bgp_lock_node (struct bgp_node *node)
{
  node->lock++;
  return node;
}

/* Unlock node. */
void
bgp_unlock_node (struct bgp_node *node)
{
  assert (node->lock > 0);
  node->lock--;

  if (node->lock == 0)
    bgp_node_delete (node);
}

/* Find matched prefix. */
struct bgp_node *
bgp_node_match (const struct bgp_table *table, struct prefix *p)
{
  struct bgp_node *node;
  struct bgp_node *matched;

  matched = NULL;
  node = table->top;

  /* Walk down tree.  If there is matched route then store it to
     matched. */
  while (node && node->p.prefixlen <= p->prefixlen &&
         prefix_match (&node->p, p))
    {
      if (node->u.info)
        matched = node;
      node = node->link[prefix_bit(&p->u.prefix, node->p.prefixlen)];
    }

  /* If matched route found, return it. */
  if (matched)
    return bgp_lock_node (matched);

  return NULL;
}

struct bgp_node *
bgp_node_match_ipv4 (const struct bgp_table *table, struct in_addr *addr)
{
  struct prefix_ipv4 p;

  memset (&p, 0, sizeof (struct prefix_ipv4));
  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_PREFIXLEN;
  p.prefix = *addr;

  return bgp_node_match (table, (struct prefix *) &p);
}

#ifdef HAVE_IPV6
struct bgp_node *
bgp_node_match_ipv6 (const struct bgp_table *table, struct in6_addr *addr)
{
  struct prefix_ipv6 p;

  memset (&p, 0, sizeof (struct prefix_ipv6));
  p.family = AF_INET6;
  p.prefixlen = IPV6_MAX_PREFIXLEN;
  p.prefix = *addr;

  return bgp_node_match (table, (struct prefix *) &p);
}
#endif /* HAVE_IPV6 */

/* Lookup same prefix node.  Return NULL when we can't find route. */
struct bgp_node *
bgp_node_lookup (const struct bgp_table *table, struct prefix *p)
{
  struct bgp_node *node;

  node = table->top;

  while ((node != NULL) && (node->p.prefixlen <= p->prefixlen)
                        && prefix_match (&node->p, p))
    {
      if ((node->p.prefixlen == p->prefixlen) && (node->u.info != NULL))
        return bgp_lock_node (node);

      node = node->link[prefix_bit(&p->u.prefix, node->p.prefixlen)];
    }

  return NULL;
}

/*------------------------------------------------------------------------------
 * For given table and prefix: find or add node.
 *
 * Once a node has been created, the prefix is stable, until the lock expires.
 *
 * If creates a new node, sets the node->prn as given.  (If does not create
 * node, should find the prn same as given.)
 *
 * NB: prn must be NULL unless table is safi == SAFI_MPLS_VPN.
 *
 * Returns with a lock on the node.
 */
extern struct bgp_node *
bgp_node_get (struct bgp_table *const table, struct prefix *p,
                                                           struct bgp_node *prn)
{
  struct bgp_node *new;
  struct bgp_node *node;
  struct bgp_node *match;

  qassert((prn == NULL) || (table->safi == SAFI_MPLS_VPN)) ;

  match = NULL;
  node = table->top;
  while (node && node->p.prefixlen <= p->prefixlen &&
         prefix_match (&node->p, p))
    {
      if (node->p.prefixlen == p->prefixlen)
        {
          qassert(node->prn == prn) ;
          return bgp_lock_node (node);
        }
      match = node;
      node = node->link[prefix_bit(&p->u.prefix, node->p.prefixlen)];
    }

  if (node == NULL)
    {
      new = bgp_node_set (table, p);
      if (match)
        set_link (match, new);
      else
        table->top = new;
    }
  else
    {
      new = bgp_node_calloc ();         /* returns zeroized node        */

      route_common (&node->p, p, &new->p);
      new->p.family = p->family;
      new->table = table;
      set_link (new, node);

      if (match)
        set_link (match, new);
      else
        table->top = new;

      if (new->p.prefixlen != p->prefixlen)
        {
          match = new;
          new = bgp_node_set (table, p);
          set_link (match, new);
          table->count++;
        }
    } ;

  new->prn = prn ;

  table->count++;
  bgp_lock_node (new);

  return new;
}

/* Delete node from the routing table. */
static void
bgp_node_delete (struct bgp_node *node)
{
  struct bgp_node *child;
  struct bgp_node *parent;

  assert (node->lock   == 0);
  assert (node->u.info == NULL);
  assert (!node->on_wq) ;

  if (node->l_left && node->l_right)
    return;

  if (node->l_left)
    child = node->l_left;
  else
    child = node->l_right;

  parent = node->parent;

  if (child)
    child->parent = parent;

  if (parent)
    {
      if (parent->l_left == node)
        parent->l_left = child;
      else
        parent->l_right = child;
    }
  else
    node->table->top = child;

  node->table->count--;

  bgp_node_free (node);

  /* If parent node is stub then delete it also. */
  if (parent && parent->lock == 0)
    bgp_node_delete (parent);
}

/* Get fist node and lock it.  This function is useful when one want
   to lookup all the node exist in the routing table. */
struct bgp_node *
bgp_table_top (const struct bgp_table *const table)
{
  /* If there is no node in the routing table return NULL. */
  if (table->top == NULL)
    return NULL;

  if (qdebug)
    bgp_table_check(table) ;

  /* Lock the top node and return it. */
  bgp_lock_node (table->top);
  return table->top;
}

/* Unlock current node and lock next node then return it. */
struct bgp_node *
bgp_route_next (struct bgp_node *node)
{
  struct bgp_node *next;
  struct bgp_node *start;

  /* Node may be deleted from bgp_unlock_node so we have to preserve
     next node's pointer. */

  if (node->l_left)
    {
      next = node->l_left;
      bgp_lock_node (next);
      bgp_unlock_node (node);
      return next;
    }
  if (node->l_right)
    {
      next = node->l_right;
      bgp_lock_node (next);
      bgp_unlock_node (node);
      return next;
    }

  start = node;
  while (node->parent)
    {
      if (node->parent->l_left == node && node->parent->l_right)
        {
          next = node->parent->l_right;
          bgp_lock_node (next);
          bgp_unlock_node (start);
          return next;
        }
      node = node->parent;
    }
  bgp_unlock_node (start);
  return NULL;
}

/* Unlock current node and lock next node until limit. */
struct bgp_node *
bgp_route_next_until (struct bgp_node *node, struct bgp_node *limit)
{
  struct bgp_node *next;
  struct bgp_node *start;

  /* Node may be deleted from bgp_unlock_node so we have to preserve
     next node's pointer. */

  if (node->l_left)
    {
      next = node->l_left;
      bgp_lock_node (next);
      bgp_unlock_node (node);
      return next;
    }
  if (node->l_right)
    {
      next = node->l_right;
      bgp_lock_node (next);
      bgp_unlock_node (node);
      return next;
    }

  start = node;
  while (node->parent && node != limit)
    {
      if (node->parent->l_left == node && node->parent->l_right)
        {
          next = node->parent->l_right;
          bgp_lock_node (next);
          bgp_unlock_node (start);
          return next;
        }
      node = node->parent;
    }
  bgp_unlock_node (start);
  return NULL;
}

unsigned long
bgp_table_count (const struct bgp_table *table)
{
  return table->count;
}

/*==============================================================================
 * Stuff for debug.
 */
static uint bgp_table_node_check(const struct bgp_node* rn, uint count) ;

/*------------------------------------------------------------------------------
 * Walk the given table and check that it is tickety-boo.
 *
 * For each node, will check:
 *
 *   1) prefix is valid -- all zeros beyond the prefix length.
 *
 *   1) child-nodes point back at the parent.
 *
 *   2) child node prefix length is greater than parent's
 *
 *   3) child node prefix matches parent's to parent's length
 *
 *   4) left child has  '0' in prefix at parent's length
 *      right child has '1' ditto
 *
 *   5) count nodes and check that total matches
 */
static void
bgp_table_check(const struct bgp_table *table)
{
  struct bgp_node *node ;
  uint count ;

  node  = table->top ;
  count = table->count ;
  if (node != NULL)
    count = bgp_table_node_check(node, count) ;

  qassert(count == 0) ;
} ;

/*------------------------------------------------------------------------------
 * Check the given node and its children -- node not NULL
 */
static uint
bgp_table_node_check(const struct bgp_node* rn, uint count)
{
  uint bit ;

  qassert(count != 0) ;

  count -= 1 ;

  qassert(prefix_check(&rn->p)) ;

  for (bit = 0 ; bit <= 1 ; ++bit)
    {
      const struct bgp_node* cn ;

      cn = rn->link[bit] ;
      if (cn != NULL)
        {
          qassert(rn == cn->parent) ;
          qassert(rn->p.prefixlen < cn->p.prefixlen) ;
          qassert(prefix_match(&rn->p, &cn->p)) ;
          qassert(bit == prefix_bit(&cn->p.u.prefix, rn->p.prefixlen)) ;

          count = bgp_table_node_check(cn, count) ;
        } ;
    } ;

  return count ;
} ;

/*==============================================================================
 * Pool for bgp_nodes.
 */
enum
{
  rn_pool_size = 1024,
} ;

struct rn_pool
{
  struct rn_pool*  next ;
  struct bgp_node  rns[rn_pool_size] ;
};

struct rn_pool*   rn_pools = NULL ;
struct bgp_node*  rn_frees = NULL ;

static struct bgp_node *
bgp_node_calloc (void)
{
  struct bgp_node* rn;

  rn = rn_frees ;

  if (rn == NULL)
    {
      struct rn_pool* pool ;
      uint i ;

      pool = XCALLOC(MTYPE_BGP_NODE, sizeof(struct rn_pool)) ;

      pool->next = rn_pools ;
      rn_pools   = pool ;

      for (i = 0 ; i < rn_pool_size ; ++i)
        {
          rn = &pool->rns[i] ;

          rn->wq_next = rn_frees ;
          rn_frees    = rn ;
        } ;
    } ;

  rn_frees = rn->wq_next ;

  memset(rn, 0, sizeof(struct bgp_node)) ;
  return rn ;
} ;

/* Free route node.
 */
static void
bgp_node_free (struct bgp_node *rn)
{
  rn->wq_next = rn_frees ;
  rn_frees    = rn ;
} ;

extern void
bgp_table_all_finish(void)
{
  struct rn_pool* pool ;

  rn_frees = NULL ;

  while ((pool = rn_pools) != NULL)
    {
      rn_pools = rn_pools->next ;

      XFREE(MTYPE_BGP_NODE, pool) ;
    } ;
} ;
