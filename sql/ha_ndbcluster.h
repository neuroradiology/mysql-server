/*
   Copyright (c) 2004, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
  This file defines the NDB Cluster handler: the interface between MySQL and
  NDB Cluster
*/

/* The class defining a handle to an NDB Cluster table */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface                       /* gcc class implementation */
#endif

/* DDL names have to fit in system table ndb_schema */
#define NDB_MAX_DDL_NAME_BYTESIZE 63
#define NDB_MAX_DDL_NAME_BYTESIZE_STR "63"

/* Blob tables and events are internal to NDB and must never be accessed */
#define IS_NDB_BLOB_PREFIX(A) is_prefix(A, "NDB$BLOB")

#include <ndbapi/NdbApi.hpp>
#include <ndbapi/ndbapi_limits.h>
#include <kernel/ndb_limits.h>
#include "ndb_conflict.h"

#define NDB_IGNORE_VALUE(x) (void)x

#define NDB_HIDDEN_PRIMARY_KEY_LENGTH 8

class Ndb;             // Forward declaration
class NdbOperation;    // Forward declaration
class NdbTransaction;  // Forward declaration
class NdbRecAttr;      // Forward declaration
class NdbScanOperation; 
class NdbIndexScanOperation; 
class NdbBlob;
class NdbIndexStat;
class NdbEventOperation;
class ha_ndbcluster_cond;
class Ndb_event_data;

typedef enum ndb_index_type {
  UNDEFINED_INDEX = 0,
  PRIMARY_KEY_INDEX = 1,
  PRIMARY_KEY_ORDERED_INDEX = 2,
  UNIQUE_INDEX = 3,
  UNIQUE_ORDERED_INDEX = 4,
  ORDERED_INDEX = 5
} NDB_INDEX_TYPE;

typedef enum ndb_index_status {
  UNDEFINED = 0,
  ACTIVE = 1,
  TO_BE_DROPPED = 2
} NDB_INDEX_STATUS;

typedef struct ndb_index_data {
  NDB_INDEX_TYPE type;
  NDB_INDEX_STATUS status;  
  const NdbDictionary::Index *index;
  const NdbDictionary::Index *unique_index;
  unsigned char *unique_index_attrid_map;
  bool null_in_unique_index;
  /*
    In mysqld, keys and rows are stored differently (using KEY_PART_INFO for
    keys and Field for rows).
    So we need to use different NdbRecord for an index for passing values
    from a key and from a row.
  */
  NdbRecord *ndb_record_key;
  NdbRecord *ndb_unique_record_key;
  NdbRecord *ndb_unique_record_row;
} NDB_INDEX_DATA;

typedef enum ndb_write_op {
  NDB_INSERT = 0,
  NDB_UPDATE = 1,
  NDB_PK_UPDATE = 2
} NDB_WRITE_OP;

class NDB_ALTER_DATA : public Sql_alloc
{
public:
  NDB_ALTER_DATA(NdbDictionary::Dictionary *dict,
		 const NdbDictionary::Table *table) :
    dictionary(dict),
    old_table(table),
    new_table(new NdbDictionary::Table(*table)),
      table_id(table->getObjectId()),
      old_table_version(table->getObjectVersion())
  {}
  ~NDB_ALTER_DATA()
  { delete new_table; }
  NdbDictionary::Dictionary *dictionary;
  const  NdbDictionary::Table *old_table;
  NdbDictionary::Table *new_table;
  Uint32 table_id;
  Uint32 old_table_version;
};

typedef union { const NdbRecAttr *rec; NdbBlob *blob; void *ptr; } NdbValue;

int get_ndb_blobs_value(TABLE* table, NdbValue* value_array,
                        uchar*& buffer, uint& buffer_size,
                        my_ptrdiff_t ptrdiff);

typedef enum {
  NSS_INITIAL= 0,
  NSS_DROPPED,
  NSS_ALTERED 
} NDB_SHARE_STATE;

/*
  Stats that can be retrieved from ndb
*/
struct Ndb_statistics {
  Uint64 row_count;
  Uint64 commit_count;
  ulong row_size;
  Uint64 fragment_memory;
  Uint64 fragment_extent_space; 
  Uint64 fragment_extent_free_space;
};

struct NDB_SHARE {
  NDB_SHARE_STATE state;
  MEM_ROOT mem_root;
  THR_LOCK lock;
  pthread_mutex_t mutex;
  char *key;
  uint key_length;
  char *new_key;
  uint use_count;
  uint commit_count_lock;
  ulonglong commit_count;
  char *db;
  char *table_name;
  Ndb::TupleIdRange tuple_id_range;
  struct Ndb_statistics stat;
  struct Ndb_index_stat *index_stat_list;
  bool util_thread; // if opened by util thread
  uint32 connect_count;
  uint32 flags;
#ifdef HAVE_NDB_BINLOG
  NDB_CONFLICT_FN_SHARE *m_cfn_share;
#endif
  Ndb_event_data *event_data; // Place holder before NdbEventOperation is created
  NdbEventOperation *op;
  char *old_names; // for rename table
  MY_BITMAP *subscriber_bitmap;
  NdbEventOperation *new_op;
};

inline
NDB_SHARE_STATE
get_ndb_share_state(NDB_SHARE *share)
{
  NDB_SHARE_STATE state;
  pthread_mutex_lock(&share->mutex);
  state= share->state;
  pthread_mutex_unlock(&share->mutex);
  return state;
}

inline
void
set_ndb_share_state(NDB_SHARE *share, NDB_SHARE_STATE state)
{
  pthread_mutex_lock(&share->mutex);
  share->state= state;
  pthread_mutex_unlock(&share->mutex);
}

struct Ndb_tuple_id_range_guard {
  Ndb_tuple_id_range_guard(NDB_SHARE* _share) :
    share(_share),
    range(share->tuple_id_range) {
    pthread_mutex_lock(&share->mutex);
  }
  ~Ndb_tuple_id_range_guard() {
    pthread_mutex_unlock(&share->mutex);
  }
  NDB_SHARE* share;
  Ndb::TupleIdRange& range;
};

/* NDB_SHARE.flags */
#define NSF_HIDDEN_PK   1u /* table has hidden primary key */
#define NSF_BLOB_FLAG   2u /* table has blob attributes */
#define NSF_NO_BINLOG   4u /* table should not be binlogged */
#define NSF_BINLOG_FULL 8u /* table should be binlogged with full rows */
#define NSF_BINLOG_USE_UPDATE 16u  /* table update should be binlogged using
                                     update log event */
inline void set_binlog_logging(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_logging"));
  share->flags&= ~NSF_NO_BINLOG;
}
inline void set_binlog_nologging(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_nologging"));
  share->flags|= NSF_NO_BINLOG;
}
inline my_bool get_binlog_nologging(NDB_SHARE *share)
{ return (share->flags & NSF_NO_BINLOG) != 0; }
inline void set_binlog_updated_only(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_updated_only"));
  share->flags&= ~NSF_BINLOG_FULL;
}
inline void set_binlog_full(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_full"));
  share->flags|= NSF_BINLOG_FULL;
}
inline my_bool get_binlog_full(NDB_SHARE *share)
{ return (share->flags & NSF_BINLOG_FULL) != 0; }
inline void set_binlog_use_write(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_use_write"));
  share->flags&= ~NSF_BINLOG_USE_UPDATE;
}
inline void set_binlog_use_update(NDB_SHARE *share)
{
  DBUG_PRINT("info", ("set_binlog_use_update"));
  share->flags|= NSF_BINLOG_USE_UPDATE;
}
inline my_bool get_binlog_use_update(NDB_SHARE *share)
{ return (share->flags & NSF_BINLOG_USE_UPDATE) != 0; }

/*
  Place holder for ha_ndbcluster thread specific data
*/

enum THD_NDB_OPTIONS
{
  TNO_NO_LOG_SCHEMA_OP=  1 << 0,
  /*
    In participating mysqld, do not try to acquire global schema
    lock, as one other mysqld already has the lock.
  */
  TNO_NO_LOCK_SCHEMA_OP= 1 << 1
  /*
    Skip drop of ndb table in delete_table.  Used when calling
    mysql_rm_table_part2 in "show tables", as we do not want to
    remove ndb tables "by mistake".  The table should not exist
    in ndb in the first place.
  */
  ,TNO_NO_NDB_DROP_TABLE=    1 << 2
};

enum THD_NDB_TRANS_OPTIONS
{
  TNTO_INJECTED_APPLY_STATUS= 1 << 0
  ,TNTO_NO_LOGGING=           1 << 1
  ,TNTO_TRANSACTIONS_OFF=     1 << 2
};

struct Ndb_local_table_statistics {
  int no_uncommitted_rows_count;
  ulong last_count;
  ha_rows records;
};

class Thd_ndb 
{
 public:
  Thd_ndb();
  ~Thd_ndb();

  void init_open_tables();

  Ndb_cluster_connection *connection;
  Ndb *ndb;
  /* this */
  class ha_ndbcluster *m_handler;
  ulong count;
  uint lock_count;
  uint start_stmt_count;
  uint save_point_count;
  NdbTransaction *trans;
  bool m_error;
  bool m_slow_path;
  bool m_force_send;

  int m_error_code;
  query_id_t m_query_id; /* query id whn m_error_code was set */
  uint32 options;
  uint32 trans_options;
  List<NDB_SHARE> changed_tables;
  HASH open_tables;
  /*
    This is a memroot used to buffer rows for batched execution.
    It is reset after every execute().
  */
  MEM_ROOT m_batch_mem_root;
  /*
    Estimated pending batched execution bytes, once this is > BATCH_FLUSH_SIZE
    we execute() to flush the rows buffered in m_batch_mem_root.
  */
  uint m_unsent_bytes;
  uint m_batch_size;

  uint m_execute_count;

  uint m_scan_count;
  uint m_pruned_scan_count;
  
  uint m_transaction_no_hint_count[MAX_NDB_NODES];
  uint m_transaction_hint_count[MAX_NDB_NODES];

  NdbTransaction *global_schema_lock_trans;
  uint global_schema_lock_count;
  uint global_schema_lock_error;

  unsigned m_connect_count;
  bool valid_ndb(void);
  bool recycle_ndb(THD* thd);
};

struct st_ndb_status {
  st_ndb_status() { bzero(this, sizeof(struct st_ndb_status)); }
  long cluster_node_id;
  const char * connected_host;
  long connected_port;
  long number_of_replicas;
  long number_of_data_nodes;
  long number_of_ready_data_nodes;
  long connect_count;
  long execute_count;
  long scan_count;
  long pruned_scan_count;
  long transaction_no_hint_count[MAX_NDB_NODES];
  long transaction_hint_count[MAX_NDB_NODES];
  long long api_client_stats[Ndb::NumClientStatistics];
};

int ndbcluster_commit(handlerton *hton, THD *thd, bool all);
class ha_ndbcluster: public handler
{
 public:
  ha_ndbcluster(handlerton *hton, TABLE_SHARE *table);
  ~ha_ndbcluster();

#ifndef NDB_WITHOUT_READ_BEFORE_WRITE_REMOVAL
  void column_bitmaps_signal(uint sig_type);
#endif
  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
  void local_close(THD *thd, bool release_metadata);

  int optimize(THD* thd, HA_CHECK_OPT* check_opt);
  int analyze(THD* thd, HA_CHECK_OPT* check_opt);
  int analyze_index(THD* thd);

  int write_row(uchar *buf);
  int update_row(const uchar *old_data, uchar *new_data);
  int delete_row(const uchar *buf);
  int index_init(uint index, bool sorted);
  int index_end();
  int index_read(uchar *buf, const uchar *key, uint key_len, 
                 enum ha_rkey_function find_flag);
  int index_next(uchar *buf);
  int index_prev(uchar *buf);
  int index_first(uchar *buf);
  int index_last(uchar *buf);
  int index_read_last(uchar * buf, const uchar * key, uint key_len);
  int rnd_init(bool scan);
  int rnd_end();
  int rnd_next(uchar *buf);
  int rnd_pos(uchar *buf, uchar *pos);
  void position(const uchar *record);
  virtual int cmp_ref(const uchar * ref1, const uchar * ref2);
  int read_range_first(const key_range *start_key,
                       const key_range *end_key,
                       bool eq_range, bool sorted);
  int read_range_first_to_buf(const key_range *start_key,
                              const key_range *end_key,
                              bool eq_range, bool sorted,
                              uchar* buf);
  int read_range_next();

#ifndef NDB_WITH_NEW_MRR_INTERFACE
  /**
   * Multi range stuff
   */
  int read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                             KEY_MULTI_RANGE*ranges, uint range_count,
                             bool sorted, HANDLER_BUFFER *buffer);
  int read_multi_range_next(KEY_MULTI_RANGE **found_range_p);
  bool null_value_index_search(KEY_MULTI_RANGE *ranges,
			       KEY_MULTI_RANGE *end_range,
			       HANDLER_BUFFER *buffer);
#endif

  bool get_error_message(int error, String *buf);
  ha_rows records();
  ha_rows estimate_rows_upper_bound()
    { return HA_POS_ERROR; }
  int info(uint);
#if MYSQL_VERSION_ID < 50501
  typedef PARTITION_INFO PARTITION_STATS;
#endif
  void get_dynamic_partition_info(PARTITION_STATS *stat_info, uint part_id);
  uint32 calculate_key_hash_value(Field **field_array);
  bool read_before_write_removal_possible();
  ha_rows read_before_write_removal_rows_written(void) const;
  int extra(enum ha_extra_function operation);
  int extra_opt(enum ha_extra_function operation, ulong cache_size);
  int reset();
  int external_lock(THD *thd, int lock_type);
  void unlock_row();
  int start_stmt(THD *thd, thr_lock_type lock_type);
  void update_create_info(HA_CREATE_INFO *create_info);
  void print_error(int error, myf errflag);
  const char * table_type() const;
  const char ** bas_ext() const;
  ulonglong table_flags(void) const;
  void prepare_for_alter();
  int add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys);
  int prepare_drop_index(TABLE *table_arg, uint *key_num, uint num_of_keys);
  int final_drop_index(TABLE *table_arg);
  void set_part_info(partition_info *part_info, bool early);
  ulong index_flags(uint idx, uint part, bool all_parts) const;
  virtual const key_map *keys_to_use_for_scanning() { return &btree_keys; }
  uint max_supported_record_length() const;
  uint max_supported_keys() const;
  uint max_supported_key_parts() const;
  uint max_supported_key_length() const;
  uint max_supported_key_part_length() const;

  int rename_table(const char *from, const char *to);
  int delete_table(const char *name);
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info);
  int get_default_no_partitions(HA_CREATE_INFO *info);
  bool get_no_parts(const char *name, uint *no_parts);
  void set_auto_partitions(partition_info *part_info);
  virtual bool is_fatal_error(int error, uint flags)
  {
    if (!handler::is_fatal_error(error, flags) ||
        error == HA_ERR_NO_PARTITION_FOUND)
      return FALSE;
    return TRUE;
  }

  THR_LOCK_DATA **store_lock(THD *thd,
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);

  bool low_byte_first() const;

  const char* index_type(uint key_number);

  double scan_time();
  ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key);
  void start_bulk_insert(ha_rows rows);
  int end_bulk_insert();

  bool start_bulk_update();
  int bulk_update_row(const uchar *old_data, uchar *new_data,
                      uint *dup_key_found);
  int exec_bulk_update(uint *dup_key_found);
  void end_bulk_update();
  int ndb_update_row(const uchar *old_data, uchar *new_data,
                     int is_bulk_update);

  static Thd_ndb* seize_thd_ndb(THD*);
  static void release_thd_ndb(Thd_ndb* thd_ndb);
 
static void set_dbname(const char *pathname, char *dbname);
static void set_tabname(const char *pathname, char *tabname);

  /*
    static member function as it needs to access private
    NdbTransaction methods
  */
  static void release_completed_operations(NdbTransaction*);

  /*
    Condition pushdown
  */

 /*
   Push condition down to the table handler.
   SYNOPSIS
     cond_push()
     cond   Condition to be pushed. The condition tree must not be
     modified by the by the caller.
   RETURN
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.
   NOTES
   The pushed conditions form a stack (from which one can remove the
   last pushed condition using cond_pop).
   The table handler filters out rows using (pushed_cond1 AND pushed_cond2 
   AND ... AND pushed_condN)
   or less restrictive condition, depending on handler's capabilities.
   
   handler->reset() call empties the condition stack.
   Calls to rnd_init/rnd_end, index_init/index_end etc do not affect the  
   condition stack.
   The current implementation supports arbitrary AND/OR nested conditions
   with comparisons between columns and constants (including constant
   expressions and function calls) and the following comparison operators:
   =, !=, >, >=, <, <=, like, "not like", "is null", and "is not null". 
   Negated conditions are supported by NOT which generate NAND/NOR groups.
 */ 
  const Item *cond_push(const Item *cond);
 /*
   Pop the top condition from the condition stack of the handler instance.
   SYNOPSIS
     cond_pop()
     Pops the top if condition stack, if stack is not empty
 */
  void cond_pop();

  uint8 table_cache_type();

  /*
   * Internal to ha_ndbcluster, used by C functions
   */
  int ndb_err(NdbTransaction*, bool have_lock= FALSE);

  my_bool register_query_cache_table(THD *thd, char *table_key,
                                     uint key_length,
                                     qc_engine_callback *engine_callback,
                                     ulonglong *engine_data);

  int check_if_supported_alter(TABLE *altered_table,
                               HA_CREATE_INFO *create_info,
                               HA_ALTER_INFO *alter_info,
                               HA_ALTER_FLAGS *alter_flags,
                               uint table_changes);

  int alter_table_phase1(THD *thd,
                         TABLE *altered_table,
                         HA_CREATE_INFO *create_info,
                         HA_ALTER_INFO *alter_info,
                         HA_ALTER_FLAGS *alter_flags);

  int alter_table_phase2(THD *thd,
                         TABLE *altered_table,
                         HA_CREATE_INFO *create_info,
                         HA_ALTER_INFO *alter_info,
                         HA_ALTER_FLAGS *alter_flags);

  int alter_table_phase3(THD *thd, TABLE *table,
                         HA_CREATE_INFO *create_info,
                         HA_ALTER_INFO *alter_info,
                         HA_ALTER_FLAGS *alter_flags);

private:
#ifdef HAVE_NDB_BINLOG
  int prepare_conflict_detection(enum_conflicting_op_type op_type,
                                 const NdbRecord* key_rec,
                                 const uchar* old_data,
                                 const uchar* new_data,
                                 NdbTransaction* trans,
                                 NdbInterpretedCode* code,
                                 NdbOperation::OperationOptions* options,
                                 bool& conflict_handled);
#endif
  void setup_key_ref_for_ndb_record(const NdbRecord **key_rec,
                                    const uchar **key_row,
                                    const uchar *record,
                                    bool use_active_index);
  friend int ndbcluster_drop_database_impl(THD *thd, const char *path);
  friend int ndb_handle_schema_change(THD *thd, 
                                      Ndb *ndb, NdbEventOperation *pOp,
                                      NDB_SHARE *share);

  void check_read_before_write_removal();
  static int drop_table(THD *thd, ha_ndbcluster *h, Ndb *ndb,
                        const char *path,
                        const char *db,
                        const char *table_name);

  int add_index_impl(THD *thd, TABLE *table_arg,
                     KEY *key_info, uint num_of_keys);
  int create_ndb_index(THD *thd, const char *name, KEY *key_info, bool unique);
  int create_ordered_index(THD *thd, const char *name, KEY *key_info);
  int create_unique_index(THD *thd, const char *name, KEY *key_info);
  int create_index(THD *thd, const char *name, KEY *key_info, 
                   NDB_INDEX_TYPE idx_type, uint idx_no);
// Index list management
  int create_indexes(THD *thd, Ndb *ndb, TABLE *tab);
  int open_indexes(THD *thd, Ndb *ndb, TABLE *tab, bool ignore_error);
  void renumber_indexes(Ndb *ndb, TABLE *tab);
  int drop_indexes(Ndb *ndb, TABLE *tab);
  int add_index_handle(THD *thd, NdbDictionary::Dictionary *dict,
                       KEY *key_info, const char *key_name, uint index_no);
  int add_table_ndb_record(NdbDictionary::Dictionary *dict);
  int add_hidden_pk_ndb_record(NdbDictionary::Dictionary *dict);
  int add_index_ndb_record(NdbDictionary::Dictionary *dict,
                           KEY *key_info, uint index_no);
  int check_default_values(const NdbDictionary::Table* ndbtab);
  int get_metadata(THD *thd, const char* path);
  void release_metadata(THD *thd, Ndb *ndb);
  NDB_INDEX_TYPE get_index_type(uint idx_no) const;
  NDB_INDEX_TYPE get_index_type_from_table(uint index_no) const;
  NDB_INDEX_TYPE get_index_type_from_key(uint index_no, KEY *key_info, 
                                         bool primary) const;
  bool has_null_in_unique_index(uint idx_no) const;
  bool check_index_fields_not_null(KEY *key_info);

  int set_up_partition_info(partition_info *part_info,
                            NdbDictionary::Table&) const;
  int set_range_data(const partition_info* part_info,
                     NdbDictionary::Table&) const;
  int set_list_data(const partition_info* part_info,
                    NdbDictionary::Table&) const;
  int ndb_pk_update_row(THD *thd, 
                        const uchar *old_data, uchar *new_data,
                        uint32 old_part_id);
  int pk_read(const uchar *key, uint key_len, uchar *buf, uint32 *part_id);
  int ordered_index_scan(const key_range *start_key,
                         const key_range *end_key,
                         bool sorted, bool descending, uchar* buf,
                         part_id_range *part_spec);
  int unique_index_read(const uchar *key, uint key_len, 
                        uchar *buf);
  int full_table_scan(const KEY* key_info, 
                      const key_range *start_key,
                      const key_range *end_key,
                      uchar *buf);
  int flush_bulk_insert(bool allow_batch= FALSE);
  int ndb_write_row(uchar *record, bool primary_key_update,
                    bool batched_update);

  bool start_bulk_delete();
  int end_bulk_delete();
  int ndb_delete_row(const uchar *record, bool primary_key_update);

  int ndb_optimize_table(THD* thd, uint delay);

  int alter_frm(THD *thd, const char *file, NDB_ALTER_DATA *alter_data);

  bool check_all_operations_for_error(NdbTransaction *trans,
                                      const NdbOperation *first,
                                      const NdbOperation *last,
                                      uint errcode);
  int peek_indexed_rows(const uchar *record, NDB_WRITE_OP write_op);
  int scan_handle_lock_tuple(NdbScanOperation *scanOp, NdbTransaction *trans);
  int fetch_next(NdbScanOperation* op);
  int set_auto_inc(THD *thd, Field *field);
  int set_auto_inc_val(THD *thd, Uint64 value);
  int next_result(uchar *buf); 
  int close_scan();
  void unpack_record(uchar *dst_row, const uchar *src_row);

  void set_dbname(const char *pathname);
  void set_tabname(const char *pathname);

  const NdbDictionary::Column *get_hidden_key_column() {
    return m_table->getColumn(table_share->fields);
  }
  const NdbDictionary::Column *get_partition_id_column() {
    Uint32 index= table_share->fields + (table_share->primary_key == MAX_KEY);
    return m_table->getColumn(index);
  }

  bool add_row_check_if_batch_full_size(Thd_ndb *thd_ndb, uint size);
  bool add_row_check_if_batch_full(Thd_ndb *thd_ndb) {
    return add_row_check_if_batch_full_size(thd_ndb, m_bytes_per_write);
  }
  uchar *get_buffer(Thd_ndb *thd_ndb, uint size);
  uchar *copy_row_to_buffer(Thd_ndb *thd_ndb, const uchar *record);

  int get_blob_values(const NdbOperation *ndb_op, uchar *dst_record,
                      const MY_BITMAP *bitmap);
  int set_blob_values(const NdbOperation *ndb_op, my_ptrdiff_t row_offset,
                      const MY_BITMAP *bitmap, uint *set_count, bool batch);
  friend int g_get_ndb_blobs_value(NdbBlob *ndb_blob, void *arg);
  void release_blobs_buffer();
  Uint32 setup_get_hidden_fields(NdbOperation::GetValueSpec gets[2]);
  void get_hidden_fields_keyop(NdbOperation::OperationOptions *options,
                               NdbOperation::GetValueSpec gets[2]);
  void get_hidden_fields_scan(NdbScanOperation::ScanOptions *options,
                              NdbOperation::GetValueSpec gets[2]);
  void eventSetAnyValue(THD *thd,
                        NdbOperation::OperationOptions *options) const;
  bool check_index_fields_in_write_set(uint keyno);

  const NdbOperation *pk_unique_index_read_key(uint idx, 
                                               const uchar *key, uchar *buf,
                                               NdbOperation::LockMode lm,
                                               Uint32 *ppartition_id);
  int read_multi_range_fetch_next();
  
  int primary_key_cmp(const uchar * old_row, const uchar * new_row);
  void print_results();

  virtual void get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values);
  bool uses_blob_value(const MY_BITMAP *bitmap) const;

  static inline bool isManualBinlogExec(THD *thd);

  char *update_table_comment(const char * comment);

  int write_ndb_file(const char *name);

  int check_ndb_connection(THD* thd);

  void set_rec_per_key();
  int records_update();
  void no_uncommitted_rows_execute_failure();
  void no_uncommitted_rows_update(int);

  /* Ordered index statistics v4 */
  int ndb_index_stat_query(uint inx,
                           const key_range *min_key,
                           const key_range *max_key,
                           NdbIndexStat::Stat& stat,
                           int from);
  int ndb_index_stat_get_rir(uint inx,
                             key_range *min_key,
                             key_range *max_key,
                             ha_rows *rows_out);
  int ndb_index_stat_set_rpk(uint inx);
  int ndb_index_stat_analyze(Ndb *ndb,
                             uint *inx_list,
                             uint inx_count);

  NdbTransaction *start_transaction_part_id(uint32 part_id, int &error);
  inline NdbTransaction *get_transaction_part_id(uint32 part_id, int &error)
  {
    if (m_thd_ndb->trans)
      return m_thd_ndb->trans;
    return start_transaction_part_id(part_id, error);
  }

  NdbTransaction *start_transaction(int &error);
  inline NdbTransaction *get_transaction(int &error)
  {
    if (m_thd_ndb->trans)
      return m_thd_ndb->trans;
    return start_transaction(error);
  }

  NdbTransaction *start_transaction_row(const NdbRecord *ndb_record,
                                        const uchar *record,
                                        int &error);
  NdbTransaction *start_transaction_key(uint index,
                                        const uchar *key_data,
                                        int &error);

  friend int check_completed_operations_pre_commit(Thd_ndb*,
                                                   NdbTransaction*,
                                                   const NdbOperation*,
                                                   uint *ignore_count);
  friend int ndbcluster_commit(handlerton *hton, THD *thd, bool all);
  int start_statement(THD *thd, Thd_ndb *thd_ndb, uint table_count);
  int init_handler_for_statement(THD *thd);

  Thd_ndb *m_thd_ndb;
  NdbScanOperation *m_active_cursor;
  const NdbDictionary::Table *m_table;
  /*
    Normal NdbRecord for accessing rows, with all fields including hidden
    fields (hidden primary key, user-defined partitioning function value).
  */
  NdbRecord *m_ndb_record;
  /* NdbRecord for accessing tuple by hidden Uint64 primary key. */
  NdbRecord *m_ndb_hidden_key_record;

  /* Bitmap used for NdbRecord operation column mask. */
  MY_BITMAP m_bitmap;
  my_bitmap_map m_bitmap_buf[(NDB_MAX_ATTRIBUTES_IN_TABLE +
                              8*sizeof(my_bitmap_map) - 1) /
                             (8*sizeof(my_bitmap_map))]; // Buffer for m_bitmap
  /* Bitmap with bit set for all primary key columns. */
  MY_BITMAP *m_pk_bitmap_p;
  my_bitmap_map m_pk_bitmap_buf[(NDB_MAX_ATTRIBUTES_IN_TABLE +
                                 8*sizeof(my_bitmap_map) - 1) /
                                (8*sizeof(my_bitmap_map))]; // Buffer for m_pk_bitmap
  struct Ndb_local_table_statistics *m_table_info;
  struct Ndb_local_table_statistics m_table_info_instance;
  char m_dbname[FN_HEADLEN];
  //char m_schemaname[FN_HEADLEN];
  char m_tabname[FN_HEADLEN];
  THR_LOCK_DATA m_lock;
  bool m_lock_tuple;
  NDB_SHARE *m_share;
  NDB_INDEX_DATA  m_index[MAX_KEY];
  key_map btree_keys;

  /*
    Pointer to row returned from scan nextResult().
  */
  union
  {
    const char *_m_next_row;
    const uchar *m_next_row;
  };
  /* For read_multi_range scans, the get_range_no() of current row. */
  int m_current_range_no;

  MY_BITMAP **m_key_fields;
  MY_BITMAP m_save_read_set;
  // NdbRecAttr has no reference to blob
  NdbValue m_value[NDB_MAX_ATTRIBUTES_IN_TABLE];
  Uint64 m_ref;
  partition_info *m_part_info;
  uint32 m_part_id;
  bool m_user_defined_partitioning;
  bool m_use_partition_pruning;
  bool m_sorted;
  bool m_use_write;
  bool m_ignore_dup_key;
  bool m_has_unique_index;
  bool m_ignore_no_key;
  bool m_read_before_write_removal_possible;
  bool m_read_before_write_removal_used;
  ha_rows m_rows_updated;
  ha_rows m_rows_deleted;
  ha_rows m_rows_to_insert; // TODO: merge it with handler::estimation_rows_to_insert?
  ha_rows m_rows_inserted;
  ha_rows m_rows_changed;
  bool m_delete_cannot_batch;
  bool m_update_cannot_batch;
  uint m_bytes_per_write;
  bool m_skip_auto_increment;
  bool m_blobs_pending;
  bool m_slow_path;
  bool m_is_bulk_delete;

  /* State for setActiveHook() callback for reading blob data. */
  uint m_blob_counter;
  uint m_blob_expected_count_per_row;
  uchar *m_blob_destination_record;
  Uint64 m_blobs_row_total_size; /* Bytes needed for all blobs in current row */
  
  // memory for blobs in one tuple
  uchar *m_blobs_buffer;
  Uint64 m_blobs_buffer_size;
  uint m_dupkey;
  // set from thread variables at external lock
  ha_rows m_autoincrement_prefetch;

  ha_ndbcluster_cond *m_cond;
  bool m_disable_multi_read;
  const uchar *m_multi_range_result_ptr;
  KEY_MULTI_RANGE *m_multi_ranges;
  /*
    Points 1 past the end of last multi range operation currently being
    executed, to support splitting large multi range reands into manageable
    pieces.
  */
  KEY_MULTI_RANGE *m_multi_range_defined_end;
  NdbIndexScanOperation *m_multi_cursor;
  Ndb *get_ndb(THD *thd);

  int update_stats(THD *thd, bool do_read_stat, bool have_lock= FALSE,
                   uint part_id= ~(uint)0);
  int add_handler_to_open_tables(THD*, Thd_ndb*, ha_ndbcluster* handler);
};

int ndbcluster_discover(THD* thd, const char* dbname, const char* name,
                        const void** frmblob, uint* frmlen);
int ndbcluster_table_exists_in_engine(THD* thd,
                                      const char *db, const char *name);
void ndbcluster_print_error(int error, const NdbOperation *error_op);

static const char ndbcluster_hton_name[]= "ndbcluster";
static const int ndbcluster_hton_name_length=sizeof(ndbcluster_hton_name)-1;
extern int ndbcluster_terminating;

#include "ndb_util_thread.h"
extern Ndb_util_thread ndb_util_thread;

#include "ha_ndb_index_stat.h"
extern Ndb_index_stat_thread ndb_index_stat_thread;
