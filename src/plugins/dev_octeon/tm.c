/*
 * Copyright (c) 2024 Marvell.
 * SPDX-License-Identifier: Apache-2.0
 * https://spdx.org/licenses/Apache-2.0.html
 */

#include <vnet/vnet.h>
#include <vnet/dev/dev.h>
#include <vnet/dev/pci.h>
#include <vnet/dev/counters.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>
#include <dev_octeon/octeon.h>
#include <dev_octeon/common.h>
#include <base/roc_api.h>
#include <common.h>
#include "tm.h"

VLIB_REGISTER_LOG_CLASS (oct_log, static) = {
  .class_name = "octeon",
  .subclass_name = "tm",
};

static vnet_dev_rv_t
oct_roc_err (vnet_dev_t *dev, int rv, char *fmt, ...)
{
  u8 *s = 0;
  va_list va;

  va_start (va, fmt);
  s = va_format (s, fmt, &va);
  va_end (va);

  log_err (dev, "%v - ROC error %s (%d)", s, roc_error_msg_get (rv), rv);

  vec_free (s);
  return VNET_DEV_ERR_INTERNAL;
}

int
oct_tm_sys_node_add (u32 hw_if_idx, u32 node_id, u32 parent_node_id,
		     u32 priority, u32 weight, u32 lvl,
		     tm_node_params_t *params)

{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  struct roc_nix_tm_node *parent_node = NULL;
  struct roc_nix_tm_node *tm_node = NULL;
  struct roc_nix_tm_shaper_profile *profile = NULL;
  int rc = 0;

  /* We don't support dynamic updates */
  if (roc_nix_tm_is_user_hierarchy_enabled (nix))
    {
      rc = -ERANGE;
      return oct_roc_err (dev, rc, "roc_nix_tm_dynamic update not supported");
    }
  if (parent_node_id)
    {
      parent_node = roc_nix_tm_node_get (nix, parent_node_id);
    }

  /* Find the right level */
  if (lvl != ROC_TM_LVL_ROOT && parent_node)
    {
      lvl = parent_node->lvl + 1;
    }
  else if (parent_node_id == ROC_NIX_TM_NODE_ID_INVALID)
    {
      lvl = ROC_TM_LVL_ROOT;
    }
  else
    {
      /* Neither proper parent nor proper level id given */
      rc = -ERANGE;
      return oct_roc_err (dev, rc, "roc_nix_tm_invalid_parent-id_err");
    }

  tm_node = plt_zmalloc (sizeof (struct roc_nix_tm_node), 0);
  if (!tm_node)
    {
      rc = -ENOMEM;
      return oct_roc_err (dev, rc, "oct_nix_tm_node_alloc_failed");
    }

  tm_node->id = node_id;
  tm_node->parent_id = parent_node_id;
  tm_node->lvl = lvl;
  tm_node->priority = priority;
  tm_node->free_fn = plt_free;
  tm_node->weight = weight;
  tm_node->shaper_profile_id = params->shaper_profile_id;

  profile = roc_nix_tm_shaper_profile_get (nix, params->shaper_profile_id);

  rc = roc_nix_tm_node_add (nix, tm_node);
  if (rc < 0)
    {
      plt_free (tm_node);
      return oct_roc_err (dev, rc, "roc_nix_tm_node_add_err");
    }

  roc_nix_tm_shaper_default_red_algo (tm_node, profile);
  return 0;
}

int
oct_tm_sys_node_delete (u32 hw_if_idx, u32 node_id)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  struct roc_nix_tm_node *tm_node = NULL;
  int rc;
  bool free_node = 1;

  if ((rc = roc_nix_tm_is_user_hierarchy_enabled (nix)))
    {
      rc = -ERANGE;
      return oct_roc_err (dev, rc, "roc_nix_tm_dynamic update not supported");
    }
  if (node_id == ROC_NIX_TM_NODE_ID_INVALID)
    {
      rc = -EINVAL;
      return oct_roc_err (dev, rc, "oct_tm_node_delete_invalid_node-id");
    }

  tm_node = roc_nix_tm_node_get (nix, node_id);
  if (!tm_node)
    {
      rc = -EINVAL;
      return oct_roc_err (dev, rc, "oct_tm_node_delete  node-id not found");
    }

  rc = roc_nix_tm_node_delete (nix, tm_node->id, free_node);
  if (rc)
    {
      return oct_roc_err (dev, rc, "roc_nix_tm_delete_failed");
    }
  return 0;
}

int
oct_tm_sys_shaper_profile_create (u32 hw_if_idx, tm_shaper_params_t *params)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  struct roc_nix_tm_shaper_profile *profile;
  int rc;

  if (roc_nix_tm_shaper_profile_get (nix, params->shaper_id))
    {
      rc = -EINVAL;
      return oct_roc_err (dev, rc, "oct_nix_tm_shaper_exists");
    }

  profile = plt_zmalloc (sizeof (struct roc_nix_tm_shaper_profile), 0);
  if (!profile)
    {
      rc = -ENOMEM;
      return oct_roc_err (dev, rc, "oct_nix_tm_shaper_create_alloc_failed");
    }
  profile->id = params->shaper_id;
  profile->commit_rate = params->commit.rate;
  profile->commit_sz = params->commit.burst_size;
  profile->peak_rate = params->peak.rate;
  profile->peak_sz = params->peak.burst_size;
  /* If Byte mode, then convert to bps */
  if (!params->pkt_mode)
    {
      profile->commit_rate *= 8;
      profile->peak_rate *= 8;
      profile->commit_sz *= 8;
      profile->peak_sz *= 8;
    }
  profile->pkt_len_adj = params->pkt_len_adj;
  profile->pkt_mode = params->pkt_mode;
  profile->free_fn = plt_free;

  rc = roc_nix_tm_shaper_profile_add (nix, profile);

  /* Fill error information based on return value */
  if (rc)
    {
      plt_free (profile);
      return oct_roc_err (dev, rc, "roc_nix_tm_shaper_creation_failed");
    }

  return rc;
}

int
oct_tm_sys_node_shaper_update (u32 hw_if_idx, u32 node_id, u32 profile_id)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  struct roc_nix_tm_shaper_profile *profile;
  struct roc_nix_tm_node *node;
  int rc;

  rc = roc_nix_tm_node_shaper_update (nix, node_id, profile_id, false);
  if (rc)
    {
      return oct_roc_err (dev, rc, "oct_nix_tm_node_shaper_update_failed");
    }

  node = roc_nix_tm_node_get (nix, node_id);
  if (!node)
    {
      rc = -EINVAL;
      return oct_roc_err (dev, rc,
			  "oct_nix_tm_node_shaper_update_node_failure");
    }

  profile = roc_nix_tm_shaper_profile_get (nix, profile_id);
  roc_nix_tm_shaper_default_red_algo (node, profile);

  return 0;
}
int
oct_tm_sys_shaper_profile_delete (u32 hw_if_idx, u32 shaper_id)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  int rc;

  rc = roc_nix_tm_shaper_profile_delete (nix, shaper_id);
  if (rc)
    {
      return oct_roc_err (dev, rc, "roc_nix_tm_shaper_delete_failed");
    }

  return rc;
}

int
oct_tm_sys_node_read_stats (u32 hw_if_idx, u32 node_id,
			    tm_stats_params_t *stats)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  int rc = 0;
  int clear = 0;
  struct roc_nix_tm_node_stats nix_tm_stats;
  struct roc_nix_tm_node *node;

  node = roc_nix_tm_node_get (nix, node_id);
  if (!node)
    {
      goto exit;
    }

  if (roc_nix_tm_lvl_is_leaf (nix, node->lvl))
    {
      struct roc_nix_stats_queue qstats;

      rc = roc_nix_stats_queue_get (nix, node->id, 0, &qstats);
      if (!rc)
	{
	  stats->n_pkts = qstats.tx_pkts;
	  stats->n_bytes = qstats.tx_octs;
	  printf ("  - STATS for node \n");
	  printf ("  -- pkts (%" PRIu64 ") bytes (%" PRIu64 ")\n",
		  stats->n_pkts, stats->n_bytes);
	}
      goto exit;
    }

  rc = roc_nix_tm_node_stats_get (nix, node_id, clear, &nix_tm_stats);
  if (!rc)
    {
      stats->leaf.n_pkts_dropped[TM_COLOR_RED] =
	nix_tm_stats.stats[ROC_NIX_TM_NODE_PKTS_DROPPED];
      stats->leaf.n_bytes_dropped[TM_COLOR_RED] =
	nix_tm_stats.stats[ROC_NIX_TM_NODE_BYTES_DROPPED];
    }

exit:
  if (rc)
    {
      return oct_roc_err (dev, rc, "tm_node_read_stats_err");
    }
  return rc;
}

int
oct_tm_sys_start (u32 hw_if_idx)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  int rc = 0;

  if (roc_nix_tm_is_user_hierarchy_enabled (nix))
    {
      rc = -EIO;
      return oct_roc_err (dev, rc, "oct_nix_tm_hirearchy_exists");
    }

  if (roc_nix_tm_leaf_cnt (nix) < port->intf.num_tx_queues)
    {
      rc = -EINVAL;
      return oct_roc_err (dev, rc, "oct_nix_tm_incomplete hierarchy");
    }

  rc = roc_nix_tm_hierarchy_disable (nix);
  if (rc)
    {
      return oct_roc_err (dev, rc, "oct_nix_tm_hirearchy_exists");
    }

  rc = roc_nix_tm_hierarchy_enable (nix, ROC_NIX_TM_USER, true);
  if (rc)
    {
      return oct_roc_err (dev, rc, "oct_nix_tm_hierarchy_enabled_failed");
    }
  return 0;
}

int
oct_tm_sys_stop (u32 hw_if_idx)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_idx);
  vnet_dev_port_t *port =
    vnet_dev_get_port_from_dev_instance (hi->dev_instance);
  vnet_dev_t *dev = port->dev;
  oct_device_t *cd = vnet_dev_get_data (dev);
  struct roc_nix *nix = cd->nix;
  int rc = 0;

  /* Disable hierarchy */
  rc = roc_nix_tm_hierarchy_disable (nix);
  if (rc)
    {
      rc = -EIO;
      return oct_roc_err (dev, rc, "oct_nix_tm_stop_failed");
    }

  return 0;
}

tm_system_t dev_oct_tm_ops = {
  .node_add = oct_tm_sys_node_add,
  .node_delete = oct_tm_sys_node_delete,
  .node_read_stats = oct_tm_sys_node_read_stats,
  .shaper_profile_create = oct_tm_sys_shaper_profile_create,
  .node_shaper_update = oct_tm_sys_node_shaper_update,
  .shaper_profile_delete = oct_tm_sys_shaper_profile_delete,
  .start_tm = oct_tm_sys_start,
  .stop_tm = oct_tm_sys_stop,
};
