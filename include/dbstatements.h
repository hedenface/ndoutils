/**
 * @file dbstatements.h Database prepared statement declarations for ndo2db
 * daemon
 */
/*
 * Copyright 2014 Nagios Core Development Team and Community Contributors
 *
 * This file is part of NDOUtils.
 *
 * NDOUtils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NDOUtils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NDOUtils. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NDOUTILS_INCLUDE_DBSTATEMENTS_H
#define NDOUTILS_INCLUDE_DBSTATEMENTS_H

/* Bring in the ndo2db_idi type declaration. */
#include "ndo2db.h"


/**
 * Initializes our prepared statements once connected to the database and our
 * instance_id is available.
 * @param idi Input data and DB connection info.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 */
int ndo2db_stmt_init_stmts(ndo2db_idi *idi);
/**
 * Frees resources allocated for prepared statements.
 * @return Currently NDO_OK in all cases.
 */
int ndo2db_stmt_free_stmts(void);


/**
 * Fetches all known objects for an instance from the DB.
 * @param idi Input data and DB connection info.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 * @post It is possible for the object cache to be partially populated if an
 * error occurs while processing results.
 */
int ndo2db_load_obj_cache(ndo2db_idi *idi);
/**
 * Frees resources allocated for the object cache.
 * @param idi Input data and DB connection info.
 * @return Currently NDO_OK in all cases, there are no detectable errors.
 */
int ndo2db_free_obj_cache(ndo2db_idi *idi);
/**
 * Sets all objects as inactive in the DB for the current instance.
 */
int ndo2db_set_all_objs_inactive(ndo2db_idi *idi);


/* Statement handler functions. These are intended to function as the
 * corresponding ndo2db_handle_* functions. */
int ndo2db_stmt_handle_servicecheckdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_hostcheckdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_hoststatusdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_servicestatusdata(ndo2db_idi *idi);

int ndo2db_stmt_handle_contactstatusdata(ndo2db_idi *);

int ndo2db_stmt_handle_adaptiveprogramdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_adaptivehostdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_adaptiveservicedata(ndo2db_idi *idi);
int ndo2db_stmt_handle_adaptivecontactdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_aggregatedstatusdata(ndo2db_idi *idi);
int ndo2db_stmt_handle_retentiondata(ndo2db_idi *idi);

int ndo2db_stmt_handle_configfilevariables(ndo2db_idi *idi, int config_type);
int ndo2db_stmt_handle_configvariables(ndo2db_idi *idi);
int ndo2db_stmt_handle_runtimevariables(ndo2db_idi *idi);

int ndo2db_stmt_handle_configdumpstart(ndo2db_idi *idi);
int ndo2db_stmt_handle_configdumpend(ndo2db_idi *idi);

int ndo2db_stmt_handle_hostdefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_hostgroupdefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_hostdependencydefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_hostescalationdefinition(ndo2db_idi *idi);

int ndo2db_stmt_handle_servicedefinition(ndo2db_idi *);
int ndo2db_stmt_handle_servicegroupdefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_servicedependencydefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_serviceescalationdefinition(ndo2db_idi *idi);

int ndo2db_stmt_handle_commanddefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_timeperiodefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_contactdefinition(ndo2db_idi *idi);
int ndo2db_stmt_handle_contactgroupdefinition(ndo2db_idi *idi);

int ndo2db_stmt_save_customvariables(ndo2db_idi *idi, unsigned long o_id);
int ndo2db_stmt_save_customvariable_status(ndo2db_idi *idi, unsigned long o_id,
		unsigned long t);

#endif
