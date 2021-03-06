/*
 * Copyright 2002, 2009 Seanodes Ltd http://www.seanodes.com. All rights
 * reserved and protected by French, UK, U.S. and other countries' copyright laws.
 * This file is part of Exanodes project and is subject to the terms
 * and conditions defined in the LICENSE file which is present in the root
 * directory of the project.
 */
#ifndef __RPC_COMMAND_H__
#define __RPC_COMMAND_H__

#include "admind/src/adm_error.h"

typedef void (*LocalCommand) (int thr_nb, void *data);
typedef void (*RegisterLocalCommand) (LocalCommand local_commands[]);

typedef enum rpc_command {
 RPC_COMMAND_NULL,
#define RPC_COMMAND_FIRST RPC_ADM_CLDISKADD
 RPC_ADM_CLDISKADD,
 RPC_ADM_CLDISKDEL,
 RPC_ADM_CLINFO_COMPONENTS,
 RPC_ADM_CLINFO_FS,
 RPC_ADM_CLINFO_GROUP_DISK,
 RPC_ADM_CLINFO_NODE_DISKS,
 RPC_ADM_CLINFO_VOLUME,
 RPC_ADM_CLINFO_EXPORT,
 RPC_ADM_CLINFO_GET_NTH_IQN,
 RPC_ADM_CLINFO_DISK_INFO,
#ifdef WITH_MONITORING
 RPC_ADM_CLINFO_MONITORING,
#endif
 RPC_ADM_CLNODEADD_BEGIN,
 RPC_ADM_CLNODEADD_COMMIT,
 RPC_ADM_CLNODEADD_DISK,
 RPC_ADM_CLNODEADD_NETWORK,
 RPC_ADM_CLNODEDEL,
 RPC_ADM_CLNODESTOP,
 RPC_ADM_CLTUNE,
 RPC_ADM_DGCREATE,
 RPC_ADM_DGDELETE,
 RPC_ADM_DGSTART,
 RPC_ADM_DGSTOP,
 RPC_ADM_DGDISKRECOVER,
 RPC_ADM_DGDISKADD,
 RPC_ADM_DGCHECK,
 RPC_ADM_DGRESET,
 RPC_ADM_VLCREATE,
 RPC_ADM_VLDELETE,
 RPC_ADM_VLRESIZE,
 RPC_ADM_VLSTART,
 RPC_ADM_VLSTOP,
 RPC_ADM_VLTUNE_LUN,
 RPC_ADM_VLTUNE_READAHEAD,
 RPC_ADM_VLTUNE_IQNAUTH,
#ifdef WITH_MONITORING
 RPC_ADM_CLMONITORSTART,
 RPC_ADM_CLMONITORSTOP,
#endif
#ifdef WITH_FS
 RPC_ADM_FSCREATE,
#endif
 RPC_SERVICE_ADMIND_GETNBDSTATS,
 RPC_SERVICE_ADMIND_GETVRTSTATS,
 RPC_SERVICE_ADMIND_RECOVER,
 RPC_SERVICE_ADMIND_STOP,
 RPC_SERVICE_ADMIND_CHECK_LICENSE,
 RPC_SERVICE_MONITOR_RECOVER,
#ifdef WITH_FS
 RPC_SERVICE_FS_STOP,
 RPC_SERVICE_FS_STARTSTOP,
 RPC_SERVICE_FS_CONFIG_UPDATE,
 RPC_SERVICE_FS_DEVICE_STATUS,
 RPC_SERVICE_FS_HANDLE_GFS,
 RPC_SERVICE_FS_UPDATE_CMAN,
 RPC_SERVICE_FS_GFS_ADD_LOGS,
 RPC_SERVICE_FS_GFS_UPDATE_TUNING,
 RPC_SERVICE_FS_GENERIC_CHECK,
 RPC_SERVICE_FS_GENERIC_COPY,
 RPC_SERVICE_FS_GFS_CREATE_CONFIG,
 RPC_SERVICE_FS_GENERIC_GROW,
 RPC_SERVICE_FS_SHM_UPDATE,
#endif
 RPC_SERVICE_NBD_RECOVER,
 RPC_SERVICE_NBD_RESUME,
 RPC_SERVICE_NBD_STOP,
 RPC_SERVICE_NBD_SUSPEND,
 RPC_SERVICE_RDEV_CHECK_DOWN,
 RPC_SERVICE_RDEV_RECOVER,
 RPC_SERVICE_RDEV_STOP,
 RPC_SERVICE_VRT_STOP,
 RPC_SERVICE_VRT_CHECK_UP,
 RPC_SERVICE_VRT_RECOVER,
 RPC_SERVICE_VRT_RESUME,
 RPC_SERVICE_VRT_SUSPEND,
 RPC_SERVICE_LUM_SUSPEND,
 RPC_SERVICE_LUM_RESUME,
 RPC_SERVICE_LUM_RECOVER,
 RPC_SERVICE_LUM_STOP,
 RPC_SERVICE_LUM_UNPUBLISH,
 RPC_INST_SET_LEADERABLE,
 RPC_INST_SYNC_BEFORE_RECOVERY,
 RPC_INST_SYNC_AFTER_RECOVERY
#define RPC_COMMAND_LAST  RPC_INST_SYNC_AFTER_RECOVERY
} rpc_command_t;

LocalCommand rpc_command_get(rpc_command_t cmd_id);

void rpc_command_set(rpc_command_t cmd_id, LocalCommand fct);

void rpc_command_resetall(void);

#endif

