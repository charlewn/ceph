// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "KineticStore.h"
#include "common/ceph_crypto.h"
#include <kinetic/kinetic.h>
#include <set>
#include <map>
#include <string>
#include "include/memory.h"
#include <errno.h>
using std::string;
#include "common/perf_counters.h"
#include <deque>
#include <mutex>
#include <sys/types.h>
#include <sys/statfs.h>
#include "common/debug.h"

/* FIX ME
   There is no mechanism to wait until connection is available when all the connection is used.
   When connection is fully used and new request comes, request try to use NULL connection and SIGSEV will happen.
   This bug will be fixed by function to check if connection is available.
 */

#define dout_subsys ceph_subsys_kinetic
#undef dout_prefix
#define dout_prefix *_dout << "kinetic "

std::deque<std::unique_ptr<kinetic::ThreadsafeBlockingKineticConnection>> KineticStore::connection_pool;
std::mutex KineticStore::conn_lock;

int KineticStore::init(string option_str)
{
  // init defaults.  caller can override these if they want
  // prior to calling open.
  host = cct->_conf->kinetic_host;
  port = cct->_conf->kinetic_port;
  user_id = cct->_conf->kinetic_user_id;
  hmac_key = cct->_conf->kinetic_hmac_key;
  use_ssl = cct->_conf->kinetic_use_ssl;
  return 0;
}

int KineticStore::_test_init(CephContext *cct)
{
  kinetic::KineticConnectionFactory conn_factory =
    kinetic::NewKineticConnectionFactory();

  kinetic::ConnectionOptions options;
  options.host = cct->_conf->kinetic_host;
  options.port = cct->_conf->kinetic_port;
  options.user_id = cct->_conf->kinetic_user_id;
  options.hmac_key = cct->_conf->kinetic_hmac_key;
  options.use_ssl = cct->_conf->kinetic_use_ssl;

  std::unique_ptr<kinetic::ThreadsafeBlockingKineticConnection> kinetic_conn;
  kinetic::Status status = conn_factory.NewThreadsafeBlockingConnection(
    options, kinetic_conn,
    g_conf->kinetic_num_connections);
  kinetic_conn.reset();
  if (!status.ok())
    derr << __func__ << "Unable to connect to kinetic store " << options.host
         << ":" << options.port << " : " << status.ToString() << dendl;
  return status.ok() ? 0 : -EIO;
}

int KineticStore::do_open(ostream &out, bool create_if_missing)
{
  kinetic::KineticConnectionFactory conn_factory =
    kinetic::NewKineticConnectionFactory();
  kinetic::ConnectionOptions options;
  options.host = host;
  options.port = port;
  options.user_id = user_id;
  options.hmac_key = hmac_key;
  options.use_ssl = use_ssl;
  for(int i = 0; i < g_conf->kinetic_num_connections; i++) {
    kinetic::Status status = conn_factory.NewThreadsafeBlockingConnection(
      options, kinetic_conn,
      g_conf->kinetic_timeout_seconds);
    if (!status.ok()) {
      derr << "Unable to connect to kinetic store " << host << ":" << port
	   << " : " << status.ToString() << dendl;
      return -EINVAL;
    }
    connection_pool.push_back(std::move(kinetic_conn));
  }

  PerfCountersBuilder plb(g_ceph_context, "kinetic", l_kinetic_first, l_kinetic_last);
  plb.add_u64_counter(l_kinetic_gets, "kinetic_get", "Gets");
  plb.add_u64_counter(l_kinetic_txns, "kinetic_transaction", "Transactions");
  logger = plb.create_perf_counters();
  cct->get_perfcounters_collection()->add(logger);
  return 0;
}

KineticStore::KineticStore(CephContext *c) :
  cct(c),
  logger(NULL)
{
  host = c->_conf->kinetic_host;
  port = c->_conf->kinetic_port;
  user_id = c->_conf->kinetic_user_id;
  hmac_key = c->_conf->kinetic_hmac_key;
  use_ssl = c->_conf->kinetic_use_ssl;
}

KineticStore::~KineticStore()
{
  close();
  delete logger;
}

void KineticStore::close()
{
  kinetic_conn.reset();
  if (logger)
    cct->get_perfcounters_collection()->remove(logger);
}

int KineticStore::get_statfs(struct statfs *buf)
{
  unique_ptr<kinetic::ThreadsafeBlockingKineticConnection> getlog_conn;
  vector<com::seagate::kinetic::client::proto::Command_GetLog_Type> log_type{com::seagate::kinetic::client::proto::Command_GetLog_Type_CAPACITIES};
  {
    std::lock_guard<std::mutex> guard(conn_lock);
    getlog_conn = std::move(connection_pool.front());
    connection_pool.pop_front();
  }
  unique_ptr<kinetic::DriveLog> drive_log;
  uint64_t blk_size = cct->_conf->keyvaluestore_default_strip_size;
  kinetic::KineticStatus status = getlog_conn->GetLog(log_type, drive_log);
  if (!status.ok()) {
    derr << "kinetic GetLog error: " << status.message() << dendl;
    {
      std::lock_guard<std::mutex> guard(conn_lock);
      connection_pool.push_back(std::move(getlog_conn));
    }
    return -1;
  }
  buf->f_type = (__SWORD_TYPE)0xdeadbeef;
  buf->f_bsize = (__SWORD_TYPE)blk_size;
  buf->f_blocks = drive_log->capacity.nominal_capacity_in_bytes / blk_size;
  buf->f_bfree = (uint64_t)((float)drive_log->capacity.nominal_capacity_in_bytes * (1.0 - drive_log->capacity.portion_full)) / blk_size;
  buf->f_bavail = (uint64_t)((float)drive_log->capacity.nominal_capacity_in_bytes * (1.0 - drive_log->capacity.portion_full)) / blk_size;
  dout(10) << __func__ << " bsize " << blk_size << " blocks " << buf->f_blocks
	   << " bytes " << drive_log->capacity.nominal_capacity_in_bytes
	   << dendl;
  {
    std::lock_guard<std::mutex> guard(conn_lock);
    connection_pool.push_back(std::move(getlog_conn));
  }
  return 0;
}

int KineticStore::submit_transaction(KeyValueDB::Transaction t)
{
  KineticTransactionImpl * _t =
    static_cast<KineticTransactionImpl *>(t.get());

  dout(20) << __func__ << dendl;

  vector<KineticOp>::iterator it = _t->ops.begin();
  if (g_conf->kinetic_max_batch_ops > 1) {
    // batch!
    int num_of_commit = _t->ops.size() / g_conf->kinetic_max_batch_ops + 1;
    for (int i = 0; it != _t->ops.end() && i < num_of_commit; ++i) {
      utime_t batch_start = ceph_clock_now(NULL);
      kinetic::KineticStatus startstatus(kinetic::StatusCode::OK, "");
      startstatus = _t->kinetic_conn->BatchStart(&(_t->batch_id));
      if (!startstatus.ok()) {
	derr << "kinetic error batch start: "
	     << startstatus.message() << dendl;
	derr << "error number of commit: " << i << dendl;
	derr << "error batch id: " << _t->batch_id << dendl;
	assert(0 == "kinetic batch start error");
	return -1;
      }
      for (int j = 0;
	   it != _t->ops.end() && j < g_conf->kinetic_max_batch_ops;
	   ++it, ++j) {
	kinetic::KineticStatus status(kinetic::StatusCode::OK, "");
	if (it->type == KINETIC_OP_WRITE) {
	  string data(it->data.c_str(), it->data.length());
	  kinetic::KineticRecord record(
	    data, "", "",
	    com::seagate::kinetic::client::proto::Command::INVALID_ALGORITHM);
	  dout(30) << __func__ << " put " << it->key
		   << " (" << data.length() << " bytes)" << dendl;
	  status = _t->kinetic_conn->BatchPutKey(
	    _t->batch_id, it->key, "", kinetic::WriteMode::IGNORE_VERSION,
	    make_shared<const kinetic::KineticRecord>(record));
	} else {
	  assert(it->type == KINETIC_OP_DELETE);
	  dout(30) << __func__ << " delete " << it->key << dendl;
	  status = _t->kinetic_conn->BatchDeleteKey(
	    _t->batch_id, it->key, "",
	    kinetic::WriteMode::IGNORE_VERSION);
	}
	if (!status.ok()) {
	  derr << "kinetic error submitting transaction: "
	       << status.message() << dendl;
	  derr << "error number of commit: " << i << dendl;
	  derr << "error number of batch: " << j << dendl;
	  assert(0 == "kinetic submit error");
	  return -1;
	}
      }
      utime_t commit_start = ceph_clock_now(NULL);
      kinetic::KineticStatus status = _t->kinetic_conn->BatchCommit(_t->batch_id);
      utime_t end = ceph_clock_now(NULL);
      dout(10) << __func__ << " batch commit took " << (end - commit_start)
	       << " out of " << (end - batch_start)
	       << dendl;
      if (!status.ok()) {
	derr << "kinetic error committing transaction: "
	     << status.message() << dendl;
	derr << "kinetic error batch id: " << _t->batch_id << dendl;
	derr << "kinetic error batch operations in queue: " << _t->ops.size()
	     << dendl;
	assert(0 == "kinetic batch commit error");
	return -1;
      }
      _t->batch_id = 0;
    }
  } else {
    // no batch, no transactions.  TOTALLY UNSAFE.
    for ( ; it != _t->ops.end(); ++it) {
      kinetic::KineticStatus status(kinetic::StatusCode::OK, "");
      kinetic::PersistMode mode;
      if (it + 1 == _t->ops.end())
	mode = kinetic::PersistMode::FLUSH;
      else
	mode = kinetic::PersistMode::WRITE_BACK;
      if (it->type == KINETIC_OP_WRITE) {
	string data(it->data.c_str(), it->data.length());
	kinetic::KineticRecord record(
	  data, "", "",
	  com::seagate::kinetic::client::proto::Command::INVALID_ALGORITHM);
	dout(30) << __func__ << " put " << it->key
		 << " (" << data.length() << " bytes)" << dendl;
	status = _t->kinetic_conn->Put(
	  it->key, string(),
	  kinetic::WriteMode::IGNORE_VERSION,
	  record,
	  mode);
      } else {
	assert(it->type == KINETIC_OP_DELETE);
	dout(30) << __func__ << " delete " << it->key << dendl;
	status = _t->kinetic_conn->Delete(
	  it->key, "",
	  kinetic::WriteMode::IGNORE_VERSION,
	  mode);
      }
      if (!status.ok()) {
	derr << "kinetic error submitting transaction: "
	     << status.message() << dendl;
	assert(0 == "kinetic submit error");
	return -1;
      }
    }
    logger->inc(l_kinetic_txns);
  }
  return 0;
}

int KineticStore::submit_transaction_sync(KeyValueDB::Transaction t)
{
  return submit_transaction(t);
}

KineticStore::KineticTransactionImpl::KineticTransactionImpl(KineticStore *_db)
{
  db = _db;
  batch_id = 0;
  {
    std::lock_guard<std::mutex> guard(conn_lock);
    kinetic_conn = std::move(connection_pool.front());
    connection_pool.pop_front();
  }
}

KineticStore::KineticTransactionImpl::~KineticTransactionImpl()
{
  if(batch_id) {
    kinetic_conn->BatchAbort(batch_id);
  }
  std::lock_guard<std::mutex> guard(conn_lock);
  connection_pool.push_back(std::move(kinetic_conn));
}

void KineticStore::KineticTransactionImpl::set(
  const string &prefix,
  const string &k,
  const bufferlist &to_set_bl)
{
  string key = combine_strings(prefix, k);
  dout(30) << __func__ << " key " << key << dendl;
  ops.push_back(KineticOp(KINETIC_OP_WRITE, key, to_set_bl));
}

void KineticStore::KineticTransactionImpl::rmkey(const string &prefix,
					         const string &k)
{
  string key = combine_strings(prefix, k);
  dout(30) << __func__ << " key " << key << dendl;
  ops.push_back(KineticOp(KINETIC_OP_DELETE, key));
}

void KineticStore::KineticTransactionImpl::rmkeys_by_prefix(const string &prefix)
{
  dout(20) << __func__ << " prefix " << prefix << dendl;
  KeyValueDB::Iterator it = db->get_iterator(prefix);
  for (it->seek_to_first();
       it->valid();
       it->next()) {
    string key = combine_strings(prefix, it->key());
    ops.push_back(KineticOp(KINETIC_OP_DELETE, key));
    dout(30) << __func__ << "  key " << key << dendl;
  }
}

int KineticStore::get(
    const string &prefix,
    const std::set<string> &keys,
    std::map<string, bufferlist> *out)
{
  unique_ptr<kinetic::ThreadsafeBlockingKineticConnection> get_conn;
  {
    std::lock_guard<std::mutex> lk(conn_lock);
    while (connection_pool.empty())
      conn_cond.wait(lk);
    get_conn = std::move(connection_pool.front());
    connection_pool.pop_front();
  }
  dout(30) << __func__ << " prefix " << prefix << " keys " << keys << dendl;
  for (std::set<string>::const_iterator i = keys.begin();
       i != keys.end();
       ++i) {
    unique_ptr<kinetic::KineticRecord> record;
    string key = combine_strings(prefix, *i);
    dout(30) << __func__ << "  before get key " << key << dendl;
    kinetic::KineticStatus status = get_conn->Get(key, record);
    if (!status.ok())
      break;
    dout(30) << __func__ << "  get got key: " << key << dendl;
    out->insert(make_pair(*i, to_bufferlist(*record.get())));
  }
  logger->inc(l_kinetic_gets);
  {
    std::lock_guard<std::mutex> guard(conn_lock);
    connection_pool.push_back(std::move(get_conn));
    conn_cond.notify_one();
  }
  return 0;
}

int KineticStore::get(
    const string &prefix,
    const string &key,
    bufferlist *out)
{
  int r = 0;
  unique_ptr<kinetic::ThreadsafeBlockingKineticConnection> get_conn;
  {
    std::unique_lock<std::mutex> lk(conn_lock);
    while (connection_pool.empty())
      conn_cond.wait(lk);
    get_conn = std::move(connection_pool.front());
    connection_pool.pop_front();
  }
  dout(30) << __func__ << " prefix " << prefix << " key " << key << dendl;
  unique_ptr<kinetic::KineticRecord> record;
  string full_key = combine_strings(prefix, key);
  dout(30) << __func__ << "  before get key " << full_key << dendl;
  kinetic::KineticStatus status = get_conn->Get(full_key, record);
  if (!status.ok()) {
#warning fix get() return code
    r = -ENOENT;   // FIXME: error code?
    goto out;
  }
  dout(30) << __func__ << "  got key: " << full_key
	   << " = '" << *record->value() << "'" << dendl;
  *out = to_bufferlist(*record.get());
  r = 0;
 out:
  logger->inc(l_kinetic_gets);
  {
    std::lock_guard<std::mutex> guard(conn_lock);
    connection_pool.push_back(std::move(get_conn));
    conn_cond.notify_one();
  }
  return r;
}

string KineticStore::combine_strings(const string &prefix, const string &value)
{
  string out = prefix;
  out.push_back(1);
  out.append(value);
  return out;
}

bufferlist KineticStore::to_bufferlist(const kinetic::KineticRecord &record)
{
  bufferlist bl;
  bl.append(*(record.value()));
  return bl;
}

int KineticStore::split_key(string &in, string *prefix, string *key)
{
  size_t prefix_len = 0;
  const char* in_data = in.c_str();
  
  // Find separator inside Slice
  char* separator = (char*) memchr((void*)in_data, 1, in.size());
  if (separator == NULL)
     return -EINVAL;
  prefix_len = size_t(separator - in_data);
  if (prefix_len >= in.size())
    return -EINVAL;

  // Fetch prefix and/or key directly from Slice
  if (prefix)
    *prefix = string(in_data, prefix_len);
  if (key)
    *key = string(separator+1, in.size()-prefix_len-1);
  return 0;
}

KineticStore::KineticWholeSpaceIteratorImpl::KineticWholeSpaceIteratorImpl() : kinetic_status(kinetic::StatusCode::OK, "")
{
  std::lock_guard<std::mutex> guard(conn_lock);
  kinetic_conn = std::move(connection_pool.front());
  connection_pool.pop_front();
}

KineticStore::KineticWholeSpaceIteratorImpl::~KineticWholeSpaceIteratorImpl()
{
  std::lock_guard<std::mutex> guard(conn_lock);
  connection_pool.push_back(std::move(kinetic_conn));
}

int KineticStore::KineticWholeSpaceIteratorImpl::seek_to_first(const string &prefix)
{
  dout(30) << __func__ << " prefix " << prefix << dendl;
  kinetic_status = kinetic_conn->GetNext(prefix, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
  }
  else {
    current_key = end_key;
  }
  return 0;
}

int KineticStore::KineticWholeSpaceIteratorImpl::seek_to_last()
{
  dout(30) << __func__ << dendl;
  current_key = end_key;
  kinetic_status = kinetic_conn->GetPrevious(current_key, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
  }
  return 0;
}

int KineticStore::KineticWholeSpaceIteratorImpl::seek_to_last(const string &prefix)
{
  dout(30) << __func__ << " prefix " << prefix << dendl;
  kinetic_status = kinetic_conn->GetPrevious(prefix + "\2", next_key, record);
  if(!kinetic_status.ok()) {
    current_key = end_key;
  }
  else {
    current_key = *next_key;
  }
  return 0;
}

int KineticStore::KineticWholeSpaceIteratorImpl::upper_bound(const string &prefix, const string &after) {
  dout(30) << __func__ << dendl;
  current_key = combine_strings(prefix, after);
  kinetic_status = kinetic_conn->GetNext(current_key, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
  }
  else {
    current_key = end_key;
  }
  return 0;
}

int KineticStore::KineticWholeSpaceIteratorImpl::lower_bound(const string &prefix, const string &to) {
  dout(30) << __func__ << dendl;
  current_key = combine_strings(prefix, to);
  kinetic_status = kinetic_conn->Get(current_key, record);
  if(kinetic_status.ok()) {
    return 0;
  }
  kinetic_status = kinetic_conn->GetNext(current_key, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
    return 0;
  }
  else {
    current_key = end_key;
  }
  return 0;
}

bool KineticStore::KineticWholeSpaceIteratorImpl::valid() {
  bool valid = current_key != end_key;
  dout(30) << __func__ << " = " << valid << dendl;
  return valid;
}

int KineticStore::KineticWholeSpaceIteratorImpl::next() {
  dout(30) << __func__ << dendl;
  kinetic_status = kinetic_conn->GetNext(current_key, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
    return 0;
  }
  current_key = end_key;
  return -1;
}

int KineticStore::KineticWholeSpaceIteratorImpl::prev() {
  dout(30) << __func__ << dendl;
  kinetic_status = kinetic_conn->GetPrevious(current_key, next_key, record);
  if(kinetic_status.ok()) {
    current_key = *next_key;
    return 0;
  }
  current_key = end_key;
  return -1;
}

string KineticStore::KineticWholeSpaceIteratorImpl::key() {
  dout(30) << __func__ << dendl;
  string out_key;
  split_key(current_key, NULL, &out_key);
  return out_key;
}

pair<string,string> KineticStore::KineticWholeSpaceIteratorImpl::raw_key() {
  dout(30) << __func__ << dendl;
  string prefix, key;
  split_key(current_key, &prefix, &key);
  return make_pair(prefix, key);
}

bool KineticStore::KineticWholeSpaceIteratorImpl::raw_key_is_prefixed(const string &prefix) {
  // Look for "prefix\1" right in *keys_iter without making a copy
  const string& key = current_key;
  if ((key.size() > prefix.length()) && (key[prefix.length()] == '\1')) {
    return memcmp(key.c_str(), prefix.c_str(), prefix.length()) == 0;
  } else {
    return false;
  }
}


bufferlist KineticStore::KineticWholeSpaceIteratorImpl::value() {
  dout(30) << __func__ << dendl;
  return to_bufferlist(*record.get());
}

int KineticStore::KineticWholeSpaceIteratorImpl::status() {
  dout(30) << __func__ << dendl;
  return kinetic_status.ok() ? 0 : -1;
}

