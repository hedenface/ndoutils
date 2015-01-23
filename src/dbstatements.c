/**
 * @file dbstatements.c Database prepared statement support for ndo2db daemon
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

/* Headers from our project. */
#include "../include/config.h"
#include "../include/common.h"
#include "../include/utils.h"
#include "../include/protoapi.h"
#include "../include/ndo2db.h"
#include "../include/db.h"
#include "../include/dbstatements.h"

/* Nagios headers. */
#ifdef BUILD_NAGIOS_2X
#include "../include/nagios-2x/nagios.h"
#include "../include/nagios-2x/broker.h"
#include "../include/nagios-2x/comments.h"
#endif
#ifdef BUILD_NAGIOS_3X
#include "../include/nagios-3x/nagios.h"
#include "../include/nagios-3x/broker.h"
#include "../include/nagios-3x/comments.h"
#endif
#ifdef BUILD_NAGIOS_4X
#include "../include/nagios-4x/nagios.h"
#include "../include/nagios-4x/broker.h"
#include "../include/nagios-4x/comments.h"
#endif



/** Our prefixed table names (from db.c). */
extern char *ndo2db_db_tablenames[NDO2DB_NUM_DBTABLES];



/** Default and minimum number of object cache hash slots. */
#define NDO2DB_OBJECT_HASHSLOTS 4096

/** Our type for local object ids: 32-bit unsigned, 32-bit signed in the DB.
 * Previosly the convention was "unsigned long" locally, but that can be 32 or
 * 64-bit realistically. If we consistently use one type, we can get object ids
 * directly into bound storage without intermediate variables in most cases. */
typedef uint32_t ndo2db_id_t;

/** Our hash value type: 64-bit unsigned. */
typedef uint64_t ndo2db_hash_t;

/** Cached object information and hash bucket list node. */
struct ndo2db_object {
	ndo2db_hash_t h;
	char *name1;
	char *name2;
	ndo2db_id_t id;
	int type;
	my_bool is_active;
	struct ndo2db_object *next;
};

/** Our object type,name1,name2 to id hash table and cache. */
static struct ndo2db_object **ndo2db_objects;

/** Allocated object hash table size, the number of buckets. */
static size_t ndo2db_objects_size;
/** Number of cached objects in the hash table. */
static size_t ndo2db_objects_count;
/** Count of activated objects. */
static size_t ndo2db_objects_activated;
/** Count of collided objects. */
static size_t ndo2db_objects_collisions;



/** Prepared statement identifiers/indices and count. */
enum ndo2db_stmt_id {
	/** For when we want to say 'no statement'. The entry in ndo2db_stmts is not
	 * used, and will have empty/null values unless something is wrong. */
	NDO2DB_STMT_NONE = 0,
	NDO2DB_STMT_THE_FIFTH = NDO2DB_STMT_NONE,

	NDO2DB_STMT_GET_OBJ_ID,
	NDO2DB_STMT_GET_OBJ_ID_N2_NULL,
	NDO2DB_STMT_GET_OBJ_ID_INSERT,
	NDO2DB_STMT_GET_OBJ_IDS,
	NDO2DB_STMT_SET_OBJ_ACTIVE,

	NDO2DB_STMT_SAVE_LOG,
	NDO2DB_STMT_FIND_LOG,

	NDO2DB_STMT_HANDLE_PROCESSDATA,
	NDO2DB_STMT_UPDATE_PROCESSDATA_PROGRAMSTATUS,

	NDO2DB_STMT_TIMEDEVENT_ADD,
	NDO2DB_STMT_TIMEDEVENT_EXECUTE,
	NDO2DB_STMT_TIMEDEVENT_REMOVE,
	NDO2DB_STMT_TIMEDEVENTQUEUE_CLEAN,
	NDO2DB_STMT_TIMEDEVENTQUEUE_ADD,
	NDO2DB_STMT_TIMEDEVENTQUEUE_REMOVE,

	NDO2DB_STMT_HANDLE_SYSTEMCOMMAND,
	NDO2DB_STMT_HANDLE_EVENTHANDLER,
	NDO2DB_STMT_HANDLE_NOTIFICATION,
	NDO2DB_STMT_HANDLE_CONTACTNOTIFICATION,
	NDO2DB_STMT_HANDLE_CONTACTNOTIFICATIONMETHOD,

	NDO2DB_STMT_COMMENTHISTORY_ADD,
	NDO2DB_STMT_COMMENTHISTORY_DELETE,
	NDO2DB_STMT_COMMENT_ADD,
	NDO2DB_STMT_COMMENT_DELETE,

	NDO2DB_STMT_DOWNTIMEHISTORY_ADD,
	NDO2DB_STMT_DOWNTIMEHISTORY_START,
	NDO2DB_STMT_DOWNTIMEHISTORY_STOP,
	NDO2DB_STMT_DOWNTIME_ADD,
	NDO2DB_STMT_DOWNTIME_START,
	NDO2DB_STMT_DOWNTIME_STOP,

	NDO2DB_STMT_HANDLE_FLAPPING,
	NDO2DB_STMT_HANDLE_PROGRAMSTATUS,

	NDO2DB_STMT_HANDLE_HOSTCHECK,
	NDO2DB_STMT_HANDLE_SERVICECHECK,
	NDO2DB_STMT_HANDLE_HOSTSTATUS,
	NDO2DB_STMT_HANDLE_SERVICESTATUS,

	NDO2DB_STMT_HANDLE_CONTACTSTATUS,
	NDO2DB_STMT_HANDLE_EXTERNALCOMMAND,
	NDO2DB_STMT_HANDLE_ACKNOWLEDGEMENT,
	NDO2DB_STMT_HANDLE_STATECHANGE,

	NDO2DB_STMT_HANDLE_CONFIGFILE,
	NDO2DB_STMT_SAVE_CONFIGFILEVARIABLE,

	NDO2DB_STMT_HANDLE_RUNTIMEVARIABLE,

	NDO2DB_STMT_HANDLE_HOST,
	NDO2DB_STMT_SAVE_HOSTPARENT,
	NDO2DB_STMT_SAVE_HOSTCONTACTGROUP,
	NDO2DB_STMT_SAVE_HOSTCONTACT,

	NDO2DB_STMT_HANDLE_HOSTGROUP,
	NDO2DB_STMT_SAVE_HOSTGROUPMEMBER,

	NDO2DB_STMT_HANDLE_SERVICE,
#ifdef BUILD_NAGIOS_4X
	NDO2DB_STMT_SAVE_SERVICEPARENT,
#endif
	NDO2DB_STMT_SAVE_SERVICECONTACTGROUP,
	NDO2DB_STMT_SAVE_SERVICECONTACT,

	NDO2DB_STMT_HANDLE_SERVICEGROUP,
	NDO2DB_STMT_SAVE_SERVICEGROUPMEMBER,

	NDO2DB_STMT_HANDLE_HOSTDEPENDENCY,
	NDO2DB_STMT_HANDLE_SERVICEDEPENDENCY,

	NDO2DB_STMT_HANDLE_HOSTESCALATION,
	NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACTGROUP,
	NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACT,

	NDO2DB_STMT_HANDLE_SERVICEESCALATION,
	NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACTGROUP,
	NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACT,

	NDO2DB_STMT_HANDLE_COMMAND,

	NDO2DB_STMT_HANDLE_TIMEPERIOD,
	NDO2DB_STMT_SAVE_TIMEPERIODRANGE,

	NDO2DB_STMT_HANDLE_CONTACT,
	NDO2DB_STMT_SAVE_CONTACTADDRESS,
	NDO2DB_STMT_SAVE_CONTACTNOTIFICATIONCOMMAND,

	NDO2DB_STMT_HANDLE_CONTACTGROUP,
	NDO2DB_STMT_SAVE_CONTACTGROUPMEMBER,

	NDO2DB_STMT_SAVE_CUSTOMVARIABLE,
	NDO2DB_STMT_SAVE_CUSTOMVARIABLESTATUS,

	NDO2DB_NUM_STMTS
};

/** Prepared statement names for debugging info, indexed by id. */
static const char *ndo2db_stmt_names[] = {
	"NDO2DB_STMT_NONE",

	"NDO2DB_STMT_GET_OBJ_ID",
	"NDO2DB_STMT_GET_OBJ_ID_N2_NULL",
	"NDO2DB_STMT_GET_OBJ_ID_INSERT",
	"NDO2DB_STMT_GET_OBJ_IDS",
	"NDO2DB_STMT_SET_OBJ_ACTIVE",

	"NDO2DB_STMT_SAVE_LOG",
	"NDO2DB_STMT_FIND_LOG",

	"NDO2DB_STMT_HANDLE_PROCESSDATA",
	"NDO2DB_STMT_UPDATE_PROCESSDATA_PROGRAMSTATUS",

	"NDO2DB_STMT_TIMEDEVENT_ADD",
	"NDO2DB_STMT_TIMEDEVENT_EXECUTE",
	"NDO2DB_STMT_TIMEDEVENT_REMOVE",
	"NDO2DB_STMT_TIMEDEVENTQUEUE_CLEAN",
	"NDO2DB_STMT_TIMEDEVENTQUEUE_ADD",
	"NDO2DB_STMT_TIMEDEVENTQUEUE_REMOVE",

	"NDO2DB_STMT_HANDLE_SYSTEMCOMMAND",
	"NDO2DB_STMT_HANDLE_EVENTHANDLER",
	"NDO2DB_STMT_HANDLE_NOTIFICATION",
	"NDO2DB_STMT_HANDLE_CONTACTNOTIFICATION",
	"NDO2DB_STMT_HANDLE_CONTACTNOTIFICATIONMETHOD",

	"NDO2DB_STMT_COMMENTHISTORY_ADD",
	"NDO2DB_STMT_COMMENTHISTORY_DELETE",
	"NDO2DB_STMT_COMMENT_ADD",
	"NDO2DB_STMT_COMMENT_DELETE",

	"NDO2DB_STMT_DOWNTIMEHISTORY_ADD",
	"NDO2DB_STMT_DOWNTIMEHISTORY_START",
	"NDO2DB_STMT_DOWNTIMEHISTORY_STOP",
	"NDO2DB_STMT_DOWNTIME_ADD",
	"NDO2DB_STMT_DOWNTIME_START",
	"NDO2DB_STMT_DOWNTIME_STOP",

	"NDO2DB_STMT_HANDLE_FLAPPING",
	"NDO2DB_STMT_HANDLE_PROGRAMSTATUS",

	"NDO2DB_STMT_HANDLE_HOSTCHECK",
	"NDO2DB_STMT_HANDLE_SERVICECHECK",
	"NDO2DB_STMT_HANDLE_HOSTSTATUS",
	"NDO2DB_STMT_HANDLE_SERVICESTATUS",

	"NDO2DB_STMT_HANDLE_CONTACTSTATUS",
	"NDO2DB_STMT_HANDLE_EXTERNALCOMMAND",
	"NDO2DB_STMT_HANDLE_ACKNOWLEDGEMENT",
	"NDO2DB_STMT_HANDLE_STATECHANGE",

	"NDO2DB_STMT_HANDLE_CONFIGFILE",
	"NDO2DB_STMT_SAVE_CONFIGFILEVARIABLE",

	"NDO2DB_STMT_HANDLE_RUNTIMEVARIABLE",

	"NDO2DB_STMT_HANDLE_HOST",
	"NDO2DB_STMT_SAVE_HOSTPARENT",
	"NDO2DB_STMT_SAVE_HOSTCONTACTGROUP",
	"NDO2DB_STMT_SAVE_HOSTCONTACT",

	"NDO2DB_STMT_HANDLE_HOSTGROUP",
	"NDO2DB_STMT_SAVE_HOSTGROUPMEMBER",

	"NDO2DB_STMT_HANDLE_SERVICE",
#ifdef BUILD_NAGIOS_4X
	"NDO2DB_STMT_SAVE_SERVICEPARENT",
#endif
	"NDO2DB_STMT_SAVE_SERVICECONTACTGROUP",
	"NDO2DB_STMT_SAVE_SERVICECONTACT",

	"NDO2DB_STMT_HANDLE_SERVICEGROUP",
	"NDO2DB_STMT_SAVE_SERVICEGROUPMEMBER",

	"NDO2DB_STMT_HANDLE_HOSTDEPENDENCY",
	"NDO2DB_STMT_HANDLE_SERVICEDEPENDENCY",

	"NDO2DB_STMT_HANDLE_HOSTESCALATION",
	"NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACTGROUP",
	"NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACT",

	"NDO2DB_STMT_HANDLE_SERVICEESCALATION",
	"NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACTGROUP",
	"NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACT",

	"NDO2DB_STMT_HANDLE_COMMAND",

	"NDO2DB_STMT_HANDLE_TIMEPERIOD",
	"NDO2DB_STMT_SAVE_TIMEPERIODRANGE",

	"NDO2DB_STMT_HANDLE_CONTACT",
	"NDO2DB_STMT_SAVE_CONTACTADDRESS",
	"NDO2DB_STMT_SAVE_CONTACTNOTIFICATIONCOMMAND",

	"NDO2DB_STMT_HANDLE_CONTACTGROUP",
	"NDO2DB_STMT_SAVE_CONTACTGROUPMEMBER",

	"NDO2DB_STMT_SAVE_CUSTOMVARIABLE",
	"NDO2DB_STMT_SAVE_CUSTOMVARIABLESTATUS",

	"NDO2DB_NUM_STMTS"
};

/** Input binding type codes for our use cases. */
enum bind_data_type {
	BIND_TYPE_BOOL, /* int8_t bind with boolean (0/1) handling / normalization */
	BIND_TYPE_INT8, /* int8_t bind */
	BIND_TYPE_INT16, /* int16_t bind */
	BIND_TYPE_INT32, /* int32_t bind */
	BIND_TYPE_UINT32, /* uint32_t bind */
	BIND_TYPE_DOUBLE, /* double bind */
	BIND_TYPE_SHORT_STRING, /* char[256] bind */
	BIND_TYPE_LONG_STRING, /* char[65536] bind */
	BIND_TYPE_FROM_UNIXTIME, /* uint32_t bind, FROM_UNIXTIME(?) placeholder */
	BIND_TYPE_TV_SEC, /* uint32_t bind, FROM_UNIXTIME(?) placeholder */
	BIND_TYPE_TV_USEC, /* int32_t bind */
	BIND_TYPE_ID, /* ndo2db_id_t bind (currently uint32_t) */
	BIND_TYPE_CURRENT_CONFIG /* idi->current_object_config_type, int8_t bind */
};

/** Additional binding flags for special cases. */
enum bind_flags {
	/** Only insert parameter value in "INSERT ... UPDATE ..." statements. */
	BIND_ONLY_INS = 1,
	/** Bound value can be NULL. */
	BIND_MAYBE_NULL = 2,
	/** Process parameter data from idi->buffered_input to bound storage. */
	BIND_BUFFERED_INPUT = 4
};

/** Bind info for template generation, binding, and data conversion. */
struct ndo2db_stmt_bind {
	/** Binding column name or NULL if not applicable. */
	const char *column;
	/** Binding and handling type information. */
	enum bind_data_type type;
	/** Data conversion index into idi->buffered_input, or -1 to skip auto
	 * data conversion of a parameter. */
	int bi_index;
	/** Additional flags. */
	enum bind_flags flags;
};

/** Prepared statement handle, bindings and parameter/result descriptions. */
struct ndo2db_stmt {
	/** Statement identifier and index. */
	enum ndo2db_stmt_id id;
	/** Prepared statment handle. */
	MYSQL_STMT *handle;

	/** Statement parameter information, assumed to live in static storage. */
	const struct ndo2db_stmt_bind *params;
	/** Prepared statement parameter bindings. Input data should be copied into
	 * the bound buffer using one of the COPY_TO_BIND or COPY_BIND_STRING_*
	 * macros. */
	MYSQL_BIND *param_binds;
	/** Count of parameters. */
	size_t np;

	/** Statement result information, assumed to live in static storage. */
	const struct ndo2db_stmt_bind *results;
	/** Prepared statement result bindings, or NULL for statements that don't
	 * generate a result set. Scalar data can be copied from the bound buffer
	 * using the COPY_FROM_BIND macro. String data can be accessed with a cast
	 * from void*. */
	MYSQL_BIND *result_binds;
	/** Count of results. */
	size_t nr;
};

/** Our prepared statements, indexed by enum ndo2db_stmt_id. */
static struct ndo2db_stmt ndo2db_stmts[NDO2DB_NUM_STMTS];



/** Short string buffer length. */
#define BIND_SHORT_STRING_LENGTH 256
/** Long string buffer length. */
#define BIND_LONG_STRING_LENGTH 65536

/**
 * Static storage for bound parameters and results. The base C to MySQL type
 * mappings we use are:
 *
 * | MySQL docs  | buffer_type       | SQL Type            | ndo2db type |
 * |-------------|-------------------|---------------------|-------------|
 * | signed char | MYSQL_TYPE_TINY   | TINYINT             | int8_t      |
 * | short int   | MYSQL_TYPE_SHORT  | SMALLINT            | int16_t     |
 * | int         | MYSQL_TYPE_LONG   | INT                 | int32_t     |
 * | double      | MYSQL_TYPE_DOUBLE | DOUBLE              | double      |
 * | char[]      | MYSQL_TYPE_STRING | TEXT, CHAR, VARCHAR | char[]      |
 *
 * TINYINT is not used in the schema, but we use int8_t elsewhere for data
 * that will fit.
 */
static int8_t ndo2db_stmt_bind_int8[27];
static int16_t ndo2db_stmt_bind_int16[4];
static int32_t ndo2db_stmt_bind_int32[3];
static uint32_t ndo2db_stmt_bind_uint32[14];
static double ndo2db_stmt_bind_double[9];
static char ndo2db_stmt_bind_short_str[13][BIND_SHORT_STRING_LENGTH];
static char ndo2db_stmt_bind_long_str[2][BIND_LONG_STRING_LENGTH];
/** Static storage for binding metadata, typed for MYSQL_BIND. */
static unsigned long ndo2db_stmt_bind_length[13];
static my_bool ndo2db_stmt_bind_is_null[4];
static my_bool ndo2db_stmt_bind_error[4];

/** Maximum bound buffer usage counts across all statements. Intended to help
 * determine the number of ndo2db_stmt_bind_* buffers needed, and check that
 * availability is not exceeded. */
static size_t ndo2db_stmt_bind_n_int8;
static size_t ndo2db_stmt_bind_n_int16;
static size_t ndo2db_stmt_bind_n_int32;
static size_t ndo2db_stmt_bind_n_uint32;
static size_t ndo2db_stmt_bind_n_double;
static size_t ndo2db_stmt_bind_n_short_str;
static size_t ndo2db_stmt_bind_n_long_str;
static size_t ndo2db_stmt_bind_n_length;
static size_t ndo2db_stmt_bind_n_is_null;
static size_t ndo2db_stmt_bind_n_error;

/** Resets maximum bound buffer type usage counts. */
#define RESET_BOUND_BUFFER_COUNTS \
	ndo2db_stmt_bind_n_int8 = 0, \
	ndo2db_stmt_bind_n_int16 = 0, \
	ndo2db_stmt_bind_n_int32 = 0, \
	ndo2db_stmt_bind_n_uint32 = 0, \
	ndo2db_stmt_bind_n_double = 0, \
	ndo2db_stmt_bind_n_short_str = 0, \
	ndo2db_stmt_bind_n_long_str = 0, \
	ndo2db_stmt_bind_n_length = 0, \
	ndo2db_stmt_bind_n_is_null = 0, \
	ndo2db_stmt_bind_n_error = 0

/** Reports maximum and available counts for a bound buffer type. */
#define REPORT_BOUND_BUFFER_USAGE(msg_pre, type) \
	do { \
		size_t array_size = ARRAY_SIZE(ndo2db_stmt_bind_ ## type); \
		int d = (int)(ndo2db_stmt_bind_n_ ## type) - (int)array_size; \
		ndo2db_log_debug_info(NDO2DB_DEBUGL_STMT, 0, \
				"%s: n_%s=%zu %s ARRAY_SIZE(ndo2db_stmt_bind_%s)=%zu, d=%d\n", \
				msg_pre, #type, ndo2db_stmt_bind_n_ ## type, \
				((d > 0) ? ">" : (d < 0) ? "<" : "=="), \
				#type, array_size, d); \
	} while (0)

/** Reports maximum and available counts for all bound buffer types. */
#define REPORT_BOUND_BUFFER_USAGES(msg_pre) \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, int8); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, int16); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, int32); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, uint32); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, double); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, short_str); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, long_str); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, length); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, is_null); \
	REPORT_BOUND_BUFFER_USAGE(msg_pre, error) \



/**
 * ndo2db_stmt_init_*() prepared statement initialization function type.
 * This type exists to document the interface and define the initializer array,
 * it's not used otherwise.
 * @param idi Input data and DB connection handle.
 * @param dbuf Temporary dynamic buffer for printing statement templates.
 * @return NDO_OK on success, NDO_ERROR otherwise.
 */
typedef int (*ndo2db_stmt_initializer)(ndo2db_idi *idi, ndo_dbuf *dbuf);

/** Declare a prepared statement initialization function. */
#define NDO_DECLARE_STMT_INITIALIZER(f) \
	static int f(ndo2db_idi *idi, ndo_dbuf *dbuf)

NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_obj);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_log);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_processdata);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_timedevent);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_systemcommand);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_eventhandler);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_notification);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_contactnotification);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_contactnotificationmethod);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_comment);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_downtime);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_flapping);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_programstatus);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_hostcheck);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_servicecheck);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_hoststatus);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_servicestatus);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_contactstatus);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_externalcommand);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_acknowledgement);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_statechange);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_configfile);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_runtimevariable);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_host);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_hostgroup);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_service);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_servicegroup);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_hostdependency);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_servicedependency);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_hostescalation);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_serviceescalation);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_command);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_timeperiod);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_contact);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_contactgroup);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_customvariable);
NDO_DECLARE_STMT_INITIALIZER(ndo2db_stmt_init_customvariablestatus);

#undef NDO_DECLARE_STMT_INITIALIZER

/** Prepared statement initializer functions. Order in this list does not
 * matter: ndo2db_stmt_init_stmts() doesn't use statement ids, the initializers
 * themselves know the statement ids they need. After prefixing table names,
 * and connecting to the DB and obtaining an instance_id, executing all these
 * functions will initialize all ndo2db_stmts. Generally, ndo2db_stmt_init_x
 * initializes ndo2db_stmts[NDO2DB_STMT_HANDLE_X] and/or any related
 * ndo2db_stmts[NDO2DB_STMT_SAVE_XY]. */
static const ndo2db_stmt_initializer ndo2db_stmt_initializers[] = {
	ndo2db_stmt_init_obj,
	/* ...NDO2DB_STMT_GET_OBJ_ID */
	/* ...NDO2DB_STMT_GET_OBJ_ID_N2_NULL */
	/* ...NDO2DB_STMT_GET_OBJ_ID_INSERT */
	/* ...NDO2DB_STMT_GET_OBJ_IDS */
	/* ...NDO2DB_STMT_SET_OBJ_ACTIVE */

	ndo2db_stmt_init_log,
	/* ...NDO2DB_STMT_SAVE_LOG */
	/* ...NDO2DB_STMT_FIND_LOG */

	ndo2db_stmt_init_processdata,
	/* ...NDO2DB_STMT_HANDLE_PROCESSDATA */
	/* ...NDO2DB_STMT_UPDATE_PROCESSDATA_PROGRAMSTATUS */

	ndo2db_stmt_init_timedevent,
	/* ...NDO2DB_STMT_TIMEDEVENT_ADD */
	/* ...NDO2DB_STMT_TIMEDEVENT_EXECUTE */
	/* ...NDO2DB_STMT_TIMEDEVENT_REMOVE */
	/* ...NDO2DB_STMT_TIMEDEVENTQUEUE_CLEAN */
	/* ...NDO2DB_STMT_TIMEDEVENTQUEUE_ADD */
	/* ...NDO2DB_STMT_TIMEDEVENTQUEUE_REMOVE */

	ndo2db_stmt_init_systemcommand,
	ndo2db_stmt_init_eventhandler,
	ndo2db_stmt_init_notification,
	ndo2db_stmt_init_contactnotification,
	ndo2db_stmt_init_contactnotificationmethod,

	ndo2db_stmt_init_comment,

	ndo2db_stmt_init_downtime,

	ndo2db_stmt_init_flapping,
	ndo2db_stmt_init_programstatus,

	ndo2db_stmt_init_hostcheck,
	ndo2db_stmt_init_servicecheck,
	ndo2db_stmt_init_hoststatus,
	ndo2db_stmt_init_servicestatus,

	ndo2db_stmt_init_contactstatus,
	ndo2db_stmt_init_externalcommand,
	ndo2db_stmt_init_acknowledgement,
	ndo2db_stmt_init_statechange,

	ndo2db_stmt_init_configfile,
	ndo2db_stmt_init_runtimevariable,

	ndo2db_stmt_init_host,
	/* ...NDO2DB_STMT_SAVE_HOSTPARENT */
	/* ...NDO2DB_STMT_SAVE_HOSTCONTACTGROUP */
	/* ...NDO2DB_STMT_SAVE_HOSTCONTACT */

	ndo2db_stmt_init_hostgroup,
	/* ...NDO2DB_STMT_SAVE_HOSTGROUPMEMBER */

	ndo2db_stmt_init_service,
	/* ...NDO2DB_STMT_SAVE_SERVICEPARENT for BUILD_NAGIOS_4X */
	/* ...NDO2DB_STMT_SAVE_SERVICECONTACTGROUP */
	/* ...NDO2DB_STMT_SAVE_SERVICECONTACT */

	ndo2db_stmt_init_servicegroup,
	/* ...NDO2DB_STMT_SAVE_SERVICEGROUPMEMBER */

	ndo2db_stmt_init_hostdependency,
	ndo2db_stmt_init_servicedependency,

	ndo2db_stmt_init_hostescalation,
	/* ...NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACTGROUP */
	/* ...NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACT */

	ndo2db_stmt_init_serviceescalation,
	/* ...NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACTGROUP */
	/* ...NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACT */

	ndo2db_stmt_init_command,

	ndo2db_stmt_init_timeperiod,
	/* ...NDO2DB_STMT_SAVE_TIMEPERIODRANGE */

	ndo2db_stmt_init_contact,
	/* ...NDO2DB_STMT_SAVE_CONTACTADDRESS */
	/* ...NDO2DB_STMT_SAVE_CONTACTNOTIFICATIONCOMMAND */

	ndo2db_stmt_init_contactgroup,
	/* ...NDO2DB_STMT_SAVE_CONTACTGROUPMEMBER */

	ndo2db_stmt_init_customvariable,
	ndo2db_stmt_init_customvariablestatus
};



/**
 * Copies a scalar to a bound buffer, casting as needed.
 * @param v Source value.
 * @param b Destination MYSQL_BIND.
 * @param bt Destination bound buffer type to cast to.
 */
#define COPY_TO_BIND(v, b, bt) \
	(*(bt *)(b).buffer = (bt)(v))

#define TO_BOUND_BOOL_NORMALIZED(v, b) COPY_TO_BIND(!!(v), b, int8_t)
#define TO_BOUND_BOOL(v, b) COPY_TO_BIND(v, b, int8_t)
#define TO_BOUND_INT8(v, b) COPY_TO_BIND(v, b, int8_t)
#define TO_BOUND_INT16(v, b) COPY_TO_BIND(v, b, int16_t)
#define TO_BOUND_INT32(v, b) COPY_TO_BIND(v, b, int32_t)
#define TO_BOUND_UINT32(v, b) COPY_TO_BIND(v, b, uint32_t)
#define TO_BOUND_ID(v, b) COPY_TO_BIND(v, b, ndo2db_id_t)
#define TO_BOUND_DOUBLE(v, b) COPY_TO_BIND(v, b, double)

/**
 * Casts and dereferences a bound buffer to a scalar r-value of its bound type.
 * @param b Source MYSQL_BIND.
 * @param bt Source bound buffer type to cast from.
 */
#define COPY_FROM_BIND(b, bt) \
	(*(bt *)(b).buffer)

#define FROM_BOUND_INT8(b) COPY_FROM_BIND(b, int8_t)
#define FROM_BOUND_INT32(b) COPY_FROM_BIND(b, int32_t)
#define FROM_BOUND_ID(b) COPY_FROM_BIND(b, ndo2db_id_t)

/**
 * Copies a non-null string v into bound storage. Strings longer than the
 * destination buffer are truncated.
 * @param v The input string to copy from.
 * @param b The MYSQL_BIND for the parameter to copy to.
 * @post b.buffer is a null-terminated string of strlen *b.length.
 */
#define COPY_BIND_STRING_NOT_EMPTY(v, b) \
	do { \
		unsigned long n_ = (unsigned long)strlen((v)); \
		*(b).length = (n_ < (b).buffer_length) ? n_ : (b).buffer_length - 1; \
		memcpy((b).buffer, (v), (size_t)*(b).length); \
		((char *)(b).buffer)[*(b).length] = '\0'; \
	} while (0)

/**
 * Copies a string v into bound storage, defaulting to empty if v is null.
 * Strings longer than the destination buffer are truncated.
 * @param v The input string to copy from.
 * @param b The MYSQL_BIND for the buffer to copy to.
 * @post b.buffer is a null-terminated string of strlen *b.length.
 */
#define COPY_BIND_STRING_OR_EMPTY(v, b) \
	do { \
		if ((v) && *(v)) { \
			COPY_BIND_STRING_NOT_EMPTY((v), (b)); \
		} \
		else { \
			*(b).length = 0; \
			((char *)(b).buffer)[0] = '\0'; \
		} \
	} while (0)

/**
 * Copies a possibly null string v into bound storage. Strings longer than the
 * destination buffer are truncated.
 * @param v The input string to copy from.
 * @param b The MYSQL_BIND for the buffer to copy to.
 * @post b.buffer is a null-terminated string of strlen *b.length; and
 * *b.is_null is 1 if v was null, 0 if not.
 */
#define COPY_BIND_STRING_OR_NULL(v, b) \
	do { \
		COPY_BIND_STRING_OR_EMPTY((v), (b)); \
		*(b).is_null = !(v); \
	} while (0)

/**
 * Copies a timeval into bound storage.
 * @param tv struct timeval to copy.
 * @param bs uint32 MYSQL_BIND for tv.tv_sec.
 * @param bu int32 MYSQL_BIND for tv.tv_usec.
 */
#define COPY_TV_TO_BOUND_TV(tv, bs, bu) \
	do { \
		TO_BOUND_UINT32((tv).tv_sec, (bs)); \
		TO_BOUND_INT32((tv).tv_usec, (bu)); \
	} while (0)



/**
 * Evaluates to the number of elements in an array of known size. Note this is
 * for "type x[N]" with N a compile-time constant, not "type *x".
 */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif


/**
 * Checks the status of an expression and resurns the status if not ok.
 */
#define CHK_OK(expression) \
	do { \
		int status_ = (expression); \
		if (status_ != NDO_OK) return status_; \
	} while (0)

/**
 * Saves the error status of an expression.
 */
#define SAVE_ERR(status, expression) \
	do { \
		int status_ = (expression); \
		if (status_ != NDO_OK) status = status_; \
	} while (0)



/** Expands to a checked "var = strto;" conversion for defining
 * ndo_checked_strto{d|l|ul}(). */
#define CHECKED_STRING_TO(strto, str, var) \
	char *endptr = NULL; \
	if (!str || !*str) { \
		var = 0; \
		return NDO_ERROR; \
	} \
	errno = 0; \
	var = strto; \
	return (errno || endptr == str) ? NDO_ERROR : NDO_OK

inline static int ndo_checked_strtod(const char *str, double *d) {
	CHECKED_STRING_TO(strtod(str, &endptr), str, *d);
}

inline static int ndo_checked_strtoul(const char *str, unsigned long *ul) {
	CHECKED_STRING_TO(strtoul(str, &endptr, 10), str, *ul);
}

inline static int ndo_checked_strtol(const char *str, long *l) {
	CHECKED_STRING_TO(strtol(str, &endptr, 10), str, *l);
}

#undef CHECKED_STRING_TO


/** Expands to a checked "var = (vt)strtoul(...);" conversion for defining
 * ndo_checked_strtouint32(). */
#define CHECKED_STRING_TO_UNSIGNED(vt, v, max) \
	unsigned long ul; \
	int st = ndo_checked_strtoul(str, &ul); \
	v = (vt)ul; \
	return (st != NDO_OK) ? st : (ul <= max) ? NDO_OK : NDO_ERROR

/** Expands to a checked "var = (vt)strtol(...);" conversion for defining
 * ndo_checked_strtoint{8|16|32}(). */
#define CHECKED_STRING_TO_SIGNED(vt, v, min, max) \
	long l; \
	int st = ndo_checked_strtol(str, &l); \
	v = (vt)l; \
	return (st != NDO_OK) ? st : (min <= l && l <= max) ? NDO_OK : NDO_ERROR

inline static int ndo_checked_strtouint32(const char *str, uint32_t *u) {
	CHECKED_STRING_TO_UNSIGNED(uint32_t, *u, UINT32_MAX);
}

inline static int ndo_checked_strtoint32(const char *str, int32_t *i) {
	CHECKED_STRING_TO_SIGNED(int32_t, *i, INT32_MIN, INT32_MAX);
}

inline static int ndo_checked_strtoint16(const char *str, int16_t *s) {
	CHECKED_STRING_TO_SIGNED(int16_t, *s, INT16_MIN, INT16_MAX);
}

inline static int ndo_checked_strtoint8(const char *str, int8_t *c) {
	CHECKED_STRING_TO_SIGNED(int8_t, *c, INT8_MIN, INT8_MAX);
}

#undef CHECKED_STRING_TO_SIGNED
#undef CHECKED_STRING_TO_UNSIGNED


/**
 * Converts a string in decimal "seconds[.useconds]" format to a timeval. If
 * present, useconds should be six digits with leading zeroes if needed.
 * @param str Source string.
 * @param tv Destination timeval.
 * @return NDO_ERROR if str is empty or there was a conversion or format error.
 * @post All parts of tv are set to 0 or converted values.
 * @note The strtoul and conversion to time_t are a bit dodgy, but should be
 * reasonable for our expected data for a few decades, and the types/sizes
 * would need to be sorted out on the DB side for wider data too...
 */
static int ndo_checked_strtotv(const char *str, struct timeval *tv) {
	char *endptr;
	unsigned long ul;
	int status;

	if (!str || !*str) {
		tv->tv_sec = 0;
		tv->tv_usec = 0;
		return NDO_ERROR;
	}

	endptr = NULL;
	errno = 0;
	ul = strtoul(str, &endptr, 10);
	/* We're okay if there is no errno set, and the whole string was converted
	 * or it was converted up to a '.' with the remaining part a usecs string. */
	status = (errno || (*endptr != '\0' && *endptr != '.')) ? NDO_ERROR : NDO_OK;

	tv->tv_sec = (time_t)ul;

	/* We're done if there was an error in conversion or no usecs part. */
	if (status != NDO_OK || *endptr == '\0') {
		tv->tv_usec = 0;
		return status;
	}

	str = endptr + 1;
	endptr = NULL;
	errno = 0;
	ul = strtoul(str, &endptr, 10);
	/* Okay if usecs string converted completely without error. */
	status = (errno || *endptr != '\0') ? NDO_ERROR : NDO_OK;

	tv->tv_usec = (suseconds_t)ul;

	return status;
}


/**
 * Converts standard data elements for an NDO protocol item. All conversions
 * are attempted even if one fails.
 * @param idi Input data and DB info.
 * @param type Output item type.
 * @param flags Output item flags.
 * @param attr Output item attributes.
 * @param tstamp Output item timestamp.
 * @return The error status of the last conversion (in argument order) to fail,
 * or NDO_OK.
 * @post All data are set to 0 or converted values.
 */
static int ndo2db_convert_standard_data(ndo2db_idi *idi, int32_t *type,
		int32_t *flags, int32_t *attr, struct timeval *tstamp) {
	char **bi = idi->buffered_input;
	int status = NDO_OK;

	SAVE_ERR(status, ndo_checked_strtoint32(bi[NDO_DATA_TYPE], type));
	SAVE_ERR(status, ndo_checked_strtoint32(bi[NDO_DATA_FLAGS], flags));
	SAVE_ERR(status, ndo_checked_strtoint32(bi[NDO_DATA_ATTRIBUTES], attr));
	SAVE_ERR(status, ndo_checked_strtotv(bi[NDO_DATA_TIMESTAMP], tstamp));

	return status;
}

/** Declares standard handler data: type, flags, attr, and tstamp. */
#define DECLARE_STD_DATA \
	int32_t type, flags, attr; struct timeval tstamp

/** Converts standard handler data from idi->buffered_input. */
#define CONVERT_STD_DATA \
	ndo2db_convert_standard_data(idi, &type, &flags, &attr, &tstamp)

/** Declares and converts standard handler data. */
#define DECLARE_AND_CONVERT_STD_DATA \
	DECLARE_STD_DATA; CONVERT_STD_DATA

/** Returns ND_OK if standard handler data tstamp older than most recent
 * realtime data. */
#define RETURN_OK_IF_STD_DATA_TOO_OLD \
	if (tstamp.tv_sec < idi->dbinfo.latest_realtime_data_time) return NDO_OK

/** Declares and converts standard handler data, returns if we've seen more
 * recent realtime data. */
#define DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD \
	DECLARE_AND_CONVERT_STD_DATA; RETURN_OK_IF_STD_DATA_TOO_OLD




/**
 * Initializes our prepared statements once connected to the database and our
 * instance_id is available (the caller must ensure this).
 * @param idi Input data and DB connection info.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 */
int ndo2db_stmt_init_stmts(ndo2db_idi *idi) {
	/* Caller assures idi is non-null. */
	size_t i;
	ndo_dbuf dbuf;
	ndo_dbuf_init(&dbuf, 2048);
	int status = NDO_OK;
	RESET_BOUND_BUFFER_COUNTS;

	for (i = 0; i < ARRAY_SIZE(ndo2db_stmt_initializers); ++i) {
		/* Skip any empty initializer slots, there shouldn't be any... */
		if (!ndo2db_stmt_initializers[i]) continue;
		/* Reset our dbuf, then initialize. */
		ndo_dbuf_reset(&dbuf);
		status = ndo2db_stmt_initializers[i](idi, &dbuf);
		if (status != NDO_OK) {
			syslog(LOG_USER|LOG_ERR, "ndo2db_stmt_initializers[%zu] failed.", i);
			syslog(LOG_USER|LOG_ERR, "mysql_error: %s", mysql_error(&idi->dbinfo.mysql_conn));
			ndo2db_stmt_free_stmts();
			goto init_stmts_exit;
		}
	}

	/* Report on our bound buffer usage: is it just right, or do we have unused
	 * buffers? Too few will be logged and returned as an error during binding,
	 * which will skip this and go straight to init_stmts_exit. */
	REPORT_BOUND_BUFFER_USAGES("ndo2db_stmt_init_stmts");

init_stmts_exit:
	ndo_dbuf_free(&dbuf); /* Be sure to free memory we've allocated. */
	return status;
}


/**
 * Frees resources allocated for prepared statements.
 * @return Currently NDO_OK in all cases.
 */
int ndo2db_stmt_free_stmts(void) {
	size_t i;

	for (i = 0; i < NDO2DB_NUM_STMTS; ++i) {
		if (ndo2db_stmts[i].handle) mysql_stmt_close(ndo2db_stmts[i].handle);
		free(ndo2db_stmts[i].param_binds);
		free(ndo2db_stmts[i].result_binds);
	}

	/* Reset our statements and usage counters to initial 'null' values. */
	memset(ndo2db_stmts, 0, sizeof(ndo2db_stmts));
	RESET_BOUND_BUFFER_COUNTS;

	return NDO_OK;
}




/**
 * Prints an "INSERT INTO ..." statment template.
 * @param idi Input data and DB connection info.
 * @param dbuf Dynamic buffer for printing statement templates, reset by caller.
 * @param table Prefixed table name.
 * @param params Parameter info (column name, datatype, flags, etc.).
 * @param np Number of parameters.
 * @param up_on_dup Non-zero to add an "ON DUPLICATE KEY UPDATE ..." clause.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 */
static int ndo2db_stmt_print_insert(
	ndo2db_idi *idi,
	ndo_dbuf *dbuf,
	const char *table,
	const struct ndo2db_stmt_bind *params,
	size_t np,
	my_bool up_on_dup
) {
	size_t i;

	CHK_OK(ndo_dbuf_printf(dbuf, "INSERT INTO %s (instance_id", table));

	for (i = 0; i < np; ++i) {
		CHK_OK(ndo_dbuf_printf(dbuf, ",%s", params[i].column));
	}

	CHK_OK(ndo_dbuf_printf(dbuf, ") VALUES (%lu", idi->dbinfo.instance_id));

	for (i = 0; i < np; ++i) {
		CHK_OK(ndo_dbuf_strcat(dbuf, (params[i].type == BIND_TYPE_FROM_UNIXTIME
				|| params[i].type == BIND_TYPE_TV_SEC) ? ",FROM_UNIXTIME(?)" : ",?"));
	}

	CHK_OK(ndo_dbuf_strcat(dbuf, ")"));

	if (up_on_dup) {
		CHK_OK(ndo_dbuf_strcat(dbuf,
				" ON DUPLICATE KEY UPDATE instance_id=VALUES(instance_id)"));

		for (i = 0; i < np; ++i) {
			/* Skip update values for insert only parameters. */
			if (params[i].flags & BIND_ONLY_INS) continue;
			CHK_OK(ndo_dbuf_printf(dbuf,
					",%s=VALUES(%s)", params[i].column, params[i].column));
		}
	}

	return NDO_OK;
}



/** Sets a usage counter to the maximum of current and new values. */
#define UPDATE_BOUND_BUFFER_USAGE(new_count, global_count) \
	if (new_count > global_count) global_count = new_count

/** Updates global maximum buffer usage for a type, and logs and returns error
 * if the given count of used bound buffers exceeds the number available. For
 * use by ndo2db_stmt_bind_params() and ndo2db_stmt_bind_results(). */
#define CHECK_BOUND_BUFFER_USAGE(num, type) \
	do { \
		const size_t array_size = ARRAY_SIZE(ndo2db_stmt_bind_ ## type); \
		UPDATE_BOUND_BUFFER_USAGE(num, ndo2db_stmt_bind_n_ ## type); \
		if (num > array_size) { \
			syslog(LOG_USER|LOG_ERR, \
					"%s %s=%zu > ARRAY_SIZE(ndo2db_stmt_bind_%s)=%zu", \
					ndo2db_stmt_names[stmt->id], #num, num, #type, array_size); \
			return NDO_ERROR; \
		} \
	} while (0)


/**
 * Allocates, initializes and binds a prepared statment's input parameter
 * bindings. Frees any existing bindings for the statement.
 * @param stmt Statement to bind.
 * @return NDO_OK on success, NDO_ERROR otherwise.
 */
static int ndo2db_stmt_bind_params(struct ndo2db_stmt *stmt) {
	size_t i;
	size_t n_int8 = 0;
	size_t n_int16 = 0;
	size_t n_int32 = 0;
	size_t n_uint32 = 0;
	size_t n_double = 0;
	size_t n_short_str = 0;
	size_t n_long_str = 0;
	size_t n_length = 0;
	size_t n_is_null = 0;

	/* Allocate our parameter bindings with null values, free any existing. */
	free(stmt->param_binds);
	stmt->param_binds = calloc(stmt->np, sizeof(MYSQL_BIND));
	if (!stmt->param_binds) return NDO_ERROR;

	/* Setup the bind description structures for the parameters. */
	for (i = 0; i < stmt->np; ++i) {
		const struct ndo2db_stmt_bind *param = stmt->params + i;
		MYSQL_BIND *bind = stmt->param_binds + i;

		switch (param->type) {

		case BIND_TYPE_BOOL:
		case BIND_TYPE_CURRENT_CONFIG:
		case BIND_TYPE_INT8:
			bind->buffer_type = MYSQL_TYPE_TINY;
			bind->buffer = ndo2db_stmt_bind_int8 + n_int8++;
			break;

		case BIND_TYPE_INT16:
			bind->buffer_type = MYSQL_TYPE_SHORT;
			bind->buffer = ndo2db_stmt_bind_int16 + n_int16++;
			break;

		case BIND_TYPE_TV_USEC:
			/* suseconds_t is signed and so are our columns. Do a basic sanity check
			 * and fall through to set up a int32 binding. */
			if (i == 0 || param[-1].type != BIND_TYPE_TV_SEC) {
				syslog(LOG_USER|LOG_ERR, "ndo2db_stmt_bind_params: %s params[%zu]: "
						"BIND_TYPE_TV_USEC must follow BIND_TYPE_TV_SEC.",
						ndo2db_stmt_names[stmt->id], i);
				return NDO_ERROR;
			}
		case BIND_TYPE_INT32:
			/* MySQL says MYSQL_TYPE_LONG, their docs say int, int32 is implied. */
			bind->buffer_type = MYSQL_TYPE_LONG;
			bind->buffer = ndo2db_stmt_bind_int32 + n_int32++;
			break;

		case BIND_TYPE_TV_SEC:
			/* time_t is signed but NDO has had a convention of unsigned for times...
			 * Do a basic sanity check and fall through to set up a uint32 binding. */
			if (i == stmt->np - 1 || param[+1].type != BIND_TYPE_TV_USEC) {
				syslog(LOG_USER|LOG_ERR, "ndo2db_stmt_bind_params: %s params[%zu]: "
						"BIND_TYPE_TV_SEC must be followed by BIND_TYPE_TV_USEC.",
						ndo2db_stmt_names[stmt->id], i);
				return NDO_ERROR;
			}
		case BIND_TYPE_FROM_UNIXTIME: /* Timestamps are bound as uint32. */
		case BIND_TYPE_ID: /* This needs to change if ndo2db_id_t does. */
		case BIND_TYPE_UINT32:
			bind->buffer_type = MYSQL_TYPE_LONG;
			bind->buffer = ndo2db_stmt_bind_uint32 + n_uint32++;
			bind->is_unsigned = 1;
			break;

		case BIND_TYPE_DOUBLE:
			bind->buffer_type = MYSQL_TYPE_DOUBLE;
			bind->buffer = ndo2db_stmt_bind_double + n_double++;
			break;

		case BIND_TYPE_SHORT_STRING:
			bind->buffer_type = MYSQL_TYPE_STRING;
			bind->buffer = ndo2db_stmt_bind_short_str[n_short_str++];
			bind->buffer_length = BIND_SHORT_STRING_LENGTH;
			bind->length = ndo2db_stmt_bind_length + n_length++;
			break;

		case BIND_TYPE_LONG_STRING:
			bind->buffer_type = MYSQL_TYPE_STRING;
			bind->buffer = ndo2db_stmt_bind_long_str[n_long_str++];
			bind->buffer_length = BIND_LONG_STRING_LENGTH;
			bind->length = ndo2db_stmt_bind_length + n_length++;
			break;

		default:
			syslog(LOG_USER|LOG_ERR,
					"ndo2db_stmt_bind_params: %s params[%zu] has bad type %d.",
					ndo2db_stmt_names[stmt->id], i, param->type);
			return NDO_ERROR;
		}

		if (param->flags & BIND_MAYBE_NULL) {
			bind->is_null = ndo2db_stmt_bind_is_null + n_is_null++;
		}
	}

	/* Check our bound buffer usage is within limits. */
	ndo2db_log_debug_info(NDO2DB_DEBUGL_STMT, 0, "ndo2db_stmt_bind_params: %s "
			"n_int8=%zu, n_int16=%zu, n_int32=%zu, n_uint32=%zu, n_double=%zu, "
			"n_short_str=%zu, n_long_str=%zu, n_length=%zu, n_is_null=%zu\n",
			ndo2db_stmt_names[stmt->id], n_int8, n_int16, n_int32, n_uint32, n_double,
			n_short_str, n_long_str, n_length, n_is_null);
	CHECK_BOUND_BUFFER_USAGE(n_int8, int8);
	CHECK_BOUND_BUFFER_USAGE(n_int16, int16);
	CHECK_BOUND_BUFFER_USAGE(n_int32, int32);
	CHECK_BOUND_BUFFER_USAGE(n_uint32, uint32);
	CHECK_BOUND_BUFFER_USAGE(n_double, double);
	CHECK_BOUND_BUFFER_USAGE(n_short_str, short_str);
	CHECK_BOUND_BUFFER_USAGE(n_long_str, long_str);
	CHECK_BOUND_BUFFER_USAGE(n_length, length);
	CHECK_BOUND_BUFFER_USAGE(n_is_null, is_null);

	/* Now bind our statement parameters. */
	return mysql_stmt_bind_param(stmt->handle, stmt->param_binds)
			? NDO_ERROR : NDO_OK;
}


/**
 * Allocates, initializes and binds a prepared statment's output result
 * bindings. Frees any existing result bindings for the statement.
 * @param stmt Statement to bind.
 * @return NDO_OK on success, NDO_ERROR otherwise.
 */
static int ndo2db_stmt_bind_results(struct ndo2db_stmt *stmt) {
	size_t i;
	size_t n_int8 = 0;
	size_t n_int16 = 0;
	size_t n_int32 = 0;
	size_t n_uint32 = 0;
	size_t n_double = 0;
	size_t n_short_str = 0;
	size_t n_long_str = 0;

	/* Allocate our result bindings with null values, free any existing. */
	free(stmt->result_binds);
	stmt->result_binds = calloc(stmt->nr, sizeof(MYSQL_BIND));
	if (!stmt->result_binds) return NDO_ERROR;

	/* Setup the result bind descriptions. */
	for (i = 0; i < stmt->nr; ++i) {
		MYSQL_BIND *bind = stmt->result_binds + i;

		switch (stmt->results[i].type) {

		case BIND_TYPE_BOOL:
		case BIND_TYPE_INT8:
			bind->buffer_type = MYSQL_TYPE_TINY;
			bind->buffer = ndo2db_stmt_bind_int8 + n_int8++;
			break;

		case BIND_TYPE_INT16:
			bind->buffer_type = MYSQL_TYPE_SHORT;
			bind->buffer = ndo2db_stmt_bind_int16 + n_int16++;
			break;

		case BIND_TYPE_INT32:
			bind->buffer_type = MYSQL_TYPE_LONG;
			bind->buffer = ndo2db_stmt_bind_int32 + n_int32++;
			break;

		case BIND_TYPE_FROM_UNIXTIME: /* Timestamps are bound as uint32. */
		case BIND_TYPE_ID: /* This needs to change if ndo2db_id_t does. */
		case BIND_TYPE_UINT32:
			bind->buffer_type = MYSQL_TYPE_LONG;
			bind->buffer = ndo2db_stmt_bind_uint32 + n_uint32++;
			bind->is_unsigned = 1;
			break;

		case BIND_TYPE_DOUBLE:
			bind->buffer_type = MYSQL_TYPE_DOUBLE;
			bind->buffer = ndo2db_stmt_bind_double + n_double++;
			break;

		case BIND_TYPE_SHORT_STRING:
			bind->buffer_type = MYSQL_TYPE_STRING;
			bind->buffer = ndo2db_stmt_bind_short_str[n_short_str++];
			bind->buffer_length = BIND_SHORT_STRING_LENGTH;
			break;

		case BIND_TYPE_LONG_STRING:
			bind->buffer_type = MYSQL_TYPE_STRING;
			bind->buffer = ndo2db_stmt_bind_long_str[n_long_str++];
			bind->buffer_length = BIND_LONG_STRING_LENGTH;
			break;

		default:
			syslog(LOG_USER|LOG_ERR,
					"ndo2db_stmt_bind_results: %s results[%zu] has bad type %d.",
					ndo2db_stmt_names[stmt->id], i, stmt->results[i].type);
			return NDO_ERROR;
		}

		/* Every reult has these metadata. */
		bind->length = ndo2db_stmt_bind_length + i;
		bind->is_null = ndo2db_stmt_bind_is_null + i;
		bind->error = ndo2db_stmt_bind_error + i;
	}

	/* Check our bound buffer usage is within limits. */
	ndo2db_log_debug_info(NDO2DB_DEBUGL_STMT, 0, "ndo2db_stmt_bind_results: %s "
			"n_int8=%zu, n_int16=%zu, n_int32=%zu, n_uint32=%zu, n_double=%zu, "
			"n_short_str=%zu, n_long_str=%zu, nr=i=%zu\n",
			ndo2db_stmt_names[stmt->id], n_int8, n_int16, n_int32, n_uint32, n_double,
			n_short_str, n_long_str, i);
	CHECK_BOUND_BUFFER_USAGE(n_int8, int8);
	CHECK_BOUND_BUFFER_USAGE(n_int16, int16);
	CHECK_BOUND_BUFFER_USAGE(n_int32, int32);
	CHECK_BOUND_BUFFER_USAGE(n_uint32, uint32);
	CHECK_BOUND_BUFFER_USAGE(n_double, double);
	CHECK_BOUND_BUFFER_USAGE(n_short_str, short_str);
	CHECK_BOUND_BUFFER_USAGE(n_long_str, long_str);
	CHECK_BOUND_BUFFER_USAGE(stmt->nr, length);
	CHECK_BOUND_BUFFER_USAGE(stmt->nr, is_null);
	CHECK_BOUND_BUFFER_USAGE(stmt->nr, error);

	/* Now bind our statement results. */
	return mysql_stmt_bind_result(stmt->handle, stmt->result_binds)
			? NDO_ERROR : NDO_OK;
}


/* We don't need these anymore. */
#undef CHECK_BOUND_BUFFER_USAGE
#undef UPDATE_BOUND_BUFFER_USAGE



/**
 * Prepares and binds a statement.
 * @param idi Input data and DB connection info.
 * @param stmt_id Statement id to prepare.
 * @param template Statement SQL template.
 * @param len Template strlen.
 * @param params Column name and input datatype to bind for each parameter.
 * @param np Number of parameters.
 * @param results Column name and output datatype to bind for each result.
 * @param nr Number of results.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 * @post ndo2db_stmts[stmt_id].handle is the statment handle.
 * @post ndo2db_stmts[stmt_id].param_binds is the array of parameter bindings.
 * @post ndo2db_stmts[stmt_id].results is the array of result bindings.
 */
static int ndo2db_stmt_prepare_and_bind(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id,
	const char *template,
	const size_t len,
	const struct ndo2db_stmt_bind *params,
	const size_t np,
	const struct ndo2db_stmt_bind *results,
	const size_t nr
) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;

	/* Store our parameters/results, counts and id for later reference. */
	stmt->params = params;
	stmt->np = np;
	stmt->results = results;
	stmt->nr = nr;
	stmt->id = stmt_id;

	/* Prepare our statement with the template. */
	ndo2db_log_debug_info(NDO2DB_DEBUGL_STMT, 0,
			"do2db_stmt_prepare_and_bind: %s template: %s\n",
			ndo2db_stmt_names[stmt->id], template);

	/* Close (and free) any existing statement and get a new statement handle. */
	if (stmt->handle) mysql_stmt_close(stmt->handle);
	stmt->handle = mysql_stmt_init(&idi->dbinfo.mysql_conn);
	if (!stmt->handle) return NDO_ERROR;

	/* Prepare our statement from the template. */
	if (mysql_stmt_prepare(stmt->handle, template, (unsigned long)len)) return NDO_ERROR;

	/* Setup parameter and result bindings. */
	if (np) CHK_OK(ndo2db_stmt_bind_params(stmt));
	if (nr) CHK_OK(ndo2db_stmt_bind_results(stmt));

	return NDO_OK;
}


/**
 * Prepares and binds an "INSERT INTO ..." statement.
 * @param idi Input data and DB connection info.
 * @param dbuf Dynamic buffer for printing statment templates.
 * @param stmt_id Statement id to prepare.
 * @param table_id Table name index for the statement.
 * @param params Column name and input datatype to bind for each parameter.
 * @param np Number of parameters.
 * @param up_on_dup Non-zero to add an "ON DUPLICATE KEY UPDATE ..." clause.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 * @post ndo2db_stmts[stmt_id].handle is the statment handle.
 * @post ndo2db_stmts[stmt_id].param_binds is the array of parameter bindings.
 */
static int ndo2db_stmt_prepare_insert(
	ndo2db_idi *idi,
	ndo_dbuf *dbuf,
	const enum ndo2db_stmt_id stmt_id,
	const int table_id,
	const struct ndo2db_stmt_bind *params,
	const size_t np,
	const my_bool up_on_dup
) {
	/* Print our template with an "ON DUPLICATE KEY UPDATE..." if requested. */
	ndo_dbuf_reset(dbuf);
	CHK_OK(ndo2db_stmt_print_insert(idi, dbuf,
			ndo2db_db_tablenames[table_id], params, np, up_on_dup));

	/* Prepare our statement and bind its parameters. */
	return ndo2db_stmt_prepare_and_bind(idi, stmt_id,
			dbuf->buf, dbuf->used_size, params, np, NULL, 0);
}


/**
 * Prepares and binds a SELECT statement for fetching instance data.
 * @param idi Input data and DB connection info.
 * @param dbuf Dynamic buffer for printing the statement template.
 * @param stmt_id Statement id to prepare.
 * @param from "... FROM %s ..." table name or expression string.
 * @param params Column name and input datatype to bind for each parameter.
 * @param np Number of parameters.
 * @param results Column name and output datatype to bind for each result.
 * @param nr Number of results.
 * @param and_where Additional WHERE ... AND condition
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 * @post ndo2db_stmts[stmt_id].handle is the statment handle.
 * @post ndo2db_stmts[stmt_id].binds is the array of parameter bindings.
 * @post ndo2db_stmts[stmt_id].results is the array of result bindings.
 */
static int ndo2db_stmt_prepare_select(
		ndo2db_idi *idi,
		ndo_dbuf *dbuf,
		const enum ndo2db_stmt_id stmt_id,
		const char *from,
		const struct ndo2db_stmt_bind *params,
		const size_t np,
		const struct ndo2db_stmt_bind *results,
		const size_t nr,
		const char *and_where
) {
	size_t i;

	/* Print our full template. */
	ndo_dbuf_reset(dbuf);
	CHK_OK(ndo_dbuf_strcat(dbuf, "SELECT "));

	for (i = 0; i < nr; ++i) {
		CHK_OK(ndo_dbuf_printf(dbuf, (i ? ",%s" : "%s"), results[i].column));
	}

	CHK_OK(ndo_dbuf_printf(dbuf, " FROM %s WHERE instance_id=%lu",
			from, idi->dbinfo.instance_id));

	if (and_where && *and_where) {
		CHK_OK(ndo_dbuf_printf(dbuf, " AND %s", and_where));
	}

	/* Prepare our statement, and bind its parameters and results. */
	return ndo2db_stmt_prepare_and_bind(idi, stmt_id,
			dbuf->buf, dbuf->used_size, params, np, results, nr);
}



/**
 * Converts and copies buffered input data to bound parameter storage. Only
 * parameters with a valid buffered input index will be converted, others must
 * be processed manually.
 * @param idi Input data and DB connection info.
 * @param stmt Statement to convert data for.
 * @return NDO_OK on success, or NDO_ERROR on error.
 * @note Data conversion and truncation errors are silently ignored, as this
 * was the behavior of the string-based handlers.
 */
static int ndo2db_stmt_process_buffered_input(
	ndo2db_idi *idi,
	struct ndo2db_stmt *stmt
) {
	size_t i;
	const struct ndo2db_stmt_bind *p = stmt->params;
	MYSQL_BIND *b = stmt->param_binds;

	/* Nothing to do if no parameters or binds. */
	if (!p || !b) return NDO_OK;

	for (i = 0; i < stmt->np; ++i, ++p, ++b) {
		const char *input = (p->bi_index >= 0) ? idi->buffered_input[p->bi_index] : NULL;

		/* Skip params with no buffered_input flag. */
		if (~p->flags & BIND_BUFFERED_INPUT) continue;

		switch (p->type) {

		case BIND_TYPE_BOOL:
			TO_BOUND_BOOL((input && *input && *input != '0'), *b);
			break;

		case BIND_TYPE_INT8:
			ndo_checked_strtoint8(input, b->buffer);
			break;

		case BIND_TYPE_INT16:
			ndo_checked_strtoint16(input, b->buffer);
			break;

		case BIND_TYPE_INT32:
			ndo_checked_strtoint32(input, b->buffer);
			break;

		case BIND_TYPE_FROM_UNIXTIME: /* Timestamps are bound as uint32. */
		case BIND_TYPE_ID: /* This needs to change if ndo2db_id_t does. */
		case BIND_TYPE_UINT32:
			ndo_checked_strtouint32(input, b->buffer);
			break;

		case BIND_TYPE_DOUBLE:
			ndo_checked_strtod(input, b->buffer);
			break;

		case BIND_TYPE_SHORT_STRING:
		case BIND_TYPE_LONG_STRING:
			COPY_BIND_STRING_OR_EMPTY(input, *b);
			break;

		case BIND_TYPE_TV_SEC:
			{
				struct timeval tv;
				ndo_checked_strtotv(input, &tv);
				COPY_TV_TO_BOUND_TV(tv, b[0], b[1]);
			}
			break;

		case BIND_TYPE_TV_USEC:
			/* We checked sec/usec ordering in ndo2db_stmt_bind_params(), so this
			 * should have already been processed by an immediately preceding
			 * BIND_TYPE_TV_SEC. A more detailed check may be desired here. */
			break;

		case BIND_TYPE_CURRENT_CONFIG:
			TO_BOUND_INT8(idi->current_object_config_type, *b);
			break;

		default:
			syslog(LOG_USER|LOG_ERR,
					"ndo2db_stmt_process_buffered_input: %s params[%zu] has bad type %d.",
					ndo2db_stmt_names[stmt->id], i, p->type);
			return NDO_ERROR;
		}
	}

	return NDO_OK;
}



/**
 * Executes a prepared statement.
 * @param idi Input data and DB connection info.
 * @param stmt Prepared statement to execute.
 * @return NDO_OK on success, NDO_ERROR on failure.
 */
static int ndo2db_stmt_execute(ndo2db_idi *idi, struct ndo2db_stmt *stmt) {
	/* Try to connect if we're not connected... */
	if (!idi->dbinfo.connected) {
		CHK_OK(ndo2db_db_connect(idi));
		if (!idi->dbinfo.connected) return NDO_ERROR;
		/* This reprepares and rebinds our statements, but doesn't touch the bound
		 * buffer contents, so parameter data will be preserved. (Unless
		 * mysql_stmt_bind_param() touches the buffers, which it shouldn't...) */
		CHK_OK(ndo2db_db_hello(idi));
	}

	if (mysql_stmt_execute(stmt->handle)) {
		syslog(LOG_USER|LOG_ERR,
				"mysql_stmt_execute() failed for statement %d, mysql_stmt_error: %s",
				stmt->id, mysql_stmt_error(stmt->handle));
		ndo2db_handle_db_error(idi);
		return NDO_ERROR;
	}

	return NDO_OK;
}




/*
 * From Nagios 4 lib/dkhash.c: "Polynomial conversion ignoring overflows.
 * Pretty standard hash, once based on Ozan Yigit's sdbm() hash but later
 * modified for Nagios to produce better results on our typical data."
 * See also: http://www.cse.yorku.ca/~oz/hash.html (the original hash here was
 * K&R 1st edition).
 * @todo Try the 'canonical' seeds and primes; but need varied representative
 * data to measure the difference.
 */
#define NDO2DB_OBJECT_HASHPRIME 509U
#define NDO2DB_OBJECT_HASHSEED 0x123U /* "magic" (there is probably a better seed...) */

#define ACUMULATE_HASH(p, h) \
	while (*p) h = (ndo2db_hash_t)*p++ + h * NDO2DB_OBJECT_HASHPRIME

/**
 * Calculates an object's hash value.
 * @param n1 Object first name (non-null).
 * @param n2 Object second name (non-null).
 * @return A hash value based on the concatenation of n1 and n2.
 */
inline static ndo2db_hash_t ndo2db_obj_hash(const char *n1, const char *n2) {
	ndo2db_hash_t h = NDO2DB_OBJECT_HASHSEED;

	ACUMULATE_HASH(n1, h);
	ACUMULATE_HASH(n2, h);

	return h;
}


/**
 * Compares two objects ordered by: h, type, name1, name2.
 * @param a Object a.
 * @param b_h Object b full hash value.
 * @param b_type Object b type.
 * @param b_name1 Object b name1 (non-null).
 * @param b_name2 Object b name2 (non-null).
 * @return 0 if a == b; >0 if a > b; <0 if a < b.
 */
inline static int ndo2db_obj_compare(const struct ndo2db_object *a,
		ndo2db_hash_t b_h, int b_type, const char *b_name1,
		const char *b_name2) {
	int result;

	return
			/* First compare by full hash value. */
			(a->h > b_h) ? +1
			: (a->h < b_h) ? -1
			/* Next compare by object type if hashes are equal. */
			: ((result = a->type - b_type) != 0) ? result
			/* Next compare by first name if types are equal. */
			: ((result = strcmp(a->name1, b_name1)) != 0) ? result
			/* Compare by second name if all else is equal. Sidestep strcmp() for the
			 * common "empty second names" case (everything except services). */
			: (!*a->name2 && !*b_name2) ? 0
			: strcmp(a->name2, b_name2)
	;
}


/**
 * Fetches an existing object id from the cache.
 * @note This is only called by ndo2db_find_obj() which is only called by
 * ndo2db_get_obj_id() which normalizes null names to empty.
 * @param type ndo2db object type code.
 * @param name1 Object name1 (non-null).
 * @param name2 Object name2 (non-null).
 * @param id Object id output.
 * @return NDO_OK with the object id in *id if an object was found, otherwise
 * NDO_ERROR and *id = 0.
 */
static int ndo2db_lookup_obj(int type, const char *name1, const char *name2,
		ndo2db_id_t *id) {
	const struct ndo2db_object *curr;
	ndo2db_hash_t h; /* Raw hash value type,name1,name2 to lookup. */
	size_t i; /* Hash table index, h % ndo2db_objects_size. */
	size_t x; /* Zero-based bucket iteration counter. */

	if (!ndo2db_objects) {
#ifdef NDO2DB_DEBUG_CACHING
		ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
				"ndo2db_lookup_obj: no object cache allocated\n");
#endif
		*id = 0;
		return NDO_ERROR;
	}

	h = ndo2db_obj_hash(name1, name2);
	i = (size_t)h % ndo2db_objects_size;
#ifdef NDO2DB_DEBUG_CACHING
	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
			"ndo2db_lookup_obj: type=%d, name1=%s, name2=%s, h=%zu, i=%zu\n",
			type, name1, name2, (size_t)h, i);
#endif

	for (curr = ndo2db_objects[i], x = 0; curr; curr = curr->next, ++x) {
		int c = ndo2db_obj_compare(curr, h, type, name1, name2);
#ifdef NDO2DB_DEBUG_CACHING
		ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 1,
				"ndo2db_lookup_obj: loop [%zu]: id=%lu, h=%zu, type=%d, name1=%s, name2=%s, c=%d\n",
				x, (unsigned long)curr->id, (size_t)curr->h, curr->type, curr->name1, curr->name2, c);
#endif
		if (c == 0) {
			/* We have a match! */
#ifdef NDO2DB_DEBUG_CACHING
			ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0, "ndo2db_lookup_obj: hit\n");
#endif
			*id = curr->id;
			return NDO_OK;
		}
		else if (c > 0) {
			/* The bucket list is ordered when adding so we know this is a miss. */
			break;
		}
	}

	/* No match :(. */
#ifdef NDO2DB_DEBUG_CACHING
	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0, "ndo2db_lookup_obj: miss\n");
#endif
	*id = 0;
	return NDO_ERROR;
}


/**
 * Constructs the object cache hash table, and clears object and collision
 * counters. Any existing cache hash table or cached objects will be freed.
 * @param size Hash table size to create.
 * @return NDO_OK on success, NDO_ERROR if the table could not be allocated.
 */
static int ndo2db_init_obj_cache(size_t size) {

	if (ndo2db_objects) ndo2db_free_obj_cache();

	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
			"ndo2db_init_obj_cache: initializing object cache of size=%zu\n",
			size);

	ndo2db_objects = calloc(size, sizeof(struct ndo2db_object *));

	if (!ndo2db_objects) {
		syslog(LOG_USER|LOG_ERR,
				"ndo2db_init_obj_cache: failed to allocate object cache hash table\n");
		ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
				"ndo2db_init_obj_cache: failed to allocate object cache hash table\n");
		return NDO_ERROR;
	}

	ndo2db_objects_size = size;
	ndo2db_objects_count = 0;
	ndo2db_objects_activated = 0;
	ndo2db_objects_collisions = 0;

	return NDO_OK;
}


/**
 * Adds an object to the cache.
 * @note This is only called by ndo2db_load_obj_cache() and ndo2db_get_obj_id()
 * which normalize null names to empty.
 * @param type ndo2db object type code.
 * @param name1 Object name1 (non-null).
 * @param name2 Object name2 (non-null).
 * @param id Object id.
 * @param is_active True if this object is known active.
 * @return NDO_OK if the object was added successfully, otherwise NDO_ERROR.
 */
static int ndo2db_cache_obj(int type, const char *name1, const char *name2,
		ndo2db_id_t id, my_bool is_active) {
	struct ndo2db_object *curr;
	struct ndo2db_object *prev;
	struct ndo2db_object *new;
	ndo2db_hash_t h; /* Raw hash of name1,name2 to cache. */
	size_t i; /* Hash table index, h % ndo2db_objects_size. */
	size_t x; /* Zero-based bucket iteration counter. */

	/* Initialize the cache if needed. */
	if (!ndo2db_objects) CHK_OK(ndo2db_init_obj_cache(NDO2DB_OBJECT_HASHSLOTS));

	h = ndo2db_obj_hash(name1, name2);
	i = (size_t)h % ndo2db_objects_size;
#ifdef NDO2DB_DEBUG_CACHING
	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
			"ndo2db_cache_obj: id=%lu, type=%d, name1=%s, name2=%s, h=%zu, i=%zu\n",
			(unsigned long)id, type, name1, name2, (size_t)h, i);
#endif

	/* Construct our new object. */
	new = malloc(sizeof(struct ndo2db_object));
	if (!new) return NDO_ERROR;
	new->h = h;
	new->name1 = strdup(name1);
	new->name2 = strdup(name2);
	new->id = id;
	new->type = type;
	new->is_active = is_active;
	/* Maintain our invariants. */
	if (!new->name1 || !new->name2) {
		free(new->name1), free(new->name2), free(new);
		return NDO_ERROR;
	}

	for (prev = NULL, curr = ndo2db_objects[i], x = 0;
			curr;
			prev = curr, curr = curr->next, ++x) {
		int c = ndo2db_obj_compare(curr, h, type, name1, name2);
#ifdef NDO2DB_DEBUG_CACHING
		ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 1,
				"ndo2db_cache_obj: loop [%zu]: id=%lu, h=%zu, type=%d, name1=%s, name2=%s, c=%d\n",
				x, (unsigned long)curr->id, (size_t)curr->h, curr->type, curr->name1, curr->name2, c);
#endif
		if (c == 0) {
			/* There shouldn't be duplicates, and adding duplicates would hide
			 * objects since lookup would pick the first match, so don't do it. */
#ifdef NDO2DB_DEBUG_CACHING
			ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0, "ndo2db_cache_obj: duplicate\n");
#endif
			free(new->name1), free(new->name2), free(new);
			return NDO_ERROR;
		}
		else if (c > 0) {
			/* curr is greater than new, insert before curr. This orders the list in
			 * ascending numerical order, putting hosts and services first. */
			break;
		}
	}

	++ndo2db_objects_count;
	if (ndo2db_objects[i]) ++ndo2db_objects_collisions;

	if (prev) prev->next = new;
	else ndo2db_objects[i] = new;
	new->next = curr;

	return NDO_OK;
}


/**
 * Fetches an existing object id from the cache or DB.
 * @note This is only called by ndo2db_get_obj_id() which normalizes null names
 * to empty.
 * @param idi Input data and DB connection info.
 * @param type ndo2db object type code.
 * @param name1 Object name1 (non-null).
 * @param name2 Object name2 (non-null).
 * @param id Object id output.
 * @return NDO_OK with the object id in *id, otherwise an error code (usually
 * NDO_ERROR) and *id = 0.
 */
static int ndo2db_find_obj(ndo2db_idi *idi, int type,	const char *name1,
		const char *name2, ndo2db_id_t *id) {
	/* Select the statement and binds to use. */
	const enum ndo2db_stmt_id stmt_id = (*name2) /* name2 not empty : empty... */
			? NDO2DB_STMT_GET_OBJ_ID : NDO2DB_STMT_GET_OBJ_ID_N2_NULL;
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;
	MYSQL_BIND *result = stmt->result_binds + 0;

	/* See if the object is already cached. */
	if (ndo2db_lookup_obj(type, name1, name2, id) == NDO_OK) return NDO_OK;

	/* Nothing cached so query. Copy input data to the parameter buffers. */
	TO_BOUND_INT8(type, binds[0]);
	COPY_BIND_STRING_NOT_EMPTY(name1, binds[1]);
	/* For the DB, empty name2 is NULL to keep in line with existing data. The
	 * "name2 IS NULL" statement doesn't have a name2 parameter. */
	if (*name2) COPY_BIND_STRING_NOT_EMPTY(name2, binds[2]);

	/* Execute the statement... */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	/* ...and fetch the first (only) result row to bound storage. */
	if (mysql_stmt_fetch(stmt->handle) || *result->error || *result->is_null) {
		return NDO_ERROR;
	}

	/* We have a good id by all our meaasures if we made it here. */
	*id = FROM_BOUND_ID(*result);

	/* Cache the object for later (@todo pass activate param instead of false). */
	return ndo2db_cache_obj(type, name1, name2, *id, NDO_FALSE);
}


/**
 * Fetches an object id from the cache or DB if one exists, inserts a new row
 * if an existing id is not found for non-empty object names.
 * @param idi Input data and DB connection info.
 * @param type ndo2db object type code.
 * @param name1 Object name1.
 * @param name2 Object name2.
 * @param id Object id output.
 * @return NDO_OK with the object id in *id, otherwise an error code (usually
 * NDO_ERROR) and *id = 0.
 */
static int ndo2db_get_obj_id(ndo2db_idi *idi, int type, const char *name1,
		const char *name2, ndo2db_id_t *id) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_GET_OBJ_ID_INSERT;
	MYSQL_BIND *binds = stmt->param_binds;
	*id = 0;

	/* There is no valid objecct with an empty first name, no name means no id. */
	if (!name1 || !*name1) return NDO_OK;
	/* See if the object already exists. */
	/* name2 can be NULL in the DB. We previously converted empty to NULL before
	 * inseting to the DB, and for object caching. We'll keep consistent with
	 * earlier handling for the DB, but convert NULL to empty for object
	 * caching. This makes hashing and comparing local objects simpler. */
	if (ndo2db_find_obj(idi, type, name1, name2 ? name2 : "", id) == NDO_OK) {
		return NDO_OK;
	}

	/* No such object so insert. Copy input data to the parameter buffers. */
	TO_BOUND_INT8(type, binds[0]); /* objecttype_id */
	COPY_BIND_STRING_NOT_EMPTY(name1, binds[1]);
	/* For the DB we make empty name2 NULL to keep in line with existing data. */
	if (name2 && !*name2) name2 = NULL;
	COPY_BIND_STRING_OR_NULL(name2, binds[2]);
	/* Execute the statement and grab the inserted object id if successful. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	*id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);

	/* Cache the object for later (@todo pass activate param instead of false).
	* Don't forget our empty name convention for the cache! */
	return ndo2db_cache_obj(type, name1, name2 ? name2 : "", *id, NDO_FALSE);
}

/**
 * Expands to a ndo2db_get_obj_id(idi, NDO2DB_OBJECTTYPE_ ## suffix, ...) call.
 * idi must be available in context.
 * @param suffix NDO2DB_OBJECTTYPE_ object type code suffix.
 * @param name1 Object name1.
 * @param name2 Object name2.
 * @param id Object id output.
 */
#define NDO2DB_GET_OBJ_ID(suffix, name1, name2, id) \
	ndo2db_get_obj_id(idi, NDO2DB_OBJECTTYPE_ ## suffix, name1, name2, id)


/**
 * Fetches all previously active objects for an instance from the DB on
 * connection startup.
 * @param idi Input data and DB connection info.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 * @post It is possible for the object cache to be partially populated if an
 * error occurs while processing results.
 * @note Though executed once when starting up a connection, preparation makes
 * processing the (potentially large) result set a bit more efficient, and the
 * code (perhaps) a bit cleaner. Any performance gain will depend on result set
 * size.
 */
int ndo2db_load_obj_cache(ndo2db_idi *idi) {
	int status;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_GET_OBJ_IDS;
	MYSQL_BIND *results = stmt->result_binds;
	my_ulonglong num_objects;
	size_t num_slots;

	/* Find all previously active objects. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));

	/* Buffer the complete result set from the server. */
	if (mysql_stmt_store_result(stmt->handle)) {
		const char *err = mysql_stmt_error(stmt->handle);
		syslog(LOG_USER|LOG_ERR,
				"ndo2db_load_obj_cache: mysql_stmt_store_result() failed: %s", err);
		ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
				"ndo2db_load_obj_cache: mysql_stmt_store_result() failed: %s", err);
		return NDO_ERROR;
	}

	/* Calculate how many hash slots we want. Twice the number of objects may not
	 * be optimal for hash distribution or memory usage reasons. */
	num_objects = mysql_stmt_num_rows(stmt->handle);
	num_slots = (size_t)num_objects * 2;
	if (num_slots < NDO2DB_OBJECT_HASHSLOTS) num_slots = NDO2DB_OBJECT_HASHSLOTS;
#ifdef NDO2DB_DEBUG_CACHING
	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
			"ndo2db_load_obj_cache: rows=%llu, slots=%zu\n", num_objects, num_slots);
#endif

	/* Free and reinitialize the cache, we're rebuilding from scratch. */
	CHK_OK(ndo2db_init_obj_cache(num_slots));

	/* Process each row of the result set until an error or end of data. */
	while ((status = mysql_stmt_fetch(stmt->handle)) == 0) {

		ndo2db_id_t id = FROM_BOUND_ID(results[0]);
		int type = (int)FROM_BOUND_INT8(results[1]);
		/* name1 shouldn't be NULL, but check for thoroughness. */
		const char *name1 = (*results[2].is_null) ? NULL : results[2].buffer;
		/* name2 can be NULL in the DB, and we previously converted empty to NULL
		 * before inserting, but we convert null to empty for object caching. */
		const char *name2 = (*results[3].is_null) ? "" : results[3].buffer;

		/* There is no valid object with an empty first name, there shuoldn't be
		 * one in the DB, and there wont be one in the cache. */
		if (!name1 || !*name1) {
#ifdef NDO2DB_DEBUG_CACHING
			ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
					"ndo2db_load_obj_cache: name1 empty\n");
#endif
			continue;
		}

		/* Now we're good to cache the object (@todo pass activate param). */
		ndo2db_cache_obj(type, name1, name2, id, NDO_FALSE);
	}

	/* Success if we're here because there was no more data. */
	return (status == MYSQL_NO_DATA) ? NDO_OK : NDO_ERROR;
}


/**
 * Frees resources allocated for the object cache.
 */
void ndo2db_free_obj_cache(void) {

	if (ndo2db_objects) {
		size_t x = 0;
		for (; x < NDO2DB_OBJECT_HASHSLOTS; ++x) {
			struct ndo2db_object *curr;
			struct ndo2db_object *next;
			for (curr = ndo2db_objects[x]; curr; curr = next) {
				next = curr->next;
				free(curr->name1), free(curr->name2), free(curr);
			}
		}

		free(ndo2db_objects);
		ndo2db_objects = NULL;
	}

	ndo2db_objects_size = 0;
	ndo2db_objects_count = 0;
	ndo2db_objects_activated = 0;
	ndo2db_objects_collisions = 0;
}



/**
 * Marks all objects inactive in the DB.
 * @param idi Input data and DB connection info.
 * @return NDO_OK on success, an error code otherwise, usually NDO_ERROR.
 */
static int ndo2db_set_all_objs_inactive(ndo2db_idi *idi) {
	int status;
	char *buf = NULL;

	ndo2db_objects_activated = 0;

	/* Since this is executed once when starting up a connection, preparing this
	 * would be much more expensive than one allocaing print here. */
	if (asprintf(&buf, "UPDATE %s SET is_active=0 WHERE instance_id=%lu",
			ndo2db_db_tablenames[NDO2DB_DBTABLE_OBJECTS], idi->dbinfo.instance_id
	) < 0) return NDO_ERROR;

	status = ndo2db_db_query(idi, buf);
	free(buf);
	return status;
}


/**
 * Sets an object active in the DB for the current instance.
 * @param idi Input data and DB connection info.
 * @param type ndo2db object type code.
 * @param id Object id.
 */
static int ndo2db_set_obj_active(ndo2db_idi *idi, int type, ndo2db_id_t id) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_SET_OBJ_ACTIVE;

	++ndo2db_objects_activated;

	TO_BOUND_ID(id, stmt->param_binds[0]);
	TO_BOUND_INT8(type, stmt->param_binds[1]);
	return ndo2db_stmt_execute(idi, stmt);
}




static int ndo2db_stmt_save_logentry(ndo2db_idi *idi, my_bool is_live) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_SAVE_LOG;
	/* Set what our caller hasn't, and execute. */
	TO_BOUND_BOOL(is_live, s->param_binds[5]); /* realtime_data */
	TO_BOUND_BOOL(is_live, s->param_binds[6]); /* inferred_data_extracted */
	return ndo2db_stmt_execute(idi, s);
}

#define STRIP_TRAILING_NEWLINES(s) \
	do { \
		size_t i_ = strlen(s); \
		while (i_ && s[i_ - 1] == '\n') --i_; \
		s[i_] = '\0'; \
	} while (0)

int ndo2db_stmt_handle_logentry(ndo2db_idi *idi) {
	uint32_t log_time;
	struct ndo2db_stmt *s;
	MYSQL_BIND *b;
	MYSQL_BIND *result;

	/* Break the log line into logentry_time and logentry_data strings. */
	char *log_msg = NULL;
	const char *log_ts = strtok_r(
			idi->buffered_input[NDO_DATA_LOGENTRY], "]", &log_msg);
	if (!log_ts || *log_ts != '[' || !log_msg) return NDO_ERROR;

	/* The logentry_time string must convert successfully. */
	CHK_OK(ndo_checked_strtouint32(log_ts + 1, &log_time));
	/* Remove any trailing newlines from the log message. */
	STRIP_TRAILING_NEWLINES(log_msg);

	/* See if any entries exist with the same logentry_time and logentry_data. */
	s = ndo2db_stmts + NDO2DB_STMT_FIND_LOG;
	b = s->param_binds;
	result = s->result_binds;
	TO_BOUND_UINT32(log_time, b[0]); /* logentry_time */
	COPY_BIND_STRING_OR_EMPTY(log_msg, b[1]); /* logentry_data */
	ndo2db_stmt_execute(idi, s);
	/* We have a duplicate if the first (only) result row is non-zero. We really
	 * need to sort out error handling. Follow the original string handler here:
	 * it's only a duplicate if we successully counted one. */
	if (mysql_stmt_fetch(s->handle) == 0
			&& !*result->error && !*result->is_null
			&& FROM_BOUND_INT32(*result) != 0
	) {
#ifdef NDO2DB_DEBUG
		ndo2db_log_debug_info(NDO2DB_DEBUGL_SQL|NDO2DB_DEBUGL_STMT, 0,
				"ndo2db_stmt_handle_logentry: Ignoring duplicate.\n");
#endif
		return NDO_OK;
	}

	/* No duplicate, so copy our data to bound storage and save the log. */
	s = ndo2db_stmts + NDO2DB_STMT_SAVE_LOG;
	b = s->param_binds;
	/* logentry_time and logentry_data are already set in b[0] and b[1], the find
	 * and save statements bind to the same buffers for these data. */
	TO_BOUND_INT8(0, b[2]); /* logentry_type 0 here */
	TO_BOUND_UINT32(log_time, b[3]); /* entry_time is logentry_time here */
	TO_BOUND_INT32(0, b[4]); /* entry_time_usec 0 here */
	/* Callee sets b[5] (realtime_data) and b[6] (inferred_data_extracted). */

	return ndo2db_stmt_save_logentry(idi, 0);
	/* @todo: "Possibly extract 'inferred data' or logentry_type..." */
}

int ndo2db_stmt_handle_logdata(ndo2db_idi *idi) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_SAVE_LOG;
	MYSQL_BIND *b = s->param_binds;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Strip any trailing newlines from log data in buffered input. */
	STRIP_TRAILING_NEWLINES(idi->buffered_input[NDO_DATA_LOGENTRY]);

	ndo2db_stmt_process_buffered_input(idi, s); /* Process b[0,1,2]. */
	COPY_TV_TO_BOUND_TV(tstamp, b[3], b[4]); /* entry_time, entry_time_usec */
	/* Callee sets b[5] (realtime_data) and b[6] (inferred_data_extracted). */

	return ndo2db_stmt_save_logentry(idi, 1);
}

#undef STRIP_TRAILING_NEWLINES


int ndo2db_stmt_handle_processdata(ndo2db_idi *idi) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_PROCESSDATA;
	MYSQL_BIND *b = s->param_binds;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Copy/convert and save the process data. */
	TO_BOUND_INT32(type, b[0]); /* event_type */
	COPY_TV_TO_BOUND_TV(tstamp, b[1], b[2]); /* event_time, event_time_usec */
	ndo2db_stmt_process_buffered_input(idi, s);
	SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

	if (tstamp.tv_sec < idi->dbinfo.latest_realtime_data_time) return status;

	/* Clear live data if the process is just starting up. */
	if (type == NEBTYPE_PROCESS_PRELAUNCH) {
		#define CLEAR_TABLE(t) \
				ndo2db_db_clear_table(idi, ndo2db_db_tablenames[NDO2DB_DBTABLE_ ## t])
		/* Live data. */
		CLEAR_TABLE(PROGRAMSTATUS);
		CLEAR_TABLE(HOSTSTATUS);
		CLEAR_TABLE(SERVICESTATUS);
		CLEAR_TABLE(CONTACTSTATUS);
		CLEAR_TABLE(TIMEDEVENTQUEUE);
		CLEAR_TABLE(COMMENTS);
		CLEAR_TABLE(SCHEDULEDDOWNTIME);
		CLEAR_TABLE(RUNTIMEVARIABLES);
		CLEAR_TABLE(CUSTOMVARIABLESTATUS);
		/* Config data. */
		CLEAR_TABLE(CONFIGFILES);
		CLEAR_TABLE(CONFIGFILEVARIABLES);
		CLEAR_TABLE(CUSTOMVARIABLES);
		CLEAR_TABLE(COMMANDS);
		CLEAR_TABLE(TIMEPERIODS);
		CLEAR_TABLE(TIMEPERIODTIMERANGES);
		CLEAR_TABLE(CONTACTGROUPS);
		CLEAR_TABLE(CONTACTGROUPMEMBERS);
		CLEAR_TABLE(HOSTGROUPS);
		CLEAR_TABLE(HOSTGROUPMEMBERS);
		CLEAR_TABLE(SERVICEGROUPS);
		CLEAR_TABLE(SERVICEGROUPMEMBERS);
		CLEAR_TABLE(HOSTESCALATIONS);
		CLEAR_TABLE(HOSTESCALATIONCONTACTS);
		CLEAR_TABLE(SERVICEESCALATIONS);
		CLEAR_TABLE(SERVICEESCALATIONCONTACTS);
		CLEAR_TABLE(HOSTDEPENDENCIES);
		CLEAR_TABLE(SERVICEDEPENDENCIES);
		CLEAR_TABLE(CONTACTS);
		CLEAR_TABLE(CONTACTADDRESSES);
		CLEAR_TABLE(CONTACTNOTIFICATIONCOMMANDS);
		CLEAR_TABLE(HOSTS);
		CLEAR_TABLE(HOSTPARENTHOSTS);
		CLEAR_TABLE(HOSTCONTACTS);
		CLEAR_TABLE(SERVICES);
#ifdef BUILD_NAGIOS_4X
		CLEAR_TABLE(SERVICEPARENTSERVICES);
#endif
		CLEAR_TABLE(SERVICECONTACTS);
		CLEAR_TABLE(SERVICECONTACTGROUPS);
		CLEAR_TABLE(HOSTCONTACTGROUPS);
		CLEAR_TABLE(HOSTESCALATIONCONTACTGROUPS);
		CLEAR_TABLE(SERVICEESCALATIONCONTACTGROUPS);
		#undef CLEAR_TABLE

		/* All objects are initially inactive. */
		SAVE_ERR(status, ndo2db_set_all_objs_inactive(idi));
	}

	/* Update process status data if it's shutting down or restarting. */
	else if (type == NEBTYPE_PROCESS_SHUTDOWN || type == NEBTYPE_PROCESS_RESTART) {
		s = ndo2db_stmts + NDO2DB_STMT_UPDATE_PROCESSDATA_PROGRAMSTATUS;
		TO_BOUND_UINT32(tstamp.tv_sec, s->param_binds[0]); /* program_end_time */
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
	}

	return status;
}


int ndo2db_stmt_handle_timedeventdata(ndo2db_idi *idi) {
	int16_t timedevent_type;
	int object_type;
	ndo2db_id_t object_id;
	char **bi = idi->buffered_input;
	struct ndo2db_stmt *s;
	MYSQL_BIND *b;
	size_t i;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA; /* Our standard data type is our NEBTYPE. */

#ifndef NDO2DB_SAVE_TIMEDEVENTS_HISTORY
	/* Nothing to do with old data if not saving history. (There's a flux
	 * capacitor in here somewhere, maybe next to the potential inductor...) */
	if (tstamp.tv_sec < idi->dbinfo.latest_realtime_data_time) return NDO_OK;
#endif
	/* Skip sleep events. */
	if (type == NEBTYPE_TIMEDEVENT_SLEEP) return NDO_OK;

	/* Get the Nagios timed EVENT type (this is different from the NEBTYPE). */
	ndo_checked_strtoint16(bi[NDO_DATA_EVENTTYPE], &timedevent_type);

	/* Get the host/service object id, if applicable. */
	switch (timedevent_type) {
	case EVENT_HOST_CHECK: object_type = NDO2DB_OBJECTTYPE_HOST; break;
	case EVENT_SERVICE_CHECK: object_type = NDO2DB_OBJECTTYPE_SERVICE; break;
	case EVENT_SCHEDULED_DOWNTIME: object_type = (bi[NDO_DATA_SERVICE]
				? NDO2DB_OBJECTTYPE_SERVICE : NDO2DB_OBJECTTYPE_HOST); break;
	default: object_type = 0; break;
	}
	object_id = 0;
	if (object_type) {
		ndo2db_get_obj_id(idi, object_type,
				bi[NDO_DATA_HOST], bi[NDO_DATA_SERVICE], &object_id);
	}


/* Save a history of events that get added, executed or removed, if enabled. */
#ifdef NDO2DB_SAVE_TIMEDEVENTS_HISTORY
	/* Select our statement, if we have one for this NEBTYPE. */
	s = ndo2db_stmts;
	switch (type) {
	case NEBTYPE_TIMEDEVENT_ADD: s += NDO2DB_STMT_TIMEDEVENT_ADD; break;
	case NEBTYPE_TIMEDEVENT_EXECUTE: s += NDO2DB_STMT_TIMEDEVENT_EXECUTE; break;
	case NEBTYPE_TIMEDEVENT_REMOVE: s += NDO2DB_STMT_TIMEDEVENT_REMOVE; break;
	default: goto timedevent_history_done;
	}

	/* We have a statement. Process our input data to bound storage...  */
	b = s->param_binds;
	COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* [queued|event|deletion]_time, usecs */
	TO_BOUND_ID(object_id, b[2]); /* object_id obviously */
	TO_BOUND_INT16(timedevent_type, b[3]); /* event_type */
	ndo2db_stmt_process_buffered_input(idi, s); /* scheduled_time, recurring_event */
	/* ...and make our statement. */
	SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

timedevent_history_done:
	/* There's nothing more to do with old data now that we've saved history. */
	if (tstamp.tv_sec < idi->dbinfo.latest_realtime_data_time) return status;
#endif


	/* Remove likely expired enqueued events on connection startup. */
	if (idi->dbinfo.clean_event_queue) {
		idi->dbinfo.clean_event_queue = NDO_FALSE;
		s = ndo2db_stmts + NDO2DB_STMT_TIMEDEVENTQUEUE_CLEAN;
		/* "DELETE ... WHERE ... scheduled_time < tstamp.tv_sec + 1" equivalent to
		 * "scheduled_time <= tstamp.tv_sec" */
		TO_BOUND_UINT32(tstamp.tv_sec + 1, s->param_binds[0]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
	}


	/* Handle new live data. */
	switch (type) {
	case NEBTYPE_TIMEDEVENT_ADD:
		/* Add newly enqueued timed events. */
		s = ndo2db_stmts + NDO2DB_STMT_TIMEDEVENT_ADD;
		b = s->param_binds;
		COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* queued_time, usecs */
		i = 2;
		break;

	case NEBTYPE_TIMEDEVENT_EXECUTE:
	case NEBTYPE_TIMEDEVENT_REMOVE:
		/* Remove executed or removed enqueued events. */
		s = ndo2db_stmts + NDO2DB_STMT_TIMEDEVENT_REMOVE;
		b = s->param_binds;
		i = 0;
		break;

	default:
		return status; /* Nothing more to do. */
	}

	/* Process any further input to bound storage and execute. */
	TO_BOUND_ID(object_id, b[i]); /* object_id obviously */
	TO_BOUND_INT16(timedevent_type, b[i + 1]); /* event_type */
	ndo2db_stmt_process_buffered_input(idi, s); /* scheduled_time, recurring_event */
	SAVE_ERR(status, ndo2db_stmt_execute(idi, s));


	/* One last thing: remove old enqueued events when checks are executed.
	 * From the original notes: "If we are executing a low-priority event, remove
	 * older events from the queue, as we know they've already been executed.
	 * This is a hack! It shouldn't be necessary, but for some reason it is...
	 * Otherwise not all events are removed from the queue. :-("
	 * @todo: There aren't really low-priority events in Nagios 4, so we may be
	 * able to skip this extra DB operation per check when BUILD_NAGIOS_4X. */
	if (type == NEBTYPE_TIMEDEVENT_EXECUTE
			&& (timedevent_type == EVENT_HOST_CHECK || timedevent_type == EVENT_SERVICE_CHECK)
	) {
		s = ndo2db_stmts + NDO2DB_STMT_TIMEDEVENTQUEUE_CLEAN;
		/* "DELETE ... WHERE ... scheduled_time < tstamp.tv_sec" */
		TO_BOUND_UINT32(tstamp.tv_sec, s->param_binds[0]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
	}

	return status;
}


int ndo2db_stmt_handle_systemcommanddata(ndo2db_idi *idi) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_SYSTEMCOMMAND;
	DECLARE_AND_CONVERT_STD_DATA;
	ndo2db_stmt_process_buffered_input(idi, s);
	return ndo2db_stmt_execute(idi, s);
}


int ndo2db_stmt_handle_eventhandlerdata(ndo2db_idi *idi) {
	int8_t eventhandler_type;
	ndo2db_id_t object_id;
	ndo2db_id_t command_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_EVENTHANDLER;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our eventdler type. */
	ndo_checked_strtoint8(bi[NDO_DATA_EVENTHANDLERTYPE], &eventhandler_type);

	/* Get our object id. */
	if (eventhandler_type == SERVICE_EVENTHANDLER
			|| eventhandler_type == GLOBAL_SERVICE_EVENTHANDLER) {
		NDO2DB_GET_OBJ_ID(SERVICE, bi[NDO_DATA_HOST], bi[NDO_DATA_SERVICE], &object_id);
	}
	else {
		NDO2DB_GET_OBJ_ID(HOST, bi[NDO_DATA_HOST], NULL, &object_id);
	}

	/* Get the command id. */
	NDO2DB_GET_OBJ_ID(COMMAND, bi[NDO_DATA_COMMANDNAME], NULL, &command_id);

	/* Copy everything to bound storage... */
	TO_BOUND_INT8(eventhandler_type, b[0]);
	TO_BOUND_ID(object_id, b[1]);
	TO_BOUND_ID(command_id, b[2]);
	ndo2db_stmt_process_buffered_input(idi, s);
	/* ...and execute. */
	return ndo2db_stmt_execute(idi, s);
}


/** Lookup an optional host/service object id from names in buffered input. */
#define GET_OPTIONAL_HS_ID(type, type_host, type_service, id) \
	do { \
		if (type == type_service) { \
			NDO2DB_GET_OBJ_ID(SERVICE, \
					idi->buffered_input[NDO_DATA_HOST], \
					idi->buffered_input[NDO_DATA_SERVICE], id); \
		} \
		else if (type == type_host) { \
			NDO2DB_GET_OBJ_ID(HOST, idi->buffered_input[NDO_DATA_HOST], NULL, id); \
		} \
		else { \
			*id = 0; \
		} \
	} while (0)


int ndo2db_stmt_handle_notificationdata(ndo2db_idi *idi) {
	int8_t notification_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_NOTIFICATION;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our notification type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_NOTIFICATIONTYPE], &notification_type);
	GET_OPTIONAL_HS_ID(notification_type, HOST_NOTIFICATION, SERVICE_NOTIFICATION,
			&object_id);

	/* Copy everything to bound storage... */
	TO_BOUND_INT8(notification_type, b[0]);
	TO_BOUND_ID(object_id, b[1]);
	ndo2db_stmt_process_buffered_input(idi, s);
	/* ...and execute. */
	SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

	/* Save the notification id for handling contact notifications. */
	if (type == NEBTYPE_NOTIFICATION_START) {
		idi->dbinfo.last_notification_id = (status == NDO_OK)
				? (ndo2db_id_t)mysql_stmt_insert_id(s->handle) : 0;
	}

	return status;
}


int ndo2db_stmt_handle_contactnotificationdata(ndo2db_idi *idi) {
	ndo2db_id_t contact_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONTACTNOTIFICATION;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Get the contact id. */
	NDO2DB_GET_OBJ_ID(CONTACT, bi[NDO_DATA_CONTACTNAME], NULL, &contact_id);

	/* Copy everything to bound storage... */
	TO_BOUND_ID(idi->dbinfo.last_notification_id, b[0]);
	TO_BOUND_ID(contact_id, b[1]);
	ndo2db_stmt_process_buffered_input(idi, s);
	/* ...and execute. */
	SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

	/* Save the contact notification id for handling contact notifications per
	 * method. */
	if (type == NEBTYPE_CONTACTNOTIFICATION_START) {
		idi->dbinfo.last_contact_notification_id = (status == NDO_OK)
				? (ndo2db_id_t)mysql_stmt_insert_id(s->handle) : 0;
	}

	return status;
}


int ndo2db_stmt_handle_contactnotificationmethoddata(ndo2db_idi *idi) {
	ndo2db_id_t command_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONTACTNOTIFICATIONMETHOD;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Get the command id. */
	NDO2DB_GET_OBJ_ID(COMMAND, bi[NDO_DATA_COMMANDNAME], NULL, &command_id);

	/* Copy everything to bound storage... */
	TO_BOUND_ID(idi->dbinfo.last_contact_notification_id, b[0]);
	TO_BOUND_ID(command_id, b[1]);
	ndo2db_stmt_process_buffered_input(idi, s);
	/* ...and execute. */
	return ndo2db_stmt_execute(idi, s);
}


static int ndo2db_stmt_save_hs_check(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id
) {
	ndo2db_id_t object_id;
	ndo2db_id_t command_id;
	MYSQL_BIND *binds = ndo2db_stmts[stmt_id].param_binds;
	char **bi = idi->buffered_input;

	const my_bool is_host_check = (stmt_id == NDO2DB_STMT_HANDLE_HOSTCHECK);
	const int object_type = is_host_check
			? NDO2DB_OBJECTTYPE_HOST : NDO2DB_OBJECTTYPE_SERVICE;
	const char *name1 = bi[NDO_DATA_HOST];
	const char *name2 = is_host_check ? NULL : bi[NDO_DATA_SERVICE];
	const char *cname = bi[NDO_DATA_COMMANDNAME];

	/* Convert timestamp, etc. */
	DECLARE_AND_CONVERT_STD_DATA;

	if (
#if (defined(BUILD_NAGIOS_3X) || defined(BUILD_NAGIOS_4X))
			/* Skip precheck events, they're not useful to us. */
			type == NEBTYPE_SERVICECHECK_ASYNC_PRECHECK ||
			type == NEBTYPE_HOSTCHECK_ASYNC_PRECHECK ||
			type == NEBTYPE_HOSTCHECK_SYNC_PRECHECK ||
#endif
			(
					/* Only process initiated or processed service check data... */
					!is_host_check &&
					type != NEBTYPE_SERVICECHECK_INITIATE &&
					type != NEBTYPE_SERVICECHECK_PROCESSED
			)
	) {
		return NDO_OK;
	}

	/* Fetch our object id. */
	ndo2db_get_obj_id(idi, object_type, name1, name2, &object_id);
	/* Fetch our command id, or zero if we don't have a command name. */
	NDO2DB_GET_OBJ_ID(COMMAND, cname, NULL, &command_id);

	/* Covert/copy our input data to bound parameter storage. */
	TO_BOUND_ID(object_id, binds[0]);
	TO_BOUND_ID(command_id, binds[1]);
	/* Host checks have an additional 'is_raw_check' column. */
	if (is_host_check) {
		TO_BOUND_BOOL(
				(type == NEBTYPE_HOSTCHECK_RAW_START || type == NEBTYPE_HOSTCHECK_RAW_END),
				binds[2]);
	}
	ndo2db_stmt_process_buffered_input(idi, ndo2db_stmts + stmt_id);

	/* Now save the check. */
	return ndo2db_stmt_execute(idi, ndo2db_stmts + stmt_id);
}

int ndo2db_stmt_handle_hostcheckdata(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_check(idi, NDO2DB_STMT_HANDLE_HOSTCHECK);
}

int ndo2db_stmt_handle_servicecheckdata(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_check(idi, NDO2DB_STMT_HANDLE_SERVICECHECK);
}


int ndo2db_stmt_handle_commentdata(ndo2db_idi *idi) {
	int8_t comment_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s;
	MYSQL_BIND *b;
	char **bi = idi->buffered_input;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our comment type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_COMMENTTYPE], &comment_type);
	GET_OPTIONAL_HS_ID(comment_type, HOST_COMMENT, SERVICE_COMMENT, &object_id);

	switch (type) {

	case NEBTYPE_COMMENT_ADD:
	case NEBTYPE_COMMENT_LOAD:
		/* Save a history of comments that get added or get loaded. */
		s = ndo2db_stmts + NDO2DB_STMT_COMMENTHISTORY_ADD;
		b = s->param_binds;
		COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* entry_time, entry_time_usec */
		TO_BOUND_ID(object_id, b[2]);
		TO_BOUND_INT8(comment_type, b[3]);
		ndo2db_stmt_process_buffered_input(idi, s);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

		if (tstamp.tv_sec >= idi->dbinfo.latest_realtime_data_time) {
			/* Save new live comments that get added. The binds all point to the same
			 * data, only the statement and its table differ, so no need to muck
			 * about moving data. */
			s = ndo2db_stmts + NDO2DB_STMT_COMMENT_ADD;
			SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
		}
		break;

	case NEBTYPE_COMMENT_DELETE:
		/* Record a history of comments that get deleted. */
		s = ndo2db_stmts + NDO2DB_STMT_COMMENTHISTORY_DELETE;
		b = s->param_binds;
		COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* deletion_time, deletion_time_usec */
		/* b[2] (comment_time) and b[3] (internal_comment_id) are auto converted. */
		ndo2db_stmt_process_buffered_input(idi, s);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

		if (tstamp.tv_sec >= idi->dbinfo.latest_realtime_data_time) {
			/* Remove deleted live comments. */
			s = ndo2db_stmts + NDO2DB_STMT_COMMENT_DELETE;
			ndo2db_stmt_process_buffered_input(idi, s);
			SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
		}
		break;

	default:
		break;
	}

	return status;
}


int ndo2db_stmt_handle_downtimedata(ndo2db_idi *idi) {
	int8_t downtime_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s;
	MYSQL_BIND *b;
	char **bi = idi->buffered_input;
	int status = NDO_OK;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our downtime type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_DOWNTIMETYPE], &downtime_type);
	GET_OPTIONAL_HS_ID(downtime_type, HOST_DOWNTIME, SERVICE_DOWNTIME, &object_id);

	switch (type) {

	case NEBTYPE_DOWNTIME_ADD:
	case NEBTYPE_DOWNTIME_LOAD:
		/* Save a history of scheduled downtime that gets added or get loaded. */
		s = ndo2db_stmts + NDO2DB_STMT_DOWNTIMEHISTORY_ADD;
		b = s->param_binds;
		TO_BOUND_ID(object_id, b[0]);
		TO_BOUND_INT8(downtime_type, b[1]);
		ndo2db_stmt_process_buffered_input(idi, s);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

		if (tstamp.tv_sec >= idi->dbinfo.latest_realtime_data_time) {
			/* Save new live scheduled downtime that gets added. The binds all point
			 * to the same data, so we're ready to execute. */
			s = ndo2db_stmts + NDO2DB_STMT_DOWNTIME_ADD;
			SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
		}
		break;

	case NEBTYPE_DOWNTIME_START:
		/* Save a history of scheduled downtime starting. */
		s = ndo2db_stmts + NDO2DB_STMT_DOWNTIMEHISTORY_START;
		b = s->param_binds;
		COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* actual_start_time, usec */
		TO_BOUND_BOOL(1, b[2]); /* was_started */
		TO_BOUND_ID(object_id, b[3]);
		TO_BOUND_INT8(downtime_type, b[4]);
		ndo2db_stmt_process_buffered_input(idi, s);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

		if (tstamp.tv_sec >= idi->dbinfo.latest_realtime_data_time) {
			/* Update live scheduled downtime that starts. */
			s = ndo2db_stmts + NDO2DB_STMT_DOWNTIME_START;
			SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
		}
		break;

	case NEBTYPE_DOWNTIME_STOP:
		/* Save a history of scheduled downtime ending. */
		s = ndo2db_stmts + NDO2DB_STMT_DOWNTIMEHISTORY_STOP;
		b = s->param_binds;
		COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* actual_end_time, usec */
		TO_BOUND_BOOL((attr == NEBATTR_DOWNTIME_STOP_CANCELLED), b[2]); /* was_canceled */
		TO_BOUND_ID(object_id, b[3]);
		TO_BOUND_INT8(downtime_type, b[4]);
		ndo2db_stmt_process_buffered_input(idi, s);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, s));

		/* Fall-through: NEBTYPE_DOWNTIME_STOP and NEBTYPE_DOWNTIME_DELETE have
		 * identical live data handling. */

	case NEBTYPE_DOWNTIME_DELETE:
		if (tstamp.tv_sec >= idi->dbinfo.latest_realtime_data_time) {
			/* Remove completed or deleted live scheduled downtime. */
			s = ndo2db_stmts + NDO2DB_STMT_DOWNTIME_STOP;
			b = s->param_binds;
			TO_BOUND_ID(object_id, b[0]);
			TO_BOUND_INT8(downtime_type, b[1]);
			ndo2db_stmt_process_buffered_input(idi, s);
			SAVE_ERR(status, ndo2db_stmt_execute(idi, s));
		}
		break;

	default:
		break;
	}

	return status;
}


int ndo2db_stmt_handle_flappingdata(ndo2db_idi *idi) {
	int8_t flapping_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_FLAPPING;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our flapping type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_FLAPPINGTYPE], &flapping_type);
	GET_OPTIONAL_HS_ID(flapping_type, HOST_FLAPPING, SERVICE_FLAPPING, &object_id);

	COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* event_time, event_time_usec */
	TO_BOUND_INT8(type, b[2]); /* event_type */
	TO_BOUND_INT8(attr, b[3]); /* reason_type */
	TO_BOUND_INT8(flapping_type, b[4]);
	TO_BOUND_ID(object_id, b[5]);
	ndo2db_stmt_process_buffered_input(idi, s);

	return ndo2db_stmt_execute(idi, s);
}


int ndo2db_stmt_handle_programstatusdata(ndo2db_idi *idi) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_PROGRAMSTATUS;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	TO_BOUND_UINT32(tstamp.tv_sec, s->param_binds[0]); /* status_update_time */
	TO_BOUND_BOOL(1, s->param_binds[1]); /* is_currently_running... */
	ndo2db_stmt_process_buffered_input(idi, s);

	return ndo2db_stmt_execute(idi, s);
}


static int ndo2db_stmt_save_customvariable_status(ndo2db_idi *idi,
		ndo2db_id_t o_id, time_t t);

static int ndo2db_stmt_save_hs_status(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id,
	int obj_type,
	const char *obj_name1,
	const char *obj_name2,
	const char *ctp_name
) {
	ndo2db_id_t object_id;
	ndo2db_id_t check_timeperiod_object_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Fetch our object ids. */
	ndo2db_get_obj_id(idi, obj_type, obj_name1, obj_name2, &object_id);
	NDO2DB_GET_OBJ_ID(TIMEPERIOD, ctp_name, NULL, &check_timeperiod_object_id);

	/* Covert/copy our input data to bound parameter storage. */
	TO_BOUND_ID(object_id, binds[0]);
	TO_BOUND_UINT32(tstamp.tv_sec, binds[1]);
	TO_BOUND_ID(check_timeperiod_object_id, binds[2]);
	ndo2db_stmt_process_buffered_input(idi, stmt);

	/* Save the host/service status... */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	/* ...and any custom variable statuses. */
	return ndo2db_stmt_save_customvariable_status(idi, object_id, tstamp.tv_sec);
}

int ndo2db_stmt_handle_hoststatusdata(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_status(
		idi,
		NDO2DB_STMT_HANDLE_HOSTSTATUS,
		NDO2DB_OBJECTTYPE_HOST,
		idi->buffered_input[NDO_DATA_HOST],
		NULL,
		idi->buffered_input[NDO_DATA_HOSTCHECKPERIOD]
	);
}

int ndo2db_stmt_handle_servicestatusdata(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_status(
		idi,
		NDO2DB_STMT_HANDLE_SERVICESTATUS,
		NDO2DB_OBJECTTYPE_SERVICE,
		idi->buffered_input[NDO_DATA_HOST],
		idi->buffered_input[NDO_DATA_SERVICE],
		idi->buffered_input[NDO_DATA_SERVICECHECKPERIOD]
	);
}


int ndo2db_stmt_handle_contactstatusdata(ndo2db_idi *idi) {
	ndo2db_id_t contact_object_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONTACTSTATUS;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our contact object id. */
	NDO2DB_GET_OBJ_ID(CONTACT,
			idi->buffered_input[NDO_DATA_CONTACTNAME], NULL, &contact_object_id);

	/* Copy our id and time to bound storage and auto process the rest. */
	TO_BOUND_ID(contact_object_id, s->param_binds[0]);
	TO_BOUND_UINT32(tstamp.tv_sec, s->param_binds[1]); /* status_update_time */
	ndo2db_stmt_process_buffered_input(idi, s);

	/* Save the contact status... */
	CHK_OK(ndo2db_stmt_execute(idi, s));
	/* ...and any custom variable statuses. */
	return ndo2db_stmt_save_customvariable_status(idi, contact_object_id, tstamp.tv_sec);
}


int ndo2db_stmt_handle_adaptiveprogramdata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_adaptivehostdata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_adaptiveservicedata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_adaptivecontactdata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_externalcommanddata(ndo2db_idi *idi) {
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_EXTERNALCOMMAND;

	DECLARE_AND_CONVERT_STD_DATA;
	/* Only handle start events. */
	if (type != NEBTYPE_EXTERNALCOMMAND_START) return NDO_OK;

	ndo2db_stmt_process_buffered_input(idi, s);
	return ndo2db_stmt_execute(idi, s);
}


int ndo2db_stmt_handle_aggregatedstatusdata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_retentiondata(ndo2db_idi *idi) {
	(void)idi;
	/* Ignored as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_acknowledgementdata(ndo2db_idi *idi) {
	int8_t acknowledgement_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_ACKNOWLEDGEMENT;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;

	DECLARE_AND_CONVERT_STD_DATA;

	/* Convert our acknowledgement type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_ACKNOWLEDGEMENTTYPE], &acknowledgement_type);
	GET_OPTIONAL_HS_ID(acknowledgement_type,
			HOST_ACKNOWLEDGEMENT, SERVICE_ACKNOWLEDGEMENT, &object_id);

	COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* entry_time, entry_time_usec */
	TO_BOUND_INT8(acknowledgement_type, b[2]);
	TO_BOUND_ID(object_id, b[3]);
	ndo2db_stmt_process_buffered_input(idi, s);

	return ndo2db_stmt_execute(idi, s);
}


int ndo2db_stmt_handle_statechangedata(ndo2db_idi *idi) {
	int8_t statechange_type;
	ndo2db_id_t object_id;
	struct ndo2db_stmt *s = ndo2db_stmts + NDO2DB_STMT_HANDLE_STATECHANGE;
	MYSQL_BIND *b = s->param_binds;
	char **bi = idi->buffered_input;

	DECLARE_AND_CONVERT_STD_DATA;
	/* Only process completed state changes. */
	if (type != NEBTYPE_STATECHANGE_END) return NDO_OK;

	/* Convert our statechange type and use it to get our object id. */
	ndo_checked_strtoint8(bi[NDO_DATA_STATECHANGETYPE], &statechange_type);
	GET_OPTIONAL_HS_ID(statechange_type,
			HOST_STATECHANGE, SERVICE_STATECHANGE, &object_id);

	COPY_TV_TO_BOUND_TV(tstamp, b[0], b[1]); /* state_time, state_time_usec */
	TO_BOUND_ID(object_id, b[2]);
	ndo2db_stmt_process_buffered_input(idi, s);

	return ndo2db_stmt_execute(idi, s);
}


int ndo2db_stmt_handle_configfilevariables(ndo2db_idi *idi,
		int configfile_type) {
	ndo2db_id_t configfile_id;
	ndo2db_mbuf *mbuf;
	size_t i;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONFIGFILE;
	MYSQL_BIND *binds = stmt->param_binds;
	int status = NDO_OK;

	/* Declare and convert timestamp, etc. */
	DECLARE_AND_CONVERT_STD_DATA;
	ndo2db_log_debug_info(NDO2DB_DEBUGL_SQL, 0,
			"ndo2db_stmt_handle_configfilevariables: tstamp: %lu, latest: %lu\n",
			(unsigned long)tstamp.tv_sec,
			(unsigned long)idi->dbinfo.latest_realtime_data_time);
	/* Don't store old data. */
	RETURN_OK_IF_STD_DATA_TOO_OLD;

	/* Copy our input data to bound storage... */
	TO_BOUND_INT16(configfile_type, binds[0]);
	ndo2db_stmt_process_buffered_input(idi, stmt);
	/* ...save the config file, and then fetch its id. */
	status = ndo2db_stmt_execute(idi, stmt);
	configfile_id = (status == NDO_OK)
			? (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle) : 0;

	/* Store our id once here, it doesn't change through the loop. */
	TO_BOUND_ID(configfile_id, binds[0]);

	/* Save individual config file var=val pairs. */
	stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_CONFIGFILEVARIABLE;
	binds = stmt->param_binds;
	mbuf = idi->mbuf + NDO2DB_MBUF_CONFIGFILEVARIABLE;
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *var;
		const char *val;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the var name. */
		if (!(var = strtok(mbuf->buffer[i], "=")) || !*var) continue;
		/* Rest of the input string is the var value. */
		val = strtok(NULL, "\0");

		/* Copy our var=val to bound storage and save. */
		COPY_BIND_STRING_NOT_EMPTY(var, binds[1]);
		COPY_BIND_STRING_OR_EMPTY(val, binds[2]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}


int ndo2db_stmt_handle_configvariables(ndo2db_idi *idi) {
	(void)idi;
	/* No-op as per the string-based handler. */
	return NDO_OK;
}


int ndo2db_stmt_handle_runtimevariables(ndo2db_idi *idi) {
	int status = NDO_OK;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_RUNTIMEVARIABLE;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + NDO2DB_MBUF_RUNTIMEVARIABLE;
	size_t i;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Save individual runtime  pairs. */
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *var;
		const char *val;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the var name. */
		if (!(var = strtok(mbuf->buffer[i], "=")) || !*var) continue;
		/* Rest of the input string is the var value. */
		val = strtok(NULL, "\0");

		/* Copy our var=val to bound storage and save. */
		COPY_BIND_STRING_NOT_EMPTY(var, binds[0]);
		COPY_BIND_STRING_OR_EMPTY(val, binds[1]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}


int ndo2db_stmt_handle_configdumpstart(ndo2db_idi *idi) {
	DECLARE_STD_DATA;
	const char *cdt = idi->buffered_input[NDO_DATA_CONFIGDUMPTYPE];

	/* Convert timestamp, etc. */
	int status = CONVERT_STD_DATA;

	/* Set config dump type: 1 retained, 0 original. */
	idi->current_object_config_type =
			(cdt && strcmp(cdt, NDO_API_CONFIGDUMP_RETAINED) == 0) ? 1 : 0;

	return status;
}


int ndo2db_stmt_handle_configdumpend(ndo2db_idi *idi) {
	(void)idi;

	/* No-op as per the string-based handler, but take the opportunity to report
	 * some info on the object cache now that we should have seen all of our
	 * active objects.
	 * @todo This would be the place to purge inactive objects from the cache and
	 * rehash. */
	ndo2db_log_debug_info(NDO2DB_DEBUGL_CACHE, 0,
			"ndo2db_stmt_handle_configdumpend: object cache: "
			"size=%zu, count=%zu, activated=%zu, collisions=%zu\n",
			ndo2db_objects_size, ndo2db_objects_count, ndo2db_objects_activated,
			ndo2db_objects_collisions);

	return NDO_OK;
}


/**
 * Saves one/many or parent/child id-to-id relations.
 */
static int ndo2db_stmt_save_relations(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id,
	const ndo2db_id_t one_id,
	const size_t mbuf_index,
	const int many_type,
	const char *many_token
) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + mbuf_index;
	size_t i;
	int status = NDO_OK;

	/* Store the 'one' or parent id, it doesn't change through the loop. */
	TO_BOUND_ID(one_id, binds[0]);

	/* Save each 'many' or child id. */
	for (i = 0; i < (size_t)mbuf->used_lines; i++) {
		const char *n1 = mbuf->buffer[i];
		const char *n2 = NULL;
		ndo2db_id_t many_id;
		/* Skip empty names. */
		if (!n1 || !*n1) continue;
		/* Split the name into n1/n2 parts if we have a many_token. */
		if (many_token) {
			n1 = strtok(mbuf->buffer[i], many_token);
			n2 = strtok(NULL, "\0");
			/* Skip empty first names. */
			if (!n1 || !*n1) continue;
			/* Skip services with empty names. */
			if (many_type == NDO2DB_OBJECTTYPE_SERVICE && (!n2 || !*n2)) continue;
		}
		/* Get the 'many' or child id to bound storage and save the relation. */
		SAVE_ERR(status, ndo2db_get_obj_id(idi, many_type, n1, n2, &many_id));
		TO_BOUND_ID(many_id, binds[1]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}


static int ndo2db_stmt_save_customvariables(ndo2db_idi *idi, ndo2db_id_t o_id);


static int ndo2db_stmt_save_hs_definition(
	ndo2db_idi *idi,
	const int object_type,
	const enum ndo2db_stmt_id stmt_id,
	const size_t check_cmd_index,
	const size_t event_cmd_index,
	const size_t check_period_index,
	const size_t notif_period_index,
	const enum ndo2db_stmt_id parent_stmt_id,
	const size_t parent_mbuf_index,
	const enum ndo2db_stmt_id contact_group_stmt_id,
	const enum ndo2db_stmt_id contact_stmt_id
) {
	ndo2db_id_t object_id;
	ndo2db_id_t host_object_id;
	ndo2db_id_t check_command_id = 0;
	ndo2db_id_t event_command_id = 0;
	ndo2db_id_t check_timeperiod_id;
	ndo2db_id_t notif_timeperiod_id;
	const char *check_args = NULL;
	const char *event_args = NULL;
	ndo2db_id_t row_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;
	char **bi = idi->buffered_input;
	size_t x = 0;
	int status = NDO_OK;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get the check command args and object id. */
	if (bi[check_cmd_index] && *bi[check_cmd_index]) {
		char *args = NULL;
		const char *name = strtok_r(bi[check_cmd_index], "!", &args);
		check_args = args;
		NDO2DB_GET_OBJ_ID(COMMAND, name, NULL, &check_command_id);
	}
	/* Get the event handler command args and object id. */
	if (bi[event_cmd_index] && *bi[event_cmd_index]) {
		char *args = NULL;
		const char *name = strtok_r(bi[event_cmd_index], "!", &args);
		event_args = args;
		NDO2DB_GET_OBJ_ID(COMMAND, name, NULL, &event_command_id);
	}

	/* Get our host object id. */
	NDO2DB_GET_OBJ_ID(HOST, bi[NDO_DATA_HOSTNAME], NULL, &host_object_id);
	/* Fetch the service object id if this is a service, otherwise use the host
	 * object id as the definition object id. */
	if (object_type == NDO2DB_OBJECTTYPE_SERVICE) {
		NDO2DB_GET_OBJ_ID(SERVICE,
				bi[NDO_DATA_HOSTNAME], bi[NDO_DATA_SERVICEDESCRIPTION], &object_id);
	}
	else {
		object_id = host_object_id;
	}

	/* Flag the object as being active. */
	ndo2db_set_obj_active(idi, object_type, object_id);

	/* Get the timeperiod object ids. */
	NDO2DB_GET_OBJ_ID(TIMEPERIOD, bi[check_period_index], NULL, &check_timeperiod_id);
	NDO2DB_GET_OBJ_ID(TIMEPERIOD, bi[notif_period_index], NULL, &notif_timeperiod_id);


	/* Covert/copy our data to bound storage. */
	TO_BOUND_ID(host_object_id, binds[x]); ++x;
	TO_BOUND_ID(check_command_id, binds[x]); ++x;
	COPY_BIND_STRING_OR_EMPTY(check_args, binds[x]); ++x;
	TO_BOUND_ID(event_command_id, binds[x]); ++x;
	COPY_BIND_STRING_OR_EMPTY(event_args, binds[x]); ++x;
	TO_BOUND_ID(check_timeperiod_id, binds[x]); ++x;
	TO_BOUND_ID(notif_timeperiod_id, binds[x]); ++x;
	if (object_type == NDO2DB_OBJECTTYPE_SERVICE) TO_BOUND_ID(object_id, binds[x]);
	ndo2db_stmt_process_buffered_input(idi, stmt);


	/* Save the definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt)); /* Do we want to continue on error? */
	row_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);


	/* Save parent hosts/services. Check if the statement exist for Nagios < 4X
	 * cases where there are no parent services. NDO2DB_STMT_NONE and/or zero
	 * (false) means no-op. */
	if (parent_stmt_id) {
		SAVE_ERR(status, ndo2db_stmt_save_relations(idi, parent_stmt_id,
				row_id, parent_mbuf_index, object_type,
				(object_type == NDO2DB_OBJECTTYPE_SERVICE ? ";" : NULL)));
	}

	/* Save contact groups. */
	SAVE_ERR(status, ndo2db_stmt_save_relations(idi, contact_group_stmt_id,
			row_id, NDO2DB_MBUF_CONTACTGROUP, NDO2DB_OBJECTTYPE_CONTACTGROUP, NULL));

	/* Save contacts. */
	SAVE_ERR(status, ndo2db_stmt_save_relations(idi, contact_stmt_id,
			row_id, NDO2DB_MBUF_CONTACT, NDO2DB_OBJECTTYPE_CONTACT, NULL));

	/* Save custom variables. */
	SAVE_ERR(status, ndo2db_stmt_save_customvariables(idi, object_id));

	return status;
}

int ndo2db_stmt_handle_hostdefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_definition(
		idi,
		NDO2DB_OBJECTTYPE_HOST,
		NDO2DB_STMT_HANDLE_HOST,
		NDO_DATA_HOSTCHECKCOMMAND,
		NDO_DATA_HOSTEVENTHANDLER,
		NDO_DATA_HOSTCHECKPERIOD,
		NDO_DATA_HOSTNOTIFICATIONPERIOD,
		NDO2DB_STMT_SAVE_HOSTPARENT,
		NDO2DB_MBUF_PARENTHOST,
		NDO2DB_STMT_SAVE_HOSTCONTACTGROUP,
		NDO2DB_STMT_SAVE_HOSTCONTACT
	);
}

int ndo2db_stmt_handle_servicedefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_definition(
		idi,
		NDO2DB_OBJECTTYPE_SERVICE,
		NDO2DB_STMT_HANDLE_SERVICE,
		NDO_DATA_SERVICECHECKCOMMAND,
		NDO_DATA_SERVICEEVENTHANDLER,
		NDO_DATA_SERVICECHECKPERIOD,
		NDO_DATA_SERVICENOTIFICATIONPERIOD,
#ifdef BUILD_NAGIOS_4X
		NDO2DB_STMT_SAVE_SERVICEPARENT,
		NDO2DB_MBUF_PARENTSERVICE,
#else
		NDO2DB_STMT_NONE, /* Plead the fifth, there is no statement in this case. */
		0,
#endif
		NDO2DB_STMT_SAVE_SERVICECONTACTGROUP,
		NDO2DB_STMT_SAVE_SERVICECONTACT
	);
}


static int ndo2db_stmt_save_hs_group_definition(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id group_stmt_id,
	const int group_type,
	const size_t group_index,
	const enum ndo2db_stmt_id member_stmt_id,
	const int member_type,
	const size_t member_index
) {
	ndo2db_id_t object_id;
	ndo2db_id_t row_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + group_stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our group object id and set the object active. */
	ndo2db_get_obj_id(idi, group_type,
			idi->buffered_input[group_index], NULL, &object_id);
	ndo2db_set_obj_active(idi, group_type, object_id);

	/* Covert/copy our data to bound storage. */
	TO_BOUND_ID(object_id, binds[0]);
	ndo2db_stmt_process_buffered_input(idi, stmt);

	/* Save the definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt)); /* Do we want to continue on error? */
	row_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);

	/* Save group member relations. */
	return ndo2db_stmt_save_relations(idi, member_stmt_id,
			row_id, member_index, member_type,
			(member_type == NDO2DB_OBJECTTYPE_SERVICE ? ";" : NULL));
}

int ndo2db_stmt_handle_hostgroupdefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_group_definition(
		idi,
		NDO2DB_STMT_HANDLE_HOSTGROUP,
		NDO2DB_OBJECTTYPE_HOSTGROUP,
		NDO_DATA_HOSTGROUPNAME,
		NDO2DB_STMT_SAVE_HOSTGROUPMEMBER,
		NDO2DB_OBJECTTYPE_HOST,
		NDO2DB_MBUF_HOSTGROUPMEMBER
	);
}

int ndo2db_stmt_handle_servicegroupdefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_group_definition(
		idi,
		NDO2DB_STMT_HANDLE_SERVICEGROUP,
		NDO2DB_OBJECTTYPE_SERVICEGROUP,
		NDO_DATA_SERVICEGROUPNAME,
		NDO2DB_STMT_SAVE_SERVICEGROUPMEMBER,
		NDO2DB_OBJECTTYPE_SERVICE,
		NDO2DB_MBUF_SERVICEGROUPMEMBER
	);
}


static int ndo2db_stmt_save_hs_dependency_definition(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id,
	const int object_type,
	const char *object_name2,
	const char *depend_name2
) {
	const char *object_name1 = idi->buffered_input[NDO_DATA_HOSTNAME];
	const char *depend_name1 = idi->buffered_input[NDO_DATA_DEPENDENTHOSTNAME];
	const char *timeperiod_name1 = idi->buffered_input[NDO_DATA_DEPENDENCYPERIOD];
	ndo2db_id_t object_id;
	ndo2db_id_t dependent_id;
	ndo2db_id_t timeperiod_id;
	MYSQL_BIND *binds = ndo2db_stmts[stmt_id].param_binds;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our object ids. */
	ndo2db_get_obj_id(idi, object_type, object_name1, object_name2, &object_id);
	ndo2db_get_obj_id(idi, object_type, depend_name1, depend_name2, &dependent_id);
	NDO2DB_GET_OBJ_ID(TIMEPERIOD, timeperiod_name1, NULL, &timeperiod_id);

	/* Covert/copy our data to bound storage... */
	TO_BOUND_ID(object_id, binds[0]);
	TO_BOUND_ID(dependent_id, binds[1]);
	TO_BOUND_ID(timeperiod_id, binds[2]);
	ndo2db_stmt_process_buffered_input(idi, ndo2db_stmts + stmt_id);

	/* ...and save the definition. */
	return ndo2db_stmt_execute(idi, ndo2db_stmts + stmt_id);
}

int ndo2db_stmt_handle_hostdependencydefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_dependency_definition(
		idi,
		NDO2DB_STMT_HANDLE_HOSTDEPENDENCY,
		NDO2DB_OBJECTTYPE_HOST,
		NULL,
		NULL
	);
}

int ndo2db_stmt_handle_servicedependencydefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_save_hs_dependency_definition(
		idi,
		NDO2DB_STMT_HANDLE_SERVICEDEPENDENCY,
		NDO2DB_OBJECTTYPE_SERVICE,
		idi->buffered_input[NDO_DATA_SERVICEDESCRIPTION],
		idi->buffered_input[NDO_DATA_DEPENDENTSERVICEDESCRIPTION]
	);
}


static int ndo2db_stmt_hs_escalation_definition(
	ndo2db_idi *idi,
	const enum ndo2db_stmt_id stmt_id,
	const int object_type,
	const char *object_name2,
	const enum ndo2db_stmt_id contact_group_stmt_id,
	const enum ndo2db_stmt_id contact_stmt_id
) {
	ndo2db_id_t object_id;
	ndo2db_id_t timeperiod_id;
	ndo2db_id_t row_id;
	const char *host_name = idi->buffered_input[NDO_DATA_HOSTNAME];
	const char *timeperiod_name = idi->buffered_input[NDO_DATA_ESCALATIONPERIOD];
	struct ndo2db_stmt *stmt = ndo2db_stmts + stmt_id;
	MYSQL_BIND *binds = stmt->param_binds;
	int status = NDO_OK;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our object ids. */
	ndo2db_get_obj_id(idi, object_type, host_name, object_name2, &object_id);
	NDO2DB_GET_OBJ_ID(TIMEPERIOD, timeperiod_name, NULL, &timeperiod_id);

	/* Covert/copy our data to bound storage. */
	TO_BOUND_ID(object_id, binds[0]);
	TO_BOUND_ID(timeperiod_id, binds[1]);
	ndo2db_stmt_process_buffered_input(idi, ndo2db_stmts + stmt_id);

	/* Save the definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt)); /* Do we want to continue on error? */
	row_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);

	/* Save contact groups. */
	SAVE_ERR(status, ndo2db_stmt_save_relations(idi, contact_group_stmt_id,
			row_id, NDO2DB_MBUF_CONTACTGROUP, NDO2DB_OBJECTTYPE_CONTACTGROUP, NULL));

	/* Save contacts. */
	SAVE_ERR(status, ndo2db_stmt_save_relations(idi, contact_stmt_id,
			row_id, NDO2DB_MBUF_CONTACT, NDO2DB_OBJECTTYPE_CONTACT, NULL));

	return status;
}

int ndo2db_stmt_handle_hostescalationdefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_hs_escalation_definition(
		idi,
		NDO2DB_STMT_HANDLE_HOSTESCALATION,
		NDO2DB_OBJECTTYPE_HOST,
		NULL,
		NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACTGROUP,
		NDO2DB_STMT_SAVE_HOSTESCALATIONCONTACT
	);
}

int ndo2db_stmt_handle_serviceescalationdefinition(ndo2db_idi *idi) {
	return ndo2db_stmt_hs_escalation_definition(
		idi,
		NDO2DB_STMT_HANDLE_SERVICEESCALATION,
		NDO2DB_OBJECTTYPE_SERVICE,
		idi->buffered_input[NDO_DATA_SERVICEDESCRIPTION],
		NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACTGROUP,
		NDO2DB_STMT_SAVE_SERVICEESCALATIONCONTACT
	);
}


int ndo2db_stmt_handle_commanddefinition(ndo2db_idi *idi) {
	ndo2db_id_t object_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_COMMAND;
	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our command object id and set the object active. */
	NDO2DB_GET_OBJ_ID(COMMAND,
			idi->buffered_input[NDO_DATA_COMMANDNAME], NULL, &object_id);
	ndo2db_set_obj_active(idi, NDO2DB_OBJECTTYPE_COMMAND, object_id);

	/* Copy our id and other data to bound storage and save the definition. */
	TO_BOUND_ID(object_id, stmt->param_binds[0]);
	ndo2db_stmt_process_buffered_input(idi, stmt);
	return ndo2db_stmt_execute(idi, stmt);
}


int ndo2db_stmt_handle_timeperiodefinition(ndo2db_idi *idi) {
	int status = NDO_OK;
	ndo2db_id_t object_id;
	ndo2db_id_t row_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_TIMEPERIOD;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + NDO2DB_MBUF_TIMERANGE;
	size_t i;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our command object id and set the object active. */
	NDO2DB_GET_OBJ_ID(TIMEPERIOD,
			idi->buffered_input[NDO_DATA_TIMEPERIODNAME], NULL, &object_id);
	ndo2db_set_obj_active(idi, NDO2DB_OBJECTTYPE_TIMEPERIOD, object_id);

	/* Copy our object id and other data to bound storage... */
	TO_BOUND_ID(object_id, stmt->param_binds[0]);
	ndo2db_stmt_process_buffered_input(idi, stmt);
	/* ...then save the timeperiod definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	row_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);

	/* Get our timerange statement and binds, and store our timerange row id. */
	stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_TIMEPERIODRANGE;
	binds = stmt->param_binds;
	TO_BOUND_ID(row_id, binds[0]);
	/* Save each timerange. */
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *day;
		const char *start;
		const char *end;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the day, and start and end times. */
		if (!(day = strtok(mbuf->buffer[i], ":")) || !*day) continue;
		if (!(start = strtok(NULL, "-")) || !*start) continue;
		if (!(end = strtok(NULL, "\0")) || !*end) continue;
		/* Convert and copy our input data to bound parameter storage... */
		ndo_checked_strtoint16(day, binds[1].buffer);
		ndo_checked_strtouint32(start, binds[2].buffer);
		ndo_checked_strtouint32(end, binds[3].buffer);
		/* ...and save the timerange. */
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}


static int ndo2db_stmt_save_contact_commands(
	ndo2db_idi *idi,
	const ndo2db_id_t contatct_id,
	const int notification_type,
	const size_t mbuf_index
) {
	int status = NDO_OK;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_CONTACTNOTIFICATIONCOMMAND;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + mbuf_index;
	size_t i;

	TO_BOUND_ID(contatct_id, binds[0]);
	TO_BOUND_INT8(notification_type, binds[1]);

	/* Save each host notification command. */
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		ndo2db_id_t cmd_id;
		const char *cmd_name;
		const char *cmd_args;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the command name and arguments. */
		if (!(cmd_name = strtok(mbuf->buffer[i], "!")) || !*cmd_name) continue;
		cmd_args = strtok(NULL, "\0");
		/* Find the command id, skip this item if unsuccessful. */
		NDO2DB_GET_OBJ_ID(COMMAND, cmd_name, NULL, &cmd_id);
		if (!cmd_id) {
			status = NDO_ERROR;
			continue;
		}
		/* Convert and copy our data to bound storage and save the command. */
		TO_BOUND_ID(cmd_id, binds[2]);
		COPY_BIND_STRING_OR_EMPTY(cmd_args, binds[3]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}

int ndo2db_stmt_handle_contactdefinition(ndo2db_idi *idi) {
	ndo2db_id_t object_id;
	ndo2db_id_t host_timeperiod_id;
	ndo2db_id_t service_timeperiod_id;
	ndo2db_id_t contact_row_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONTACT;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf;
	char **bi = idi->buffered_input;
	size_t i;
	int status = NDO_OK;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our contact object id and set the object active. */
	NDO2DB_GET_OBJ_ID(CONTACT, bi[NDO_DATA_CONTACTNAME], NULL, &object_id);
	ndo2db_set_obj_active(idi, NDO2DB_OBJECTTYPE_CONTACT, object_id);

	/* Get the timeperiod object ids. */
	NDO2DB_GET_OBJ_ID(TIMEPERIOD,
			bi[NDO_DATA_HOSTNOTIFICATIONPERIOD], NULL, &host_timeperiod_id);
	NDO2DB_GET_OBJ_ID(TIMEPERIOD,
			bi[NDO_DATA_SERVICENOTIFICATIONPERIOD], NULL, &service_timeperiod_id);

	/* Copy our object ids and other data to bound storage... */
	TO_BOUND_ID(object_id, binds[0]);
	TO_BOUND_ID(host_timeperiod_id, binds[1]);
	TO_BOUND_ID(service_timeperiod_id, binds[2]);
	ndo2db_stmt_process_buffered_input(idi, stmt);

	/* ...then save the contact definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	contact_row_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);


	/* Get our address statement and binds, store our contact row id... */
	stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_CONTACTADDRESS;
	binds = stmt->param_binds;
	TO_BOUND_ID(contact_row_id, binds[0]);
	/* ...and save each address. */
	mbuf = idi->mbuf + NDO2DB_MBUF_CONTACTADDRESS;
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *num;
		const char *adr;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the address number and value. */
		if (!(num = strtok(mbuf->buffer[i], ":")) || !*num) continue;
		if (!(adr = strtok(NULL, "\0")) || !*adr) continue;
		/* Convert and copy our data to bound storage and save the address. */
		ndo_checked_strtoint16(num, binds[1].buffer);
		COPY_BIND_STRING_NOT_EMPTY(adr, binds[2]);
		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	/* Save host notification commands. */
	SAVE_ERR(status, ndo2db_stmt_save_contact_commands(idi, contact_row_id,
			HOST_NOTIFICATION, NDO2DB_MBUF_HOSTNOTIFICATIONCOMMAND));

	/* Save service notification commands. */
	SAVE_ERR(status, ndo2db_stmt_save_contact_commands(idi, contact_row_id,
			SERVICE_NOTIFICATION, NDO2DB_MBUF_SERVICENOTIFICATIONCOMMAND));

	/* Save custom variables. */
	SAVE_ERR(status, ndo2db_stmt_save_customvariables(idi, contact_row_id));

	return status;
}


int ndo2db_stmt_handle_contactgroupdefinition(ndo2db_idi *idi) {
	ndo2db_id_t object_id;
	ndo2db_id_t group_id;
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_HANDLE_CONTACTGROUP;

	DECLARE_CONVERT_STD_DATA_RETURN_OK_IF_TOO_OLD;

	/* Get our contact group object id and set the object active. */
	NDO2DB_GET_OBJ_ID(CONTACTGROUP,
			idi->buffered_input[NDO_DATA_CONTACTGROUPNAME], NULL, &object_id);
	ndo2db_set_obj_active(idi, NDO2DB_OBJECTTYPE_CONTACTGROUP, object_id);

	/* Copy our object id and other input data to bound parameter storage... */
	TO_BOUND_ID(object_id, stmt->param_binds[0]);
	ndo2db_stmt_process_buffered_input(idi, stmt);
	/* ...then save the contact group definition and get its insert id. */
	CHK_OK(ndo2db_stmt_execute(idi, stmt));
	group_id = (ndo2db_id_t)mysql_stmt_insert_id(stmt->handle);

	return ndo2db_stmt_save_relations(idi, NDO2DB_STMT_SAVE_CONTACTGROUPMEMBER,
			group_id, NDO2DB_MBUF_CONTACTGROUPMEMBER, NDO2DB_OBJECTTYPE_CONTACT, NULL);
}


static int ndo2db_stmt_save_customvariables(ndo2db_idi *idi, ndo2db_id_t o_id) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_CUSTOMVARIABLE;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + NDO2DB_MBUF_CUSTOMVARIABLE;
	size_t i;
	int status = NDO_OK;

	/* Save our object id and config type to the bound variable buffers. */
	TO_BOUND_ID(o_id, binds[0]);
	TO_BOUND_INT8(idi->current_object_config_type, binds[1]);

	/* Save each custom variable. */
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *name;
		const char *modified;
		const char *value;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the var name. */
		if (!(name = strtok(mbuf->buffer[i], ":")) || !*name) continue;
		/* Extract the has_been_modified status. */
		if (!(modified = strtok(NULL, ":"))) continue;
		/* Rest of the input string is the var value. */
		value = strtok(NULL, "\n");

		ndo_checked_strtoint8(modified, binds[2].buffer);
		COPY_BIND_STRING_NOT_EMPTY(name, binds[3]);
		COPY_BIND_STRING_OR_EMPTY(value, binds[4]);

		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}


static int ndo2db_stmt_save_customvariable_status(ndo2db_idi *idi,
		ndo2db_id_t o_id, time_t t) {
	struct ndo2db_stmt *stmt = ndo2db_stmts + NDO2DB_STMT_SAVE_CUSTOMVARIABLESTATUS;
	MYSQL_BIND *binds = stmt->param_binds;
	ndo2db_mbuf *mbuf = idi->mbuf + NDO2DB_MBUF_CUSTOMVARIABLE;
	size_t i;
	int status = NDO_OK;

	/* Save our object id and update time to the bound variable buffers. */
	TO_BOUND_ID(o_id, binds[0]);
	TO_BOUND_UINT32(t, binds[1]);

	/* Save each custom variable. */
	for (i = 0; i < (size_t)mbuf->used_lines; ++i) {
		const char *name;
		const char *modified;
		const char *value;
		/* Skip empty buffers. */
		if (!mbuf->buffer[i]) continue;
		/* Extract the var name. */
		if (!(name = strtok(mbuf->buffer[i], ":")) || !*name) continue;
		/* Extract the has_been_modified status. */
		if (!(modified = strtok(NULL, ":"))) continue;
		/* Rest of the input string is the var value. */
		value = strtok(NULL, "\n");

		ndo_checked_strtoint8(modified, binds[2].buffer);
		COPY_BIND_STRING_NOT_EMPTY(name, binds[3]);
		COPY_BIND_STRING_OR_EMPTY(value, binds[4]);

		SAVE_ERR(status, ndo2db_stmt_execute(idi, stmt));
	}

	return status;
}




/** Declare a parameter initializer with no auto convert or flags. */
#define INIT_PARAM(c, t) \
	{ c, BIND_TYPE_ ## t, -1, 0 }

/** Declare a parameter initializer list with flags and no auto convert. */
#define INIT_PARAM_F(c, t, f) \
	{ c, BIND_TYPE_ ## t, -1, f }

/** Declare a parameter initializer list with auto convert and no flags. */
#define INIT_PARAM_BI(c, t, i) \
	{ c, BIND_TYPE_ ## t, i, BIND_BUFFERED_INPUT }

/** Declare a parameter initializer list with auto convert and flags. */
#define INIT_PARAM_BIF(c, t, i, f) \
	{ c, BIND_TYPE_ ## t, i, (f)|BIND_BUFFERED_INPUT }

/** Call ndo2db_stmt_prepare_insert() with specified input params, using common
 * wariables, concatenating statement and table ids with common prefixes. */
#define PREPARE_INSERT_W_PARAMS(s, t, p) \
	ndo2db_stmt_prepare_insert(idi, dbuf, \
			NDO2DB_STMT_ ## s, NDO2DB_DBTABLE_ ## t, p, ARRAY_SIZE(p), 0)

/** Call ndo2db_stmt_prepare_insert() using common wariables, concatenating
 * statement and table ids with common prefixes. */
#define PREPARE_INSERT(s, t) \
	PREPARE_INSERT_W_PARAMS(s, t, params)

/** Call ndo2db_stmt_prepare_insert() with specified input params and
 * update_on_dup using common wariables, concatenating statement and table ids
 * with common prefixes. */
#define PREPARE_INSERT_UPDATE_W_PARAMS(s, t, p) \
	ndo2db_stmt_prepare_insert(idi, dbuf, \
			NDO2DB_STMT_ ## s, NDO2DB_DBTABLE_ ## t, p, ARRAY_SIZE(p), 1)

/** Call ndo2db_stmt_prepare_insert() with update_on_dup using common wariables,
 * concatenating statement and table ids with common prefixes. */
#define PREPARE_INSERT_UPDATE(s, t) \
	PREPARE_INSERT_UPDATE_W_PARAMS(s, t, params)

/** Prints a template from table and instance ids, then prepare and bind. */
#define CHK_OK_PRINT_PREPARE_AND_BIND(fmt, s, t, p, np, r, nr) \
	ndo_dbuf_reset(dbuf); \
	CHK_OK(ndo_dbuf_printf(dbuf, fmt, \
			ndo2db_db_tablenames[NDO2DB_DBTABLE_ ## t], idi->dbinfo.instance_id)); \
	CHK_OK(ndo2db_stmt_prepare_and_bind(idi, NDO2DB_STMT_ ## s, \
			dbuf->buf, dbuf->used_size, p, np, r, nr))


static int ndo2db_stmt_init_obj(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	/* These param/result descriptions are shared by all five object id related
	 * statments, each statement still has its own MYSQL_BINDs. */
	static const struct ndo2db_stmt_bind binding_info[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM("objecttype_id", INT8),
		INIT_PARAM("name1", SHORT_STRING),
		INIT_PARAM_F("name2", SHORT_STRING, BIND_MAYBE_NULL),
	};
	const struct ndo2db_stmt_bind *params = (binding_info + 1);
	const struct ndo2db_stmt_bind *results = (binding_info + 0);
	const char *table_name = ndo2db_db_tablenames[NDO2DB_DBTABLE_OBJECTS];

	/* Our SELECT for name2 IS NOT NULL cases.
	 * params: objecttype_id, name1, name2; result: object_id
	 * The BINARY operator is a MySQL special for case sensitivity. */
	CHK_OK(ndo2db_stmt_prepare_select(idi, dbuf,
			NDO2DB_STMT_GET_OBJ_ID, table_name, params, 3, results, 1,
			"objecttype_id=? AND BINARY name1=? AND BINARY name2=?"));

	/* Our SELECT for name2 IS NULL cases.
	* params: objecttype_id, name1; result: object_id */
	CHK_OK(ndo2db_stmt_prepare_select(idi, dbuf,
			NDO2DB_STMT_GET_OBJ_ID_N2_NULL, table_name, params, 2, results, 1,
			"objecttype_id=? AND BINARY name1=? AND name2 IS NULL"));

	/* Our object id INSERT.
	 * params: objecttype_id, name1, name2; no results */
	CHK_OK(ndo2db_stmt_prepare_insert(idi, dbuf,
			NDO2DB_STMT_GET_OBJ_ID_INSERT, NDO2DB_DBTABLE_OBJECTS, params, 3, 0));

	/* Our SELECT for loading all previously active objects.
	 * no params; results: object_id, objecttype_id, name1, name2 */
	CHK_OK(ndo2db_stmt_prepare_select(idi, dbuf,
			NDO2DB_STMT_GET_OBJ_IDS, table_name, NULL, 0, results, 4,
			"is_active=1"));

	/* Our UPDATE for marking an object active.
	 * params: object_id, objecttype_id; no results */
	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET is_active=1 WHERE instance_id=%lu "
			"AND object_id=? AND objecttype_id=?",
			SET_OBJ_ACTIVE, OBJECTS, (binding_info + 0), 2, NULL, 0);

	return NDO_OK;
}


static int ndo2db_stmt_init_log(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM_BI("logentry_time", FROM_UNIXTIME, NDO_DATA_LOGENTRYTIME),
		INIT_PARAM_BI("logentry_data", SHORT_STRING, NDO_DATA_LOGENTRY),
		INIT_PARAM_BI("logentry_type", INT32, NDO_DATA_LOGENTRYTYPE),
		INIT_PARAM("entry_time", FROM_UNIXTIME),
		INIT_PARAM("entry_time_usec", INT32),
		INIT_PARAM("realtime_data", BOOL),
		INIT_PARAM("inferred_data_extracted", BOOL)
	};
	static const struct ndo2db_stmt_bind find_params[] = {
		INIT_PARAM("logentry_time", FROM_UNIXTIME),
		INIT_PARAM("logentry_data", SHORT_STRING),
	};
	static const struct ndo2db_stmt_bind results[] = {
		INIT_PARAM("COUNT(*)", INT32)
	};

	/* Live and archived and log data INSERT.
	 * params: logentry_time, logentry_data, logentry_type,
	 * entry_time, entry_time_usec (0 for archived data),
	 * realtime_data (0/1), inferred_data_extracted (0/1); no results */
	CHK_OK(PREPARE_INSERT(SAVE_LOG, LOGENTRIES));

	/* SELECT to check for duplicate log entries.
	 * params: logentry_time, logentry_data; result: COUNT(*) */
	return ndo2db_stmt_prepare_select(idi, dbuf,
			NDO2DB_STMT_FIND_LOG, ndo2db_db_tablenames[NDO2DB_DBTABLE_LOGENTRIES],
			find_params, 2, results, 1,
			"logentry_time=FROM_UNIXTIME(?) AND logentry_data=?");
}


static int ndo2db_stmt_init_processdata(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind process_params[] = {
		INIT_PARAM("event_type", INT32),
		INIT_PARAM("event_time", FROM_UNIXTIME),
		INIT_PARAM("event_time_usec", INT32),
		INIT_PARAM_BI("process_id", INT32, NDO_DATA_PROCESSID),
		INIT_PARAM_BI("program_name", SHORT_STRING, NDO_DATA_PROGRAMNAME),
		INIT_PARAM_BI("program_version", SHORT_STRING, NDO_DATA_PROGRAMVERSION),
		INIT_PARAM_BI("program_date", SHORT_STRING, NDO_DATA_PROGRAMDATE)
	};
	static const struct ndo2db_stmt_bind status_params[] = {
		INIT_PARAM("program_end_time", FROM_UNIXTIME)
	};

	CHK_OK(PREPARE_INSERT_W_PARAMS(
			HANDLE_PROCESSDATA, PROCESSEVENTS, process_params));

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET program_end_time=?, "
			"is_currently_running=0 WHERE instance_id=%lu",
			UPDATE_PROCESSDATA_PROGRAMSTATUS, PROGRAMSTATUS,
			status_params, 1, NULL, 0);

	return NDO_OK;
}


static int ndo2db_stmt_init_timedevent(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	#define INIT_PARAMS_TIMEDEVENT(params_name, sec_column, usec_column) \
		static const struct ndo2db_stmt_bind params_name[] = { \
			INIT_PARAM(sec_column, FROM_UNIXTIME), \
			INIT_PARAM(usec_column, INT32), \
			INIT_PARAM("object_id", ID), \
			INIT_PARAM("event_type", INT16), \
			INIT_PARAM_BI("scheduled_time", FROM_UNIXTIME, NDO_DATA_RUNTIME), \
			INIT_PARAM_BI("recurring_event", BOOL, NDO_DATA_RECURRING) \
		}
	INIT_PARAMS_TIMEDEVENT(add_params, "queued_time", "queued_time_usec");
	INIT_PARAMS_TIMEDEVENT(execute_params, "event_time", "event_time_usec");
	INIT_PARAMS_TIMEDEVENT(remove_params, "deletion_time", "deletion_time_usec");
	#undef INIT_PARAMS_TIMEDEVENT
	static const struct ndo2db_stmt_bind queue_rm_params[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM("event_type", INT16),
		INIT_PARAM_BI("scheduled_time", FROM_UNIXTIME, NDO_DATA_RUNTIME),
		INIT_PARAM_BI("recurring_event", BOOL, NDO_DATA_RECURRING)
	};
	const struct ndo2db_stmt_bind *queue_clean_params = queue_rm_params + 2;

	/* Iff NDO2DB_SAVE_TIMEDEVENTS_HISTORY: INSERT/UPDATE added events. */
	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			TIMEDEVENT_ADD, TIMEDEVENTS, add_params));

	/* Iff NDO2DB_SAVE_TIMEDEVENTS_HISTORY: INSERT/UPDATE executed events. */
	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			TIMEDEVENT_EXECUTE, TIMEDEVENTS, execute_params));

	/* Iff NDO2DB_SAVE_TIMEDEVENTS_HISTORY: UPDATE removed events.*/
	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET deletion_time=FROM_UNIXTIME(?), deletion_time_usec=? "
			"WHERE instance_id=%lu AND object_id=? AND event_type=? "
			"AND scheduled_time=FROM_UNIXTIME(?) AND recurring_event=?",
			TIMEDEVENT_REMOVE, TIMEDEVENTS, remove_params, 6, NULL, 0);


	/* DELETE old queued events on connection startup, and check execution. */
	CHK_OK_PRINT_PREPARE_AND_BIND(
			"DELETE FROM %s WHERE instance_id=%lu "
			"AND scheduled_time<FROM_UNIXTIME(?)",
			TIMEDEVENTQUEUE_CLEAN, TIMEDEVENTQUEUE, queue_clean_params, 1, NULL, 0);

	/* INSERT enqueued events. */
	CHK_OK(PREPARE_INSERT_W_PARAMS(
			TIMEDEVENTQUEUE_ADD, TIMEDEVENTQUEUE, add_params));

	/* DELETE removed/executed queued events. */
	CHK_OK_PRINT_PREPARE_AND_BIND(
			"DELETE FROM %s "
			"WHERE instance_id=%lu AND object_id=? AND event_type=? "
			"AND scheduled_time=FROM_UNIXTIME(?) AND recurring_event=?",
			TIMEDEVENTQUEUE_REMOVE, TIMEDEVENTQUEUE, queue_rm_params, 4, NULL, 0);

	return NDO_OK;
}


static int ndo2db_stmt_init_systemcommand(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("command_line", SHORT_STRING, NDO_DATA_COMMANDLINE),
		INIT_PARAM_BI("timeout", INT16, NDO_DATA_TIMEOUT),
		INIT_PARAM_BI("early_timeout", BOOL, NDO_DATA_EARLYTIMEOUT),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("return_code", INT16, NDO_DATA_RETURNCODE),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_SYSTEMCOMMAND, SYSTEMCOMMANDS);
}


static int ndo2db_stmt_init_eventhandler(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("eventhandler_type", INT8),
		INIT_PARAM("object_id", ID),
		INIT_PARAM("command_object_id", ID),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("command_args", SHORT_STRING, NDO_DATA_COMMANDARGS),
		INIT_PARAM_BI("command_line", SHORT_STRING, NDO_DATA_COMMANDLINE),
		INIT_PARAM_BI("timeout", INT16, NDO_DATA_TIMEOUT),
		INIT_PARAM_BI("early_timeout", BOOL, NDO_DATA_EARLYTIMEOUT),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("return_code", INT16, NDO_DATA_RETURNCODE),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_EVENTHANDLER, EVENTHANDLERS);
}


static int ndo2db_stmt_init_notification(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("notification_type", INT8),
		INIT_PARAM("object_id", ID),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("notification_reason", INT8, NDO_DATA_NOTIFICATIONREASON),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT),
		INIT_PARAM_BI("escalated", BOOL, NDO_DATA_ESCALATED),
		INIT_PARAM_BI("contacts_notified", BOOL, NDO_DATA_CONTACTSNOTIFIED)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_NOTIFICATION, NOTIFICATIONS);
}


static int ndo2db_stmt_init_contactnotification(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("notification_id", ID),
		INIT_PARAM("contact_object_id", ID),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
	};
	return PREPARE_INSERT_UPDATE(
			HANDLE_CONTACTNOTIFICATION, CONTACTNOTIFICATIONS);
}


static int ndo2db_stmt_init_contactnotificationmethod(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("contactnotification_id", ID),
		INIT_PARAM("command_object_id", ID),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("command_args", SHORT_STRING, NDO_DATA_COMMANDARGS)
	};
	return PREPARE_INSERT_UPDATE(
			HANDLE_CONTACTNOTIFICATIONMETHOD, CONTACTNOTIFICATIONMETHODS);
}


static int ndo2db_stmt_init_comment(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind add_params[] = {
		INIT_PARAM_F("entry_time", FROM_UNIXTIME, BIND_ONLY_INS),
		INIT_PARAM_F("entry_time_usec", INT32, BIND_ONLY_INS),
		INIT_PARAM("object_id", ID),
		INIT_PARAM("comment_type", INT8),
		INIT_PARAM_BI("comment_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("internal_comment_id", UINT32, NDO_DATA_COMMENTID),
		INIT_PARAM_BI("entry_type", INT8, NDO_DATA_ENTRYTYPE),
		INIT_PARAM_BI("author_name", SHORT_STRING, NDO_DATA_AUTHORNAME),
		INIT_PARAM_BI("comment_data", SHORT_STRING, NDO_DATA_COMMENT),
		INIT_PARAM_BI("is_persistent", BOOL, NDO_DATA_PERSISTENT),
		INIT_PARAM_BI("comment_source", INT8, NDO_DATA_SOURCE),
		INIT_PARAM_BI("expires", BOOL, NDO_DATA_EXPIRES),
		INIT_PARAM_BI("expiration_time", FROM_UNIXTIME, NDO_DATA_EXPIRATIONTIME)
	};
	static const struct ndo2db_stmt_bind delete_params[] = {
		INIT_PARAM("deletion_time", FROM_UNIXTIME),
		INIT_PARAM("deletion_time_usec", INT32),
		INIT_PARAM_BI("comment_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("internal_comment_id", UINT32, NDO_DATA_COMMENTID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			COMMENTHISTORY_ADD, COMMENTHISTORY, add_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			COMMENT_ADD, COMMENTS, add_params));

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET deletion_time=FROM_UNIXTIME(?), deletion_time_usec=? "
			"WHERE instance_id=%lu "
			"AND comment_time=FROM_UNIXTIME(?) AND internal_comment_id=?",
			COMMENTHISTORY_DELETE, COMMENTHISTORY, delete_params, 4, NULL, 0);

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"DELETE FROM %s WHERE instance_id=%lu "
			"AND comment_time=FROM_UNIXTIME(?) AND internal_comment_id=?",
			COMMENT_DELETE, COMMENTS, (delete_params + 2), 2, NULL, 0);

	return NDO_OK;
}


static int ndo2db_stmt_init_downtime(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind add_params[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM("downtime_type", INT8),
		INIT_PARAM_BI("author_name", SHORT_STRING, NDO_DATA_AUTHORNAME),
		INIT_PARAM_BI("comment_data", SHORT_STRING, NDO_DATA_COMMENT),
		INIT_PARAM_BI("internal_downtime_id", UINT32, NDO_DATA_DOWNTIMEID),
		INIT_PARAM_BI("triggered_by_id", UINT32, NDO_DATA_TRIGGEREDBY),
		INIT_PARAM_BI("is_fixed", BOOL, NDO_DATA_FIXED),
		INIT_PARAM_BI("duration", UINT32, NDO_DATA_DURATION),
		INIT_PARAM_BI("entry_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("scheduled_start_time", FROM_UNIXTIME, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("scheduled_end_time", FROM_UNIXTIME, NDO_DATA_ENDTIME)
	};
	static const struct ndo2db_stmt_bind start_params[] = {
		INIT_PARAM("actual_start_time", FROM_UNIXTIME),
		INIT_PARAM("actual_start_time_usec", INT32),
		INIT_PARAM("was_started", BOOL), // Constant 1...
		INIT_PARAM("object_id", ID),
		INIT_PARAM("downtime_type", INT8),
		INIT_PARAM_BI("entry_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("scheduled_start_time", FROM_UNIXTIME, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("scheduled_end_time", FROM_UNIXTIME, NDO_DATA_ENDTIME)
	};
	static const struct ndo2db_stmt_bind stop_params[] = {
		INIT_PARAM("actual_end_time", FROM_UNIXTIME),
		INIT_PARAM("actual_end_time_usec", INT32),
		INIT_PARAM("was_cancelled", BOOL),
		INIT_PARAM("object_id", ID),
		INIT_PARAM("downtime_type", INT8),
		INIT_PARAM_BI("entry_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("scheduled_start_time", FROM_UNIXTIME, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("scheduled_end_time", FROM_UNIXTIME, NDO_DATA_ENDTIME)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			DOWNTIMEHISTORY_ADD, DOWNTIMEHISTORY, add_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			DOWNTIME_ADD, SCHEDULEDDOWNTIME, add_params));

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET actual_start_time=FROM_UNIXTIME(?), "
			"actual_start_time_usec=?, was_started=? WHERE instance_id=%lu "
			"AND object_id=? "
			"AND entry_time=FROM_UNIXTIME(?) "
			"AND downtime_type=? "
			"AND scheduled_start_time=FROM_UNIXTIME(?) "
			"AND scheduled_end_time=FROM_UNIXTIME(?)",
			DOWNTIMEHISTORY_START, DOWNTIMEHISTORY, start_params, 8, NULL, 0);

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET actual_start_time=FROM_UNIXTIME(?), "
			"actual_start_time_usec=?, was_started=? WHERE instance_id=%lu "
			"AND object_id=? "
			"AND entry_time=FROM_UNIXTIME(?) "
			"AND downtime_type=? "
			"AND scheduled_start_time=FROM_UNIXTIME(?) "
			"AND scheduled_end_time=FROM_UNIXTIME(?)",
			DOWNTIME_START, SCHEDULEDDOWNTIME, start_params, 8, NULL, 0);

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"UPDATE %s SET actual_end_time=FROM_UNIXTIME(?), actual_end_time_usec=?, "
			"was_cancelled=? WHERE instance_id=%lu "
			"AND object_id=? "
			"AND entry_time=FROM_UNIXTIME(?) "
			"AND downtime_type=? "
			"AND scheduled_start_time=FROM_UNIXTIME(?) "
			"AND scheduled_end_time=FROM_UNIXTIME(?)",
			DOWNTIMEHISTORY_STOP, DOWNTIMEHISTORY, stop_params, 8, NULL, 0);

	CHK_OK_PRINT_PREPARE_AND_BIND(
			"DELETE FROM %s WHERE instance_id=%lu "
			"AND object_id=? "
			"AND entry_time=FROM_UNIXTIME(?) "
			"AND downtime_type=? "
			"AND scheduled_start_time=FROM_UNIXTIME(?) "
			"AND scheduled_end_time=FROM_UNIXTIME(?)",
			DOWNTIME_STOP, SCHEDULEDDOWNTIME, (stop_params + 3), 5, NULL, 0);

	return NDO_OK;
}


static int ndo2db_stmt_init_flapping(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("event_time", FROM_UNIXTIME),
		INIT_PARAM("event_time_usec", INT32),
		INIT_PARAM("event_type", INT8),
		INIT_PARAM("reason_type", INT8),
		INIT_PARAM("flapping_type", INT8),
		INIT_PARAM("object_id", ID),
		INIT_PARAM_BI("percent_state_change", DOUBLE, NDO_DATA_PERCENTSTATECHANGE),
		INIT_PARAM_BI("low_threshold", DOUBLE, NDO_DATA_LOWTHRESHOLD),
		INIT_PARAM_BI("high_threshold", DOUBLE, NDO_DATA_HIGHTHRESHOLD),
		INIT_PARAM_BI("comment_time", FROM_UNIXTIME, NDO_DATA_COMMENTTIME),
		INIT_PARAM_BI("internal_comment_id", UINT32, NDO_DATA_COMMENTID)
	};
	return PREPARE_INSERT(HANDLE_FLAPPING, FLAPPINGHISTORY);
}


static int ndo2db_stmt_init_programstatus(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("status_update_time", FROM_UNIXTIME),
		INIT_PARAM("is_currently_running", BOOL), // Constant 1...
		INIT_PARAM_BI("program_start_time", FROM_UNIXTIME, NDO_DATA_PROGRAMSTARTTIME),
		INIT_PARAM_BI("process_id", INT32, NDO_DATA_PROCESSID),
		INIT_PARAM_BI("daemon_mode", BOOL, NDO_DATA_DAEMONMODE),
		INIT_PARAM_BI("last_command_check", FROM_UNIXTIME, NDO_DATA_LASTCOMMANDCHECK),
		INIT_PARAM_BI("last_log_rotation", FROM_UNIXTIME, NDO_DATA_LASTLOGROTATION),
		INIT_PARAM_BI("notifications_enabled", BOOL, NDO_DATA_NOTIFICATIONSENABLED),
		INIT_PARAM_BI("active_service_checks_enabled", BOOL, NDO_DATA_ACTIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("passive_service_checks_enabled", BOOL, NDO_DATA_PASSIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("active_host_checks_enabled", BOOL, NDO_DATA_ACTIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("passive_host_checks_enabled", BOOL, NDO_DATA_PASSIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("event_handlers_enabled", BOOL, NDO_DATA_EVENTHANDLERSENABLED),
		INIT_PARAM_BI("flap_detection_enabled", BOOL, NDO_DATA_FLAPDETECTIONENABLED),
		INIT_PARAM_BI("failure_prediction_enabled", BOOL, NDO_DATA_FAILUREPREDICTIONENABLED),
		INIT_PARAM_BI("process_performance_data", BOOL, NDO_DATA_PROCESSPERFORMANCEDATA),
		INIT_PARAM_BI("obsess_over_hosts", BOOL, NDO_DATA_OBSESSOVERHOSTS),
		INIT_PARAM_BI("obsess_over_services", BOOL, NDO_DATA_OBSESSOVERSERVICES),
		INIT_PARAM_BI("modified_host_attributes", INT32, NDO_DATA_MODIFIEDHOSTATTRIBUTES),
		INIT_PARAM_BI("modified_service_attributes", INT32, NDO_DATA_MODIFIEDSERVICEATTRIBUTES),
		INIT_PARAM_BI("global_host_event_handler", SHORT_STRING, NDO_DATA_GLOBALHOSTEVENTHANDLER),
		INIT_PARAM_BI("global_service_event_handler", SHORT_STRING, NDO_DATA_GLOBALSERVICEEVENTHANDLER)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_PROGRAMSTATUS, PROGRAMSTATUS);
}


static int ndo2db_stmt_init_hostcheck(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM_F("command_object_id", ID, BIND_ONLY_INS),
		INIT_PARAM("is_raw_check", BOOL),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("check_type", INT8, NDO_DATA_CHECKTYPE),
		INIT_PARAM_BI("current_check_attempt", INT16, NDO_DATA_CURRENTCHECKATTEMPT),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXCHECKATTEMPTS),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("timeout", INT16, NDO_DATA_TIMEOUT),
		INIT_PARAM_BI("early_timeout", BOOL, NDO_DATA_EARLYTIMEOUT),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("latency", DOUBLE, NDO_DATA_LATENCY),
		INIT_PARAM_BI("return_code", INT16, NDO_DATA_RETURNCODE),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT),
		INIT_PARAM_BI("perfdata", LONG_STRING, NDO_DATA_PERFDATA),
		INIT_PARAM_BIF("command_args", SHORT_STRING, NDO_DATA_COMMANDARGS, BIND_ONLY_INS),
		INIT_PARAM_BIF("command_line", SHORT_STRING, NDO_DATA_COMMANDLINE, BIND_ONLY_INS)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_HOSTCHECK, HOSTCHECKS);
}


static int ndo2db_stmt_init_servicecheck(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("service_object_id", ID),
		INIT_PARAM_F("command_object_id", ID, BIND_ONLY_INS),
		INIT_PARAM_BI("start_time", TV_SEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("start_time_usec", TV_USEC, NDO_DATA_STARTTIME),
		INIT_PARAM_BI("end_time", TV_SEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("end_time_usec", TV_USEC, NDO_DATA_ENDTIME),
		INIT_PARAM_BI("check_type", INT8, NDO_DATA_CHECKTYPE),
		INIT_PARAM_BI("current_check_attempt", INT16, NDO_DATA_CURRENTCHECKATTEMPT),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXCHECKATTEMPTS),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("timeout", INT16, NDO_DATA_TIMEOUT),
		INIT_PARAM_BI("early_timeout", BOOL, NDO_DATA_EARLYTIMEOUT),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("latency", DOUBLE, NDO_DATA_LATENCY),
		INIT_PARAM_BI("return_code", INT16, NDO_DATA_RETURNCODE),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT),
		INIT_PARAM_BI("perfdata", LONG_STRING, NDO_DATA_PERFDATA),
		INIT_PARAM_BIF("command_args", SHORT_STRING, NDO_DATA_COMMANDARGS, BIND_ONLY_INS),
		INIT_PARAM_BIF("command_line", SHORT_STRING, NDO_DATA_COMMANDLINE, BIND_ONLY_INS)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_SERVICECHECK, SERVICECHECKS);
}


static int ndo2db_stmt_init_hoststatus(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM("status_update_time", FROM_UNIXTIME),
		INIT_PARAM("check_timeperiod_object_id", ID),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT),
		INIT_PARAM_BI("perfdata", LONG_STRING, NDO_DATA_PERFDATA),
		INIT_PARAM_BI("current_state", INT8, NDO_DATA_CURRENTSTATE),
		INIT_PARAM_BI("has_been_checked", BOOL, NDO_DATA_HASBEENCHECKED),
		INIT_PARAM_BI("should_be_scheduled", BOOL, NDO_DATA_SHOULDBESCHEDULED),
		INIT_PARAM_BI("current_check_attempt", INT16, NDO_DATA_CURRENTCHECKATTEMPT),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXCHECKATTEMPTS),
		INIT_PARAM_BI("last_check", FROM_UNIXTIME, NDO_DATA_LASTHOSTCHECK),
		INIT_PARAM_BI("next_check", FROM_UNIXTIME, NDO_DATA_NEXTHOSTCHECK),
		INIT_PARAM_BI("check_type", INT8, NDO_DATA_CHECKTYPE),
		INIT_PARAM_BI("last_state_change", FROM_UNIXTIME, NDO_DATA_LASTSTATECHANGE),
		INIT_PARAM_BI("last_hard_state_change", FROM_UNIXTIME, NDO_DATA_LASTHARDSTATECHANGE),
		INIT_PARAM_BI("last_hard_state", INT8, NDO_DATA_LASTHARDSTATE),
		INIT_PARAM_BI("last_time_up", FROM_UNIXTIME, NDO_DATA_LASTTIMEUP),
		INIT_PARAM_BI("last_time_down", FROM_UNIXTIME, NDO_DATA_LASTTIMEDOWN),
		INIT_PARAM_BI("last_time_unreachable", FROM_UNIXTIME, NDO_DATA_LASTTIMEUNREACHABLE),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("last_notification", FROM_UNIXTIME, NDO_DATA_LASTHOSTNOTIFICATION),
		INIT_PARAM_BI("next_notification", FROM_UNIXTIME, NDO_DATA_NEXTHOSTNOTIFICATION),
		INIT_PARAM_BI("no_more_notifications", BOOL, NDO_DATA_NOMORENOTIFICATIONS),
		INIT_PARAM_BI("notifications_enabled", BOOL, NDO_DATA_NOTIFICATIONSENABLED),
		INIT_PARAM_BI("problem_has_been_acknowledged", BOOL, NDO_DATA_PROBLEMHASBEENACKNOWLEDGED),
		INIT_PARAM_BI("acknowledgement_type", INT8, NDO_DATA_ACKNOWLEDGEMENTTYPE),
		INIT_PARAM_BI("current_notification_number", INT16, NDO_DATA_CURRENTNOTIFICATIONNUMBER),
		INIT_PARAM_BI("passive_checks_enabled", BOOL, NDO_DATA_PASSIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("active_checks_enabled", BOOL, NDO_DATA_ACTIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("event_handler_enabled", BOOL, NDO_DATA_EVENTHANDLERENABLED),
		INIT_PARAM_BI("flap_detection_enabled", BOOL, NDO_DATA_FLAPDETECTIONENABLED),
		INIT_PARAM_BI("is_flapping", BOOL, NDO_DATA_ISFLAPPING),
		INIT_PARAM_BI("percent_state_change", DOUBLE, NDO_DATA_PERCENTSTATECHANGE),
		INIT_PARAM_BI("latency", DOUBLE, NDO_DATA_LATENCY),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("scheduled_downtime_depth", INT16, NDO_DATA_SCHEDULEDDOWNTIMEDEPTH),
		INIT_PARAM_BI("failure_prediction_enabled", BOOL, NDO_DATA_FAILUREPREDICTIONENABLED),
		INIT_PARAM_BI("process_performance_data", BOOL, NDO_DATA_PROCESSPERFORMANCEDATA),
		INIT_PARAM_BI("obsess_over_host", BOOL, NDO_DATA_OBSESSOVERHOST),
		INIT_PARAM_BI("modified_host_attributes", UINT32, NDO_DATA_MODIFIEDHOSTATTRIBUTES),
		INIT_PARAM_BI("event_handler", SHORT_STRING, NDO_DATA_EVENTHANDLER),
		INIT_PARAM_BI("check_command", SHORT_STRING,  NDO_DATA_CHECKCOMMAND),
		INIT_PARAM_BI("normal_check_interval", DOUBLE, NDO_DATA_NORMALCHECKINTERVAL),
		INIT_PARAM_BI("retry_check_interval", DOUBLE, NDO_DATA_RETRYCHECKINTERVAL)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_HOSTSTATUS, HOSTSTATUS);
}


static int ndo2db_stmt_init_servicestatus(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("service_object_id", ID),
		INIT_PARAM("status_update_time", FROM_UNIXTIME),
		INIT_PARAM("check_timeperiod_object_id", ID),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT),
		INIT_PARAM_BI("perfdata", LONG_STRING, NDO_DATA_PERFDATA),
		INIT_PARAM_BI("current_state", INT8, NDO_DATA_CURRENTSTATE),
		INIT_PARAM_BI("has_been_checked", BOOL, NDO_DATA_HASBEENCHECKED),
		INIT_PARAM_BI("should_be_scheduled", BOOL, NDO_DATA_SHOULDBESCHEDULED),
		INIT_PARAM_BI("current_check_attempt", INT16, NDO_DATA_CURRENTCHECKATTEMPT),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXCHECKATTEMPTS),
		INIT_PARAM_BI("last_check", FROM_UNIXTIME, NDO_DATA_LASTSERVICECHECK),
		INIT_PARAM_BI("next_check", FROM_UNIXTIME, NDO_DATA_NEXTSERVICECHECK),
		INIT_PARAM_BI("check_type", INT8, NDO_DATA_CHECKTYPE),
		INIT_PARAM_BI("last_state_change", FROM_UNIXTIME, NDO_DATA_LASTSTATECHANGE),
		INIT_PARAM_BI("last_hard_state_change", FROM_UNIXTIME, NDO_DATA_LASTHARDSTATECHANGE),
		INIT_PARAM_BI("last_hard_state", INT8, NDO_DATA_LASTHARDSTATE),
		INIT_PARAM_BI("last_time_ok", FROM_UNIXTIME, NDO_DATA_LASTTIMEOK),
		INIT_PARAM_BI("last_time_warning", FROM_UNIXTIME, NDO_DATA_LASTTIMEWARNING),
		INIT_PARAM_BI("last_time_unknown", FROM_UNIXTIME, NDO_DATA_LASTTIMEUNKNOWN),
		INIT_PARAM_BI("last_time_critical", FROM_UNIXTIME, NDO_DATA_LASTTIMECRITICAL),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("last_notification", FROM_UNIXTIME, NDO_DATA_LASTSERVICENOTIFICATION),
		INIT_PARAM_BI("next_notification", FROM_UNIXTIME, NDO_DATA_NEXTSERVICENOTIFICATION),
		INIT_PARAM_BI("no_more_notifications", BOOL, NDO_DATA_NOMORENOTIFICATIONS),
		INIT_PARAM_BI("notifications_enabled", BOOL, NDO_DATA_NOTIFICATIONSENABLED),
		INIT_PARAM_BI("problem_has_been_acknowledged", BOOL, NDO_DATA_PROBLEMHASBEENACKNOWLEDGED),
		INIT_PARAM_BI("acknowledgement_type", INT8, NDO_DATA_ACKNOWLEDGEMENTTYPE),
		INIT_PARAM_BI("current_notification_number", INT16, NDO_DATA_CURRENTNOTIFICATIONNUMBER),
		INIT_PARAM_BI("passive_checks_enabled", BOOL, NDO_DATA_PASSIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("active_checks_enabled", BOOL, NDO_DATA_ACTIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("event_handler_enabled", BOOL, NDO_DATA_EVENTHANDLERENABLED),
		INIT_PARAM_BI("flap_detection_enabled", BOOL, NDO_DATA_FLAPDETECTIONENABLED),
		INIT_PARAM_BI("is_flapping", BOOL, NDO_DATA_ISFLAPPING),
		INIT_PARAM_BI("percent_state_change", DOUBLE, NDO_DATA_PERCENTSTATECHANGE),
		INIT_PARAM_BI("latency", DOUBLE, NDO_DATA_LATENCY),
		INIT_PARAM_BI("execution_time", DOUBLE, NDO_DATA_EXECUTIONTIME),
		INIT_PARAM_BI("scheduled_downtime_depth", INT16, NDO_DATA_SCHEDULEDDOWNTIMEDEPTH),
		INIT_PARAM_BI("failure_prediction_enabled", BOOL, NDO_DATA_FAILUREPREDICTIONENABLED),
		INIT_PARAM_BI("process_performance_data", BOOL, NDO_DATA_PROCESSPERFORMANCEDATA),
		INIT_PARAM_BI("obsess_over_service", BOOL, NDO_DATA_OBSESSOVERSERVICE),
		INIT_PARAM_BI("modified_service_attributes", UINT32, NDO_DATA_MODIFIEDSERVICEATTRIBUTES),
		INIT_PARAM_BI("event_handler", SHORT_STRING, NDO_DATA_EVENTHANDLER),
		INIT_PARAM_BI("check_command", SHORT_STRING,  NDO_DATA_CHECKCOMMAND),
		INIT_PARAM_BI("normal_check_interval", DOUBLE, NDO_DATA_NORMALCHECKINTERVAL),
		INIT_PARAM_BI("retry_check_interval", DOUBLE, NDO_DATA_RETRYCHECKINTERVAL)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_SERVICESTATUS, SERVICESTATUS);
}


static int ndo2db_stmt_init_contactstatus(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("contact_object_id", ID),
		INIT_PARAM("status_update_time", FROM_UNIXTIME),
		INIT_PARAM_BI("host_notifications_enabled", BOOL, NDO_DATA_HOSTNOTIFICATIONSENABLED),
		INIT_PARAM_BI("service_notifications_enabled", BOOL, NDO_DATA_SERVICENOTIFICATIONSENABLED),
		INIT_PARAM_BI("last_host_notification", FROM_UNIXTIME, NDO_DATA_LASTHOSTNOTIFICATION),
		INIT_PARAM_BI("last_service_notification", FROM_UNIXTIME, NDO_DATA_LASTSERVICENOTIFICATION),
		INIT_PARAM_BI("modified_attributes", INT32, NDO_DATA_MODIFIEDCONTACTATTRIBUTES),
		INIT_PARAM_BI("modified_host_attributes", INT32, NDO_DATA_MODIFIEDHOSTATTRIBUTES),
		INIT_PARAM_BI("modified_service_attributes", INT32, NDO_DATA_MODIFIEDSERVICEATTRIBUTES)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_CONTACTSTATUS, CONTACTSTATUS);
}


static int ndo2db_stmt_init_externalcommand(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM_BI("entry_time", FROM_UNIXTIME, NDO_DATA_ENTRYTIME),
		INIT_PARAM_BI("command_type", INT8, NDO_DATA_COMMANDTYPE),
		INIT_PARAM_BI("command_name", SHORT_STRING, NDO_DATA_COMMANDSTRING),
		INIT_PARAM_BI("command_args", SHORT_STRING, NDO_DATA_COMMANDARGS)
	};
	return PREPARE_INSERT(HANDLE_EXTERNALCOMMAND, EXTERNALCOMMANDS);
}


static int ndo2db_stmt_init_acknowledgement(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("entry_time", FROM_UNIXTIME),
		INIT_PARAM("entry_time_usec", INT32),
		INIT_PARAM("acknowledgement_type", INT8),
		INIT_PARAM("object_id", ID),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("author_name", SHORT_STRING, NDO_DATA_AUTHORNAME),
		INIT_PARAM_BI("comment_data", SHORT_STRING, NDO_DATA_COMMENT),
		INIT_PARAM_BI("is_sticky", BOOL, NDO_DATA_STICKY),
		INIT_PARAM_BI("persistent_comment", BOOL, NDO_DATA_PERSISTENT),
		INIT_PARAM_BI("notify_contacts", BOOL, NDO_DATA_NOTIFYCONTACTS)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_ACKNOWLEDGEMENT, ACKNOWLEDGEMENTS);
}


static int ndo2db_stmt_init_statechange(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("state_time", FROM_UNIXTIME),
		INIT_PARAM("state_time_usec", INT32),
		INIT_PARAM("object_id", ID),
		INIT_PARAM_BI("state", INT8, NDO_DATA_STATE),
		INIT_PARAM_BI("state_type", INT8, NDO_DATA_STATETYPE),
		INIT_PARAM_BI("state_change", INT8, NDO_DATA_STATECHANGE),
		INIT_PARAM_BI("last_state", INT8, NDO_DATA_LASTSTATE),
		INIT_PARAM_BI("last_hard_state", INT8, NDO_DATA_LASTHARDSTATE),
		INIT_PARAM_BI("current_check_attempt", INT16, NDO_DATA_CURRENTCHECKATTEMPT),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXCHECKATTEMPTS),
		INIT_PARAM_BI("output", SHORT_STRING, NDO_DATA_OUTPUT),
		INIT_PARAM_BI("long_output", LONG_STRING, NDO_DATA_LONGOUTPUT)
	};
	return PREPARE_INSERT(HANDLE_STATECHANGE, STATEHISTORY);
}


static int ndo2db_stmt_init_configfile(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind file_params[] = {
		INIT_PARAM("configfile_type", INT16),
		INIT_PARAM_BI("configfile_path", SHORT_STRING, NDO_DATA_CONFIGFILENAME)
	};
	static const struct ndo2db_stmt_bind variable_params[] = {
		INIT_PARAM("configfile_id", ID),
		INIT_PARAM("varname", SHORT_STRING),
		INIT_PARAM("varvalue", SHORT_STRING)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_CONFIGFILE, CONFIGFILES, file_params));

	return PREPARE_INSERT_W_PARAMS(
			SAVE_CONFIGFILEVARIABLE, CONFIGFILEVARIABLES, variable_params);
}


static int ndo2db_stmt_init_runtimevariable(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("varname", SHORT_STRING),
		INIT_PARAM("varvalue", SHORT_STRING)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_RUNTIMEVARIABLE, RUNTIMEVARIABLES);
}


static int ndo2db_stmt_init_host(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind host_params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM("check_command_object_id", ID),
		INIT_PARAM("check_command_args", SHORT_STRING),
		INIT_PARAM("eventhandler_command_object_id", ID),
		INIT_PARAM("eventhandler_command_args", SHORT_STRING),
		INIT_PARAM("check_timeperiod_object_id", ID),
		INIT_PARAM("notification_timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_HOSTALIAS),
		INIT_PARAM_BI("display_name", SHORT_STRING, NDO_DATA_DISPLAYNAME),
		INIT_PARAM_BI("address", SHORT_STRING, NDO_DATA_HOSTADDRESS),
		INIT_PARAM_BI("failure_prediction_options", SHORT_STRING, NDO_DATA_HOSTFAILUREPREDICTIONOPTIONS),
		INIT_PARAM_BI("check_interval", DOUBLE, NDO_DATA_HOSTCHECKINTERVAL),
		INIT_PARAM_BI("retry_interval", DOUBLE, NDO_DATA_HOSTRETRYINTERVAL),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_HOSTMAXCHECKATTEMPTS),
		INIT_PARAM_BI("first_notification_delay", DOUBLE, NDO_DATA_FIRSTNOTIFICATIONDELAY),
		INIT_PARAM_BI("notification_interval", DOUBLE, NDO_DATA_HOSTNOTIFICATIONINTERVAL),
		INIT_PARAM_BI("notify_on_down", BOOL, NDO_DATA_NOTIFYHOSTDOWN),
		INIT_PARAM_BI("notify_on_unreachable", BOOL, NDO_DATA_NOTIFYHOSTUNREACHABLE),
		INIT_PARAM_BI("notify_on_recovery", BOOL, NDO_DATA_NOTIFYHOSTRECOVERY),
		INIT_PARAM_BI("notify_on_flapping", BOOL, NDO_DATA_NOTIFYHOSTFLAPPING),
		INIT_PARAM_BI("notify_on_downtime", BOOL, NDO_DATA_NOTIFYHOSTDOWNTIME),
		INIT_PARAM_BI("stalk_on_up", BOOL, NDO_DATA_STALKHOSTONUP),
		INIT_PARAM_BI("stalk_on_down", BOOL, NDO_DATA_STALKHOSTONDOWN),
		INIT_PARAM_BI("stalk_on_unreachable", BOOL, NDO_DATA_STALKHOSTONUNREACHABLE),
		INIT_PARAM_BI("flap_detection_enabled", BOOL, NDO_DATA_HOSTFLAPDETECTIONENABLED),
		INIT_PARAM_BI("flap_detection_on_up", BOOL, NDO_DATA_FLAPDETECTIONONUP),
		INIT_PARAM_BI("flap_detection_on_down", BOOL, NDO_DATA_FLAPDETECTIONONDOWN),
		INIT_PARAM_BI("flap_detection_on_unreachable", BOOL, NDO_DATA_FLAPDETECTIONONUNREACHABLE),
		INIT_PARAM_BI("low_flap_threshold", DOUBLE, NDO_DATA_LOWHOSTFLAPTHRESHOLD),
		INIT_PARAM_BI("high_flap_threshold", DOUBLE, NDO_DATA_HIGHHOSTFLAPTHRESHOLD),
		INIT_PARAM_BI("process_performance_data", BOOL, NDO_DATA_PROCESSHOSTPERFORMANCEDATA),
		INIT_PARAM_BI("freshness_checks_enabled", BOOL, NDO_DATA_HOSTFRESHNESSCHECKSENABLED),
		INIT_PARAM_BI("freshness_threshold", INT16, NDO_DATA_HOSTFRESHNESSTHRESHOLD),
		INIT_PARAM_BI("passive_checks_enabled", BOOL, NDO_DATA_PASSIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("event_handler_enabled", BOOL, NDO_DATA_HOSTEVENTHANDLERENABLED),
		INIT_PARAM_BI("active_checks_enabled", BOOL, NDO_DATA_ACTIVEHOSTCHECKSENABLED),
		INIT_PARAM_BI("retain_status_information", BOOL, NDO_DATA_RETAINHOSTSTATUSINFORMATION),
		INIT_PARAM_BI("retain_nonstatus_information", BOOL, NDO_DATA_RETAINHOSTNONSTATUSINFORMATION),
		INIT_PARAM_BI("notifications_enabled", BOOL, NDO_DATA_HOSTNOTIFICATIONSENABLED),
		INIT_PARAM_BI("obsess_over_host", BOOL, NDO_DATA_OBSESSOVERHOST),
		INIT_PARAM_BI("failure_prediction_enabled", BOOL, NDO_DATA_HOSTFAILUREPREDICTIONENABLED),
		INIT_PARAM_BI("notes", SHORT_STRING, NDO_DATA_NOTES),
		INIT_PARAM_BI("notes_url", SHORT_STRING, NDO_DATA_NOTESURL),
		INIT_PARAM_BI("action_url", SHORT_STRING, NDO_DATA_ACTIONURL),
		INIT_PARAM_BI("icon_image", SHORT_STRING, NDO_DATA_ICONIMAGE),
		INIT_PARAM_BI("icon_image_alt", SHORT_STRING, NDO_DATA_ICONIMAGEALT),
		INIT_PARAM_BI("vrml_image", SHORT_STRING, NDO_DATA_VRMLIMAGE),
		INIT_PARAM_BI("statusmap_image", SHORT_STRING, NDO_DATA_STATUSMAPIMAGE),
		INIT_PARAM_BI("have_2d_coords", BOOL, NDO_DATA_HAVE2DCOORDS),
		INIT_PARAM_BI("x_2d", INT16, NDO_DATA_X2D),
		INIT_PARAM_BI("y_2d", INT16, NDO_DATA_Y2D),
		INIT_PARAM_BI("have_3d_coords", BOOL, NDO_DATA_HAVE3DCOORDS),
		INIT_PARAM_BI("x_3d", DOUBLE, NDO_DATA_X3D),
		INIT_PARAM_BI("y_3d", DOUBLE, NDO_DATA_Y3D),
		INIT_PARAM_BI("z_3d", DOUBLE, NDO_DATA_Z3D)
#ifdef BUILD_NAGIOS_4X
		,INIT_PARAM_BI("importance", INT32, NDO_DATA_IMPORTANCE)
#endif
	};
	static const struct ndo2db_stmt_bind parent_params[] = {
		INIT_PARAM("host_id", ID),
		INIT_PARAM("parent_host_object_id", ID)
	};
	static const struct ndo2db_stmt_bind contactgroup_params[] = {
		INIT_PARAM("host_id", ID),
		INIT_PARAM("contactgroup_object_id", ID)
	};
	static const struct ndo2db_stmt_bind contact_params[] = {
		INIT_PARAM("host_id", ID),
		INIT_PARAM("contact_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_HOST, HOSTS, host_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTPARENT, HOSTPARENTHOSTS, parent_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTCONTACTGROUP, HOSTCONTACTGROUPS, contactgroup_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTCONTACT, HOSTCONTACTS, contact_params));

	return NDO_OK;
}


static int ndo2db_stmt_init_hostgroup(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind group_params[] = {
		INIT_PARAM("hostgroup_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_HOSTGROUPALIAS)
	};
	static const struct ndo2db_stmt_bind member_params[] = {
		INIT_PARAM("hostgroup_id", ID),
		INIT_PARAM("host_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_HOSTGROUP, HOSTGROUPS, group_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTGROUPMEMBER, HOSTGROUPMEMBERS, member_params));

	return NDO_OK;
}


static int ndo2db_stmt_init_service(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind service_params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM("check_command_object_id", ID),
		INIT_PARAM("check_command_args", SHORT_STRING),
		INIT_PARAM("eventhandler_command_object_id", ID),
		INIT_PARAM("eventhandler_command_args", SHORT_STRING),
		INIT_PARAM("check_timeperiod_object_id", ID),
		INIT_PARAM("notification_timeperiod_object_id", ID),
		INIT_PARAM("service_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("display_name", SHORT_STRING, NDO_DATA_DISPLAYNAME),
		INIT_PARAM_BI("failure_prediction_options", SHORT_STRING, NDO_DATA_SERVICEFAILUREPREDICTIONOPTIONS),
		INIT_PARAM_BI("check_interval", DOUBLE, NDO_DATA_SERVICECHECKINTERVAL),
		INIT_PARAM_BI("retry_interval", DOUBLE, NDO_DATA_SERVICERETRYINTERVAL),
		INIT_PARAM_BI("max_check_attempts", INT16, NDO_DATA_MAXSERVICECHECKATTEMPTS),
		INIT_PARAM_BI("first_notification_delay", DOUBLE, NDO_DATA_FIRSTNOTIFICATIONDELAY),
		INIT_PARAM_BI("notification_interval", DOUBLE, NDO_DATA_SERVICENOTIFICATIONINTERVAL),
		INIT_PARAM_BI("notify_on_warning", BOOL, NDO_DATA_NOTIFYSERVICEWARNING),
		INIT_PARAM_BI("notify_on_unknown", BOOL, NDO_DATA_NOTIFYSERVICEUNKNOWN),
		INIT_PARAM_BI("notify_on_critical", BOOL, NDO_DATA_NOTIFYSERVICECRITICAL),
		INIT_PARAM_BI("notify_on_recovery", BOOL, NDO_DATA_NOTIFYSERVICERECOVERY),
		INIT_PARAM_BI("notify_on_flapping", BOOL, NDO_DATA_NOTIFYSERVICEFLAPPING),
		INIT_PARAM_BI("notify_on_downtime", BOOL, NDO_DATA_NOTIFYSERVICEDOWNTIME),
		INIT_PARAM_BI("stalk_on_ok", BOOL, NDO_DATA_STALKSERVICEONOK),
		INIT_PARAM_BI("stalk_on_warning", BOOL, NDO_DATA_STALKSERVICEONWARNING),
		INIT_PARAM_BI("stalk_on_unknown", BOOL, NDO_DATA_STALKSERVICEONUNKNOWN),
		INIT_PARAM_BI("stalk_on_critical", BOOL, NDO_DATA_STALKSERVICEONCRITICAL),
		INIT_PARAM_BI("is_volatile", BOOL, NDO_DATA_SERVICEISVOLATILE),
		INIT_PARAM_BI("flap_detection_enabled", BOOL, NDO_DATA_SERVICEFLAPDETECTIONENABLED),
		INIT_PARAM_BI("flap_detection_on_ok", BOOL, NDO_DATA_FLAPDETECTIONONOK),
		INIT_PARAM_BI("flap_detection_on_warning", BOOL, NDO_DATA_FLAPDETECTIONONWARNING),
		INIT_PARAM_BI("flap_detection_on_unknown", BOOL, NDO_DATA_FLAPDETECTIONONUNKNOWN),
		INIT_PARAM_BI("flap_detection_on_critical", BOOL, NDO_DATA_FLAPDETECTIONONCRITICAL),
		INIT_PARAM_BI("low_flap_threshold", DOUBLE, NDO_DATA_LOWSERVICEFLAPTHRESHOLD),
		INIT_PARAM_BI("high_flap_threshold", DOUBLE, NDO_DATA_HIGHSERVICEFLAPTHRESHOLD),
		INIT_PARAM_BI("process_performance_data", BOOL, NDO_DATA_PROCESSSERVICEPERFORMANCEDATA),
		INIT_PARAM_BI("freshness_checks_enabled", BOOL, NDO_DATA_SERVICEFRESHNESSCHECKSENABLED),
		INIT_PARAM_BI("freshness_threshold", INT16, NDO_DATA_SERVICEFRESHNESSTHRESHOLD),
		INIT_PARAM_BI("passive_checks_enabled", BOOL, NDO_DATA_PASSIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("event_handler_enabled", BOOL, NDO_DATA_SERVICEEVENTHANDLERENABLED),
		INIT_PARAM_BI("active_checks_enabled", BOOL, NDO_DATA_ACTIVESERVICECHECKSENABLED),
		INIT_PARAM_BI("retain_status_information", BOOL, NDO_DATA_RETAINSERVICESTATUSINFORMATION),
		INIT_PARAM_BI("retain_nonstatus_information", BOOL, NDO_DATA_RETAINSERVICENONSTATUSINFORMATION),
		INIT_PARAM_BI("notifications_enabled", BOOL, NDO_DATA_SERVICENOTIFICATIONSENABLED),
		INIT_PARAM_BI("obsess_over_service", BOOL, NDO_DATA_OBSESSOVERSERVICE),
		INIT_PARAM_BI("failure_prediction_enabled", BOOL, NDO_DATA_SERVICEFAILUREPREDICTIONENABLED),
		INIT_PARAM_BI("notes", SHORT_STRING, NDO_DATA_NOTES),
		INIT_PARAM_BI("notes_url", SHORT_STRING, NDO_DATA_NOTESURL),
		INIT_PARAM_BI("action_url", SHORT_STRING, NDO_DATA_ACTIONURL),
		INIT_PARAM_BI("icon_image", SHORT_STRING, NDO_DATA_ICONIMAGE),
		INIT_PARAM_BI("icon_image_alt", SHORT_STRING, NDO_DATA_ICONIMAGEALT)
#ifdef BUILD_NAGIOS_4X
		,INIT_PARAM_BI("importance", INT32, NDO_DATA_IMPORTANCE)
#endif
	};
#ifdef BUILD_NAGIOS_4X
	static const struct ndo2db_stmt_bind parent_params[] = {
		INIT_PARAM("service_id", ID),
		INIT_PARAM("parent_service_object_id", ID)
	};
#endif
	static const struct ndo2db_stmt_bind contactgroup_params[] = {
		INIT_PARAM("service_id", ID),
		INIT_PARAM("contactgroup_object_id", ID)
	};
	static const struct ndo2db_stmt_bind contact_params[] = {
		INIT_PARAM("service_id", ID),
		INIT_PARAM("contact_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_SERVICE, SERVICES, service_params));
#ifdef BUILD_NAGIOS_4X
	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICEPARENT, SERVICEPARENTSERVICES, parent_params));
#endif
	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICECONTACTGROUP, SERVICECONTACTGROUPS, contactgroup_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICECONTACT, SERVICECONTACTS, contact_params));

	return NDO_OK;
}


static int ndo2db_stmt_init_servicegroup(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind group_params[] = {
		INIT_PARAM("servicegroup_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_SERVICEGROUPALIAS)
	};
	static const struct ndo2db_stmt_bind member_params[] = {
		INIT_PARAM("servicegroup_id", ID),
		INIT_PARAM("service_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_SERVICEGROUP, SERVICEGROUPS, group_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICEGROUPMEMBER, SERVICEGROUPMEMBERS, member_params));

	return NDO_OK;
}


static int ndo2db_stmt_init_hostdependency(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM("dependent_host_object_id", ID),
		INIT_PARAM("timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("dependency_type", INT8, NDO_DATA_DEPENDENCYTYPE),
		INIT_PARAM_BI("inherits_parent", BOOL, NDO_DATA_INHERITSPARENT),
		INIT_PARAM_BI("fail_on_up", BOOL, NDO_DATA_FAILONUP),
		INIT_PARAM_BI("fail_on_down", BOOL, NDO_DATA_FAILONDOWN),
		INIT_PARAM_BI("fail_on_unreachable", BOOL, NDO_DATA_FAILONUNREACHABLE)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_HOSTDEPENDENCY, HOSTDEPENDENCIES);
}


static int ndo2db_stmt_init_servicedependency(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("service_object_id", ID),
		INIT_PARAM("dependent_service_object_id", ID),
		INIT_PARAM("timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("dependency_type", INT8, NDO_DATA_DEPENDENCYTYPE),
		INIT_PARAM_BI("inherits_parent", BOOL, NDO_DATA_INHERITSPARENT),
		INIT_PARAM_BI("fail_on_ok", BOOL, NDO_DATA_FAILONOK),
		INIT_PARAM_BI("fail_on_warning", BOOL, NDO_DATA_FAILONWARNING),
		INIT_PARAM_BI("fail_on_unknown", BOOL, NDO_DATA_FAILONUNKNOWN),
		INIT_PARAM_BI("fail_on_critical", BOOL, NDO_DATA_FAILONCRITICAL)
	};
	return PREPARE_INSERT_UPDATE(HANDLE_SERVICEDEPENDENCY, SERVICEDEPENDENCIES);
}


static int ndo2db_stmt_init_hostescalation(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind escalation_params[] = {
		INIT_PARAM("host_object_id", ID),
		INIT_PARAM("timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("first_notification", INT16, NDO_DATA_FIRSTNOTIFICATION),
		INIT_PARAM_BI("last_notification", INT16, NDO_DATA_LASTNOTIFICATION),
		INIT_PARAM_BI("notification_interval", DOUBLE, NDO_DATA_NOTIFICATIONINTERVAL),
		INIT_PARAM_BI("escalate_on_recovery", BOOL, NDO_DATA_ESCALATEONRECOVERY),
		INIT_PARAM_BI("escalate_on_down", BOOL, NDO_DATA_ESCALATEONDOWN),
		INIT_PARAM_BI("escalate_on_unreachable", BOOL, NDO_DATA_ESCALATEONUNREACHABLE)
	};
	static const struct ndo2db_stmt_bind contactgroup_params[] = {
		INIT_PARAM("hostescalation_id", ID),
		INIT_PARAM("contactgroup_object_id", ID)
	};
	static const struct ndo2db_stmt_bind contact_params[] = {
		INIT_PARAM("hostescalation_id", ID),
		INIT_PARAM("contact_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_HOSTESCALATION, HOSTESCALATIONS, escalation_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTESCALATIONCONTACTGROUP, HOSTESCALATIONCONTACTGROUPS,
			contactgroup_params));

	return PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_HOSTESCALATIONCONTACT, HOSTESCALATIONCONTACTS, contact_params);
}


static int ndo2db_stmt_init_serviceescalation(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind escalation_params[] = {
		INIT_PARAM("service_object_id", ID),
		INIT_PARAM("timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("first_notification", INT16, NDO_DATA_FIRSTNOTIFICATION),
		INIT_PARAM_BI("last_notification", INT16, NDO_DATA_LASTNOTIFICATION),
		INIT_PARAM_BI("notification_interval", DOUBLE, NDO_DATA_NOTIFICATIONINTERVAL),
		INIT_PARAM_BI("escalate_on_recovery", BOOL, NDO_DATA_ESCALATEONRECOVERY),
		INIT_PARAM_BI("escalate_on_warning", BOOL, NDO_DATA_ESCALATEONWARNING),
		INIT_PARAM_BI("escalate_on_unknown", BOOL, NDO_DATA_ESCALATEONUNKNOWN),
		INIT_PARAM_BI("escalate_on_critical", BOOL, NDO_DATA_ESCALATEONCRITICAL)
	};
	static const struct ndo2db_stmt_bind contactgroup_params[] = {
		INIT_PARAM("serviceescalation_id", ID),
		INIT_PARAM("contactgroup_object_id", ID)
	};
	static const struct ndo2db_stmt_bind contact_params[] = {
		INIT_PARAM("serviceescalation_id", ID),
		INIT_PARAM("contact_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			HANDLE_SERVICEESCALATION, SERVICEESCALATIONS, escalation_params));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICEESCALATIONCONTACTGROUP, SERVICEESCALATIONCONTACTGROUPS,
			contactgroup_params));

	return PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_SERVICEESCALATIONCONTACT, SERVICEESCALATIONCONTACTS, contact_params);
}


static int ndo2db_stmt_init_command(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("command_line", SHORT_STRING, NDO_DATA_COMMANDLINE)
	};

	return PREPARE_INSERT_UPDATE(HANDLE_COMMAND, COMMANDS);
}


static int ndo2db_stmt_init_timeperiod(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_TIMEPERIODALIAS)
	};
	static const struct ndo2db_stmt_bind range_params[] = {
		INIT_PARAM("timeperiod_id", ID),
		INIT_PARAM("day", INT16),
		INIT_PARAM("start_sec", UINT32),
		INIT_PARAM("end_sec", UINT32)
	};

	CHK_OK(PREPARE_INSERT_UPDATE(HANDLE_TIMEPERIOD, TIMEPERIODS));

	return PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_TIMEPERIODRANGE, TIMEPERIODTIMERANGES, range_params);
}


static int ndo2db_stmt_init_contact(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("contact_object_id", ID),
		INIT_PARAM("host_timeperiod_object_id", ID),
		INIT_PARAM("service_timeperiod_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_CONTACTALIAS),
		INIT_PARAM_BI("email_address", SHORT_STRING, NDO_DATA_EMAILADDRESS),
		INIT_PARAM_BI("pager_address", SHORT_STRING, NDO_DATA_PAGERADDRESS),
		INIT_PARAM_BI("host_notifications_enabled", BOOL, NDO_DATA_HOSTNOTIFICATIONSENABLED),
		INIT_PARAM_BI("service_notifications_enabled", BOOL, NDO_DATA_SERVICENOTIFICATIONSENABLED),
		INIT_PARAM_BI("can_submit_commands", BOOL, NDO_DATA_CANSUBMITCOMMANDS),
		INIT_PARAM_BI("notify_service_recovery", BOOL, NDO_DATA_NOTIFYSERVICERECOVERY),
		INIT_PARAM_BI("notify_service_warning", BOOL, NDO_DATA_NOTIFYSERVICEWARNING),
		INIT_PARAM_BI("notify_service_unknown", BOOL, NDO_DATA_NOTIFYSERVICEUNKNOWN),
		INIT_PARAM_BI("notify_service_critical", BOOL, NDO_DATA_NOTIFYSERVICECRITICAL),
		INIT_PARAM_BI("notify_service_flapping", BOOL, NDO_DATA_NOTIFYSERVICEFLAPPING),
		INIT_PARAM_BI("notify_service_downtime", BOOL, NDO_DATA_NOTIFYSERVICEDOWNTIME),
		INIT_PARAM_BI("notify_host_recovery", BOOL, NDO_DATA_NOTIFYHOSTRECOVERY),
		INIT_PARAM_BI("notify_host_down", BOOL, NDO_DATA_NOTIFYHOSTDOWN),
		INIT_PARAM_BI("notify_host_unreachable", BOOL, NDO_DATA_NOTIFYHOSTUNREACHABLE),
		INIT_PARAM_BI("notify_host_flapping", BOOL, NDO_DATA_NOTIFYHOSTFLAPPING),
		INIT_PARAM_BI("notify_host_downtime", BOOL, NDO_DATA_NOTIFYHOSTDOWNTIME)
#ifdef BUILD_NAGIOS_4X
		,INIT_PARAM_BI("minimum_importance", INT32, NDO_DATA_MINIMUMIMPORTANCE)
#endif
	};
	static const struct ndo2db_stmt_bind address_params[] = {
		INIT_PARAM("contact_id", ID),
		INIT_PARAM("address_number", INT16),
		INIT_PARAM("address", SHORT_STRING)
	};
	static const struct ndo2db_stmt_bind notif_params[] = {
		INIT_PARAM("contact_id", ID),
		INIT_PARAM("notification_type", INT8),
		INIT_PARAM("command_object_id", ID),
		INIT_PARAM("command_args", SHORT_STRING)
	};

	CHK_OK(PREPARE_INSERT_UPDATE(HANDLE_CONTACT, CONTACTS));

	CHK_OK(PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_CONTACTADDRESS, CONTACTADDRESSES, address_params));

	return PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_CONTACTNOTIFICATIONCOMMAND, CONTACTNOTIFICATIONCOMMANDS,	notif_params);
}


static int ndo2db_stmt_init_contactgroup(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("contactgroup_object_id", ID),
		INIT_PARAM_F("config_type", CURRENT_CONFIG, BIND_BUFFERED_INPUT),
		INIT_PARAM_BI("alias", SHORT_STRING, NDO_DATA_CONTACTGROUPALIAS)
	};
	static const struct ndo2db_stmt_bind mamber_params[] = {
		INIT_PARAM("contactgroup_id", ID),
		INIT_PARAM("contact_object_id", ID)
	};

	CHK_OK(PREPARE_INSERT_UPDATE(HANDLE_CONTACTGROUP, CONTACTGROUPS));

	return PREPARE_INSERT_UPDATE_W_PARAMS(
			SAVE_CONTACTGROUPMEMBER, CONTACTGROUPMEMBERS, mamber_params);
}


static int ndo2db_stmt_init_customvariable(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM("config_type", INT8),
		INIT_PARAM("has_been_modified", BOOL),
		INIT_PARAM("varname", SHORT_STRING),
		INIT_PARAM("varvalue", SHORT_STRING)
	};
	return PREPARE_INSERT_UPDATE(SAVE_CUSTOMVARIABLE, CUSTOMVARIABLES);
}


static int ndo2db_stmt_init_customvariablestatus(ndo2db_idi *idi, ndo_dbuf *dbuf) {

	static const struct ndo2db_stmt_bind params[] = {
		INIT_PARAM("object_id", ID),
		INIT_PARAM("status_update_time", FROM_UNIXTIME),
		INIT_PARAM("has_been_modified", BOOL),
		INIT_PARAM("varname", SHORT_STRING),
		INIT_PARAM("varvalue", SHORT_STRING)
	};
	return PREPARE_INSERT_UPDATE(SAVE_CUSTOMVARIABLESTATUS, CUSTOMVARIABLESTATUS);
}
