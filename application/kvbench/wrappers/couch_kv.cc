#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <queue>

#include "kvs_api.h"
#include "libcouchstore/couch_db.h"
#include "stopwatch.h"
#include "arch.h"
#include "memory.h"
#include "workload.h"

#ifndef SAMSUNG_API
#include <map>
#endif

#define workload_check (0)
#define LATENCY_CHECK  // only for async IO completion latency
//#define MAX_SAMPLES 1000000
static uint32_t max_sample = 1000000;
static int use_udd = 0;
static int kdd_is_polling = 1;
#define GB_SIZE (1024*1024*1024)

int couch_kv_min_key_len = KVS_MIN_KEY_LENGTH;
int couch_kv_max_key_len = KVS_MAX_KEY_LENGTH;

const char* g_container_name = "container1";
const int g_max_iterator_count = 16;

struct _db {
  int id;
  kvs_device_handle dev;
#ifdef SAMSUNG_API
  kvs_container_handle cont_hd;
#else
  kvs_key_space_handle cont_hd;
#endif
  int context_idx;
  IoContext contexts[256];

  std::queue<IoContext*> *iocontexts;
  std::queue<IoContext*> *iodone;
  // only used for async 
  latency_stat *l_read;
  latency_stat *l_write;
  latency_stat *l_delete;
  pthread_mutex_t mutex;

  std::queue<kvs_key*> *kvs_key_pool;
  std::queue<kvs_value*> *kvs_value_pool;
  std::mutex lock_k;
  
  /* For iterator  */
  kvs_iterator_handle iter_handle;
  int has_iter_finish;
  kvs_iterator_list iter_list;

};

static int kv_write_mode = 0;
static int queue_depth = 8;
static int aiothreads_per_device = 2;
static uint64_t coremask = 0;
static int32_t aio_count = 0;
char udd_core_masks[256];
char udd_cq_thread_masks[256];
uint32_t udd_mem_size_mb = 1024;

#define iter_read_size (32 * 1024)

#ifdef SAMSUNG_API
static kvs_iterator_option g_iter_mode;
#else
typedef struct {
  _db *db;
  uint64_t *time;
} kv_bench_data;

static const char *kv_conf_path = "../env_init.conf";
static kvs_option_iterator g_iter_mode;
static std::map<kvs_key*, kv_bench_data*> kvdata_map[3];
static std::mutex kvdata_lock[3];
static std::map<kvs_key_space_handle, kv_bench_data*> kviter_map;
static std::mutex kviter_lock;
#endif

void print_iterator_keyvals(kvs_iterator_list *iter_list){
  uint8_t *it_buffer = (uint8_t *) iter_list->it_list;
  uint32_t num_entries = iter_list->num_entries;

  if(g_iter_mode.iter_type) {
    uint32_t klen  = 16; // Only support fixed key length for iterator
    uint32_t vlen = sizeof(uint32_t);
    uint32_t vlen_value = 0;

    for(int i = 0;i < iter_list->num_entries; i++) {
      fprintf(stdout, "Iterator get %dth key: %s\n", i, it_buffer);
      it_buffer += 16;
      
      uint8_t *addr = (uint8_t *)&vlen_value;
      for (unsigned int i = 0; i < vlen; i++) {
	*(addr + i) = *(it_buffer + i);
      }
    
      it_buffer += vlen;
      it_buffer += vlen_value;
    }
  } else {
    // For fixed key length
    /*
    for(int i = 0; i < iter_list->num_entries; i++)
      fprintf(stdout, "Iterator get key %s\n",  it_buffer + i * 16);
    */

    // for ETA50K24 firmware with various key length
    uint32_t key_size = 0;
    char key[256];

    for(int i = 0;i < iter_list->num_entries; i++) {
      // get key size
      key_size = *((unsigned int*)it_buffer);
      it_buffer += sizeof(unsigned int);
      // print key
      memcpy(key, it_buffer, key_size);
      key[key_size] = 0;
      //fprintf(stdout, "%dth key --> %s\n", i, key);
      it_buffer += key_size;
    }
    
  }
}

void release_kvskeyvalue(Db *db, kvs_key *key, kvs_value *value){
  //memset(key, 0, sizeof(kvs_key));
  std::unique_lock<std::mutex> lock(db->lock_k);
  if(key)
    db->kvs_key_pool->push(key);
  if(value)
    db->kvs_value_pool->push(value);
}

void add_event(Db *db, IoContext *ctx) {
  std::unique_lock<std::mutex> lock(db->lock_k);
  db->iodone->push(ctx);

}

void print_coremask(uint64_t x)
{
  int z;
  char b[65];
  b[64] = '\0';
  for (z = 0; z < 64; z++) {
    b[63-z] = ((x>>z) & 0x1) + '0';
  }
  printf("coremask = %s\n", b);
}

uint64_t coreid_to_mask(char *core_ids)
{
    int i, tmp = 0, found = 0;
    uint64_t coremask = 0;

    for (i = 0; i < strlen(core_ids); i++) {
      if((core_ids[i] == ',' || core_ids[i] == '}') && found == 1) {
	const int coreid = tmp;
	coremask |= (1ULL << coreid);
	tmp = 0;
	found = 0;
      }
      else if (core_ids[i] <= '9' && core_ids[i] >= '0'){
	tmp = tmp * 10 + core_ids[i] - '0';
	found = 1;
      }
    }
    return coremask;
}

void free_doc(Doc *doc)
{
  if (doc->id.buf) free(doc->id.buf); 
  if (doc->data.buf) free(doc->data.buf);
}

#ifdef SAMSUNG_API

void on_io_complete(kvs_callback_context* ioctx) {
  if(ioctx->result != KVS_SUCCESS && ioctx->result != KVS_ERR_KEY_NOT_EXIST) {
    if(ioctx->opcode != IOCB_ASYNC_GET_CMD &&
      ioctx->result != KVS_ERR_BUFFER_SMALL) {
      fprintf(stdout, "io error: op = %d, key = %s, result = %s\n",
        ioctx->opcode, ioctx->key? (char*)ioctx->key->key:0,
        kvs_errstr(ioctx->result));
      exit(1);
    }
  }

  auto owner = (struct _db*)ioctx->private1;
  const auto context_idx = owner->context_idx;
  latency_stat *l_stat;

  if(use_udd) {
    std::unique_lock<std::mutex> lock(owner->lock_k);
    IoContext *ctx = owner->iocontexts->front();
    owner->iocontexts->pop();
    lock.unlock();
    if(ctx == NULL) {
      fprintf(stderr, "Not enough context, outstanding %d\n", owner->iodone->size());
      exit(1);
    }

    switch(ioctx->opcode) {
    case IOCB_ASYNC_PUT_CMD:
      //ctx->op = OP_INSERT;
      ctx->value = ioctx->value->value;
      ctx->key = ioctx->key->key;
      l_stat = owner->l_write;
      release_kvskeyvalue(owner, ioctx->key, ioctx->value); 
      break;
    case IOCB_ASYNC_GET_CMD:
      //ctx->op= OP_GET;
      ctx->value = ioctx->value->value;
      ctx->key = ioctx->key->key;
      l_stat = owner->l_read;
      release_kvskeyvalue(owner, ioctx->key, ioctx->value); 
      break;
    case IOCB_ASYNC_DEL_CMD:
      //ctx->op= OP_DEL;
      ctx->key = ioctx->key->key;
      ctx->value = NULL;
      l_stat = owner->l_delete;
      release_kvskeyvalue(owner, ioctx->key, 0); 
      break;
    case IOCB_ASYNC_ITER_NEXT_CMD:
      //ctx->op = OP_ITER_NEXT;
      print_iterator_keyvals(&owner->iter_list);
      ctx->key = ctx->value = NULL;
      owner->has_iter_finish = 1;
      break;
    }
  
    //ctx->key = ioctx->key->key;
    add_event(owner, ctx);
  } else {  // kdd
    if(kdd_is_polling) {
      switch(ioctx->opcode) {
	/*
      case IOCB_ASYNC_PUT_CMD:
	owner->contexts[context_idx].op= OP_INSERT;
	owner->contexts[context_idx].value = ioctx->value;
	owner->contexts[context_idx].valuelength = ioctx->valuesize;
	owner->contexts[context_idx].freebuf = 0;
	owner->contexts[context_idx].key = ioctx->key;
	owner->contexts[context_idx].keylength = ioctx->keysize;
	l_stat = owner->l_write;
	//fprintf(stdout, "finish write %s - %d \n", ioctx->key, ioctx->valuesize);
	break;
      case IOCB_ASYNC_GET_CMD:
	owner->contexts[context_idx].op= OP_GET;
	owner->contexts[context_idx].value = ioctx->value;
	owner->contexts[context_idx].valuelength = ioctx->valuesize;
	owner->contexts[context_idx].freebuf = 1;
	owner->contexts[context_idx].key = ioctx->key;
	owner->contexts[context_idx].keylength = ioctx->keysize;
	l_stat = owner->l_read;
	break;
      case IOCB_ASYNC_DEL_CMD:
	owner->contexts[context_idx].op= OP_DEL;
	owner->contexts[context_idx].freebuf = 0;
	owner->contexts[context_idx].key = ioctx->key;
	owner->contexts[context_idx].keylength = ioctx->keysize;
	owner->contexts[context_idx].value = NULL;
	owner->contexts[context_idx].valuelength = 0;
	l_stat = owner->l_delete;
	break;
      case IOCB_ASYNC_ITER_NEXT_CMD:
	//print_iterator_keyvals(&owner->iter_list);
	owner->contexts[context_idx].op= OP_ITER_NEXT;
	owner->contexts[context_idx].freebuf = 0;
	owner->contexts[context_idx].key = NULL;
	owner->contexts[context_idx].value = NULL;
	owner->has_iter_finish = 1;
	memset(owner->iter_list.it_list, 0, 32*1024);
	break;
	*/
      }
      owner->context_idx++;

    } else {  // interrupt mode
      //IoContext *ctx = pop_ctx(owner, 0);
      std::unique_lock<std::mutex> lock(owner->lock_k);
      IoContext *ctx = owner->iocontexts->front();
      owner->iocontexts->pop();
      lock.unlock();
      if(ctx == NULL) {
	fprintf(stderr, "Not enough context, outstanding %ld\n", owner->iodone->size());
	exit(1);
      }

      switch(ioctx->opcode) {
      case IOCB_ASYNC_PUT_CMD:
	//ctx->op = OP_INSERT;
	ctx->value = ioctx->value->value;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_write;
	release_kvskeyvalue(owner, ioctx->key, ioctx->value);
	break;
      case IOCB_ASYNC_GET_CMD:
	//ctx->op= OP_GET;
	ctx->value = ioctx->value->value;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_read;
	release_kvskeyvalue(owner, ioctx->key, ioctx->value);
	break;
      case IOCB_ASYNC_DEL_CMD:
	//ctx->op= OP_DEL;
	ctx->value = NULL;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_delete;
	release_kvskeyvalue(owner, ioctx->key, 0);
	break;
      case IOCB_ASYNC_ITER_NEXT_CMD:
	//print_iterator_keyvals(&owner->iter_list);
	//ctx->op= OP_ITER_NEXT;
	ctx->key = NULL;
	ctx->value = NULL;
	std::unique_lock<std::mutex> lock(owner->lock_k);
	owner->has_iter_finish = 1;
	lock.unlock();
	break;
      }

      add_event(owner, ctx);
    } // end of interrupt mode

  }
  
#if defined LATENCY_CHECK
  if(ioctx->private2){
    struct timespec t11;
    unsigned long long start, end;
    uint64_t cur_sample;

    clock_gettime(CLOCK_REALTIME, &t11);
    end = t11.tv_sec * 1000000000L + t11.tv_nsec;
    end /= 1000L;
    start = *((unsigned long long*)ioctx->private2);
    if (l_stat->cursor >= max_sample) {
      l_stat->cursor = l_stat->cursor % max_sample;
      l_stat->nsamples = max_sample;
    } else {
      l_stat->nsamples = l_stat->cursor + 1; 
    }
    cur_sample = l_stat->cursor;
    l_stat->cursor++;
    l_stat->samples[cur_sample] = end - start;
    free(ioctx->private2);
  }
#endif
  
}

int getevents(Db *db, int min, int max, IoContext_t **context)
{
  if(use_udd || kdd_is_polling == 0) {
    int i = 0;
    std::unique_lock<std::mutex> lock(db->lock_k);
    while(!db->iodone->empty() && i < max) {
      context[i] = db->iodone->front();
      db->iodone->pop();
      i++;
    }
    lock.unlock();
    return i;
  } else {  // kdd polling

    db->has_iter_finish = 0;
    //static std::mutex mu;
    //std::unique_lock<std::mutex> lock(mu);
    int ret = kvs_get_ioevents(db->cont_hd, max);

    for (int i= 0; i < ret; i ++) context[i] = &(db->contexts[i]);
    db->context_idx = 0;

    return ret;
  }
  
}

couchstore_error_t couchstore_setup_device(const char *dev_path,
					   char **dev_names,
					   char *config_file,
					   int num_devices, int write_mode,
					   int is_polling)

{  
  kvs_init_options options;
  kvs_init_env_opts(&options);

  int socket, core;
  get_curcpu(&socket, &core);
  uint64_t cpumask = 0;
  //TODO: add core mask
  cpumask |= (1ULL << core);

  fprintf(stderr, "master core = %d, mask = %lx\n ", core, cpumask);
  options.aio.iocoremask = cpumask;

  options.aio.queuedepth = queue_depth;
  //options.aio.is_polling = is_polling;
  options.emul_config_file = config_file;
  kdd_is_polling = is_polling;
  
  if(dev_path[1] == 'd'){ // /dev/kvemul or /dev/nvme kernel driver
    options.memory.use_dpdk = 0;
    use_udd = 0;
  } else {
    options.memory.use_dpdk = 1;
    use_udd = 1;
  }
  
  /* udd options */
  strcpy(options.udd.core_mask_str, udd_core_masks);
  strcpy(options.udd.cq_thread_mask, udd_cq_thread_masks);

  options.udd.mem_size_mb = udd_mem_size_mb;
  options.udd.syncio = write_mode;
  
  kvs_init_env(&options);
  kv_write_mode = write_mode;
  
  fprintf(stdout, "device init done\n");

  return COUCHSTORE_SUCCESS; 
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db_kvs(const char *dev_path,
					  Db **pDb, int id)
{

  int ret;
  Db *ppdb;
  *pDb = (Db*)malloc(sizeof(Db));
  memset(*pDb, 0, sizeof(Db));
  ppdb = *pDb;

  ret = kvs_open_device(dev_path, &ppdb->dev);
  if(ret != KVS_SUCCESS) {
    fprintf(stderr, "Device open failed %s\n", kvs_errstr(ret));
    exit(1);
  }
  
  ppdb->id = id;
  ppdb->context_idx = 0;
  //ppdb->iter_handle = NULL;
  ppdb->has_iter_finish = 1;
  memset(ppdb->contexts, 0, sizeof(ppdb->contexts));
  pthread_mutex_init(&(ppdb->mutex), NULL);

  if(use_udd == 1 || kdd_is_polling == 0) {
    IoContext *context = NULL;
    ppdb->iocontexts = new std::queue<IoContext*>;
    ppdb->iodone = new std::queue<IoContext*>;
    
    ppdb->kvs_key_pool = new std::queue<kvs_key*>;
    ppdb->kvs_value_pool = new std::queue<kvs_value*>;
    
    kvs_key *key = NULL;
    kvs_value *value = NULL;
    for (int i =0; i < 36000; i++) {
      context = (IoContext *)malloc(sizeof(IoContext));
      if(context == NULL){
	fprintf(stderr, "Can not allocate db context\n");
	exit(0);
      }

      ppdb->iocontexts->push(context);
      
      key = (kvs_key*)malloc(sizeof(kvs_key));
      ppdb->kvs_key_pool->push(key);

      value = (kvs_value*)malloc(sizeof(kvs_value));
      ppdb->kvs_value_pool->push(value);
      
    }
  }
    
  /* Container related op */
  uint32_t valid_cnt = 0;
  const uint32_t retrieve_cnt = 2;
  kvs_container_name names[retrieve_cnt];
  char tname[retrieve_cnt][MAX_CONT_PATH_LEN];
  for(uint8_t idx = 0; idx < retrieve_cnt; idx++) {
    names[idx].name_len = MAX_CONT_PATH_LEN;
    names[idx].name = tname[idx];
  }
  kvs_list_containers(ppdb->dev, 1, retrieve_cnt*sizeof(kvs_container_name),
    names, &valid_cnt);
  for (uint8_t idx = 0; idx < valid_cnt; idx++) {
    kvs_delete_container(ppdb->dev, names[idx].name);
  }

  kvs_container_context ctx;
  ctx.option = {KVS_KEY_ORDER_NONE};
  kvs_create_container(ppdb->dev, g_container_name, 4, &ctx);
  kvs_open_container(ppdb->dev, g_container_name, &ppdb->cont_hd);
  
  fprintf(stdout, "device open %s\n", dev_path);

  return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_close_db(Db *db)
{
  if(use_udd || kdd_is_polling == 0) {
    IoContext *tmp;

    std::unique_lock<std::mutex> lock(db->lock_k);

    while(!db->iocontexts->empty()) {
      tmp = db->iocontexts->front();
      db->iocontexts->pop();
      free(tmp);
    }
    delete db->iocontexts;
    delete db->iodone;

    kvs_key *key;
    kvs_value *value;
    while(!db->kvs_key_pool->empty()) {
      key = db->kvs_key_pool->front();
      db->kvs_key_pool->pop();
      free(key);
    }
    delete db->kvs_key_pool;
    
    while(!db->kvs_value_pool->empty()) {
      value = db->kvs_value_pool->front();
      db->kvs_value_pool->pop();
      free(value);
    }
    delete db->kvs_value_pool;
  }

  kvs_close_container(db->cont_hd);
  
  kvs_delete_container(db->dev, g_container_name);
  kvs_close_device(db->dev);
  free(db);
    
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_exit_env(){
  
  kvs_exit_env();

  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t couchstore_iterator_open(Db *db, int iterator_mode) {

  kvs_iterator_context iter_ctx;
  /*
  if(db->iter_handle != NULL) {
    fprintf(stderr, "Device only support one iterator now, please close other iterators\n");
    exit(1);
  }
  */
  iter_ctx.bitmask = 0xffff0000;
  char prefix_str[5] = "0000";
  unsigned int PREFIX_KV = 0;
  for (int i = 0; i < 4; i++){
    PREFIX_KV |= (prefix_str[i] << (3-i)*8);
  }
  iter_ctx.bit_pattern = PREFIX_KV;

  kvs_iterator_option option;
  memset(&option, 0, sizeof(kvs_iterator_option));
  if(iterator_mode == 0) {
    g_iter_mode.iter_type = KVS_ITERATOR_KEY;
    option.iter_type = KVS_ITERATOR_KEY;
  } else {
    g_iter_mode.iter_type = KVS_ITERATOR_KEY_VALUE;
    option.iter_type = KVS_ITERATOR_KEY_VALUE;
  }
  iter_ctx.option = option;
  iter_ctx.private1 = db;
  iter_ctx.private2 = NULL;

  //kvs_close_iterator_all(db->cont_hd);
  int ret = kvs_open_iterator(db->cont_hd, &iter_ctx, &db->iter_handle);
  if(ret && ret != KVS_ERR_ITERATOR_OPEN) {
    fprintf(stdout, "open iter failed with err %s\n", kvs_errstr(ret));
    exit(1);
  }
  db->iter_list.end = 0;
  db->iter_list.num_entries = 0;
  db->iter_list.size = iter_read_size;

  uint8_t *buffer;
  buffer =(uint8_t*) kvs_malloc(iter_read_size, 4096);
  db->iter_list.it_list = (uint8_t*)buffer;
  
  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t couchstore_iterator_close(Db *db) {
  int ret;

  if(db->iter_handle) {
    kvs_iterator_context iter_ctx;
    iter_ctx.private1 = db;
    iter_ctx.private2 = NULL;
    ret = kvs_close_iterator(db->cont_hd, db->iter_handle, &iter_ctx);
    //db->iter_handle = NULL;
    if(db->iter_list.it_list) kvs_free(db->iter_list.it_list);
    fprintf(stdout, "Iterator closed \n");
  }
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_iterator_next(Db *db) {
  kvs_iterator_context iter_ctx;
  kvs_iterator_option option;
  iter_ctx.option = option;//KVS_ITER_DEFAULT;
  iter_ctx.private1 = db;
  iter_ctx.private2 = NULL;

  kvs_iterator_list *iter_list = &db->iter_list;
  iter_list->size = iter_read_size;
  
  //  if(use_udd){
  std::unique_lock<std::mutex> lock(db->lock_k);
  db->has_iter_finish = 0;
  lock.unlock();
    //}

  memset(iter_list->it_list, 0, iter_read_size);
  
  kvs_iterator_next_async(db->cont_hd, db->iter_handle, iter_list, &iter_ctx, on_io_complete);

  return COUCHSTORE_SUCCESS;
}

couchstore_error_t kvs_get_async(Db *db, sized_buf *key, sized_buf *value,
				 couchstore_open_options options)

{
  int ret;
  kvs_retrieve_context ret_ctx;// = { KVS_RETRIEVE_IDEMPOTENT, 0, db, NULL};

  std::unique_lock<std::mutex> lock(db->lock_k);
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  kvs_value *kvsvalue = db->kvs_value_pool->front();
  db->kvs_value_pool->pop();
  lock.unlock();
  
  if(kvskey == NULL || kvsvalue == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}
  
  kvskey->key = key->buf;
  kvskey->length = (kvs_key_t)key->size;

  kvsvalue->value = value->buf;
  kvsvalue->length = (uint32_t)value->size;
  kvsvalue->actual_value_size = kvsvalue->offset = 0;
  
  kvs_retrieve_option option;
  ret_ctx.option = option; 
  ret_ctx.private1 = db;
  ret_ctx.private2 = NULL;
    
#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    ret_ctx.private2 = p1;
    *p1 = nanosec;
  }
#endif
  
  ret = kvs_retrieve_tuple_async(db->cont_hd, kvskey, kvsvalue, &ret_ctx, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: retrieve tuple async failed for %s, err 0x%x\n", (char*)key->buf, ret);
    if (ret_ctx.private2)
      free(ret_ctx.private2);
    exit(1);
  }

  return COUCHSTORE_SUCCESS; 
}

 couchstore_error_t kvs_get_sync(Db *db, sized_buf *key, sized_buf *value,
				 couchstore_open_options options)
{
  int ret;
  kvs_retrieve_option option;
  const kvs_retrieve_context ret_ctx = { option,db, NULL };
  const kvs_key  kvskey = { key->buf, (kvs_key_t)key->size };
  kvs_value kvsvalue = { value->buf, (uint32_t)value->size , 0, 0 /*offset */};
  
  ret = kvs_retrieve_tuple(db->cont_hd, &kvskey, &kvsvalue, &ret_ctx);

  if(ret != KVS_SUCCESS && ret != KVS_ERR_KEY_NOT_EXIST &&
    ret != KVS_ERR_BUFFER_SMALL) {
    fprintf(stderr, "KVBENCH: retrieve tuple sync failed for %s, err 0x%x\n", (char*)key->buf, ret);
    exit(1);
  } 
    
  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t kvs_store_sync(Db *db, Doc* const docs[],
				   unsigned numdocs, couchstore_save_options options)
{
  int i, ret;
  kvs_store_option option;
  option.st_type = KVS_STORE_POST;
  option.kvs_store_compress = false;

  for(i = 0; i < numdocs; i++){
    const kvs_store_context put_ctx = {option, db, NULL};
    const kvs_key kvskey = {docs[i]->id.buf, (kvs_key_t)docs[i]->id.size};
    const kvs_value kvsvalue = { docs[i]->data.buf, (uint32_t)docs[i]->data.size , 0, 0 };

    ret = kvs_store_tuple(db->cont_hd, &kvskey, &kvsvalue, &put_ctx);
    if(ret != KVS_SUCCESS) {
      fprintf(stderr, "KVBENCH: store tuple sync failed %s 0x%x\n", (char*)docs[i]->id.buf, ret);
      exit(1);
    }
  }

  return COUCHSTORE_SUCCESS;
}

couchstore_error_t kvs_store_async(Db *db, Doc* const docs[],
				   unsigned numdocs, couchstore_save_options options)
{

  int ret;

  assert(numdocs == 1);
  kvs_store_option option;
  option.st_type = KVS_STORE_POST;
  option.kvs_store_compress = false;
  kvs_store_context put_ctx; //{KVS_STORE_POST , 0, db, NULL};

  std::unique_lock<std::mutex> lock(db->lock_k);
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  kvs_value *kvsvalue = db->kvs_value_pool->front();
  db->kvs_value_pool->pop();
  lock.unlock();
  if(kvskey == NULL || kvsvalue == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}

  kvskey->key = docs[0]->id.buf;
  kvskey->length=  (kvs_key_t)docs[0]->id.size;

  kvsvalue->value = docs[0]->data.buf;
  kvsvalue->length = (uint32_t)docs[0]->data.size;
  kvsvalue->actual_value_size = kvsvalue->offset = 0;
  
  put_ctx.option = option;
  put_ctx.private1 = db;
  put_ctx.private2 = NULL;
  
#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    put_ctx.private2 = p1;
    *p1 = nanosec;
  }
#endif

  ret = kvs_store_tuple_async(db->cont_hd, kvskey, kvsvalue, &put_ctx, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: store tuple async failed %s 0x%x\n", (char*)docs[0]->id.buf, ret);
    if (put_ctx.private2)
      free(put_ctx.private2);
    exit(1);
  }

  return COUCHSTORE_SUCCESS;
  
}

couchstore_error_t kvs_delete_sync(Db *db,
				   sized_buf *key,
				   couchstore_open_options options)
{
  int ret;
  kvs_delete_option option;
  kvs_delete_context del_ctx = { option, db, 0 };

  const kvs_key  kvskey = { key->buf,(kvs_key_t) key->size };
  ret = kvs_delete_tuple(db->cont_hd, &kvskey, &del_ctx);

  return COUCHSTORE_SUCCESS;
}

 couchstore_error_t kvs_delete_async(Db *db, sized_buf *key,
				    couchstore_open_options options)
{

  int ret;
  kvs_delete_option option;
  kvs_delete_context del_ctx;// = { KVS_DELETE_TUPLE, 0, db, NULL };

  std::unique_lock<std::mutex> lock(db->lock_k);  
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  lock.unlock();
  if(kvskey == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}
  kvskey->key = key->buf;
  kvskey->length= (kvs_key_t)key->size;
  
  del_ctx.option = option;//KVS_DELETE_TUPLE;
  del_ctx.private1 = db;
  del_ctx.private2 = NULL;

#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    del_ctx.private2 = p1;
    *p1 = nanosec;
  }
#endif
  
  ret = kvs_delete_tuple_async(db->cont_hd, kvskey, &del_ctx, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: delete tuple async failed for %s, err 0x%x\n", (char*)key->buf, ret);
    if (del_ctx.private2)
      free(del_ctx.private2);
    exit(1);
  }
  
  return COUCHSTORE_SUCCESS;
 
}

void on_iothread_ready(kvs_thread_t id) {}

#else
int get_map_index(int opcode) {
  int ret = -1;
  switch(opcode) {
    case KVS_CMD_RETRIEVE:
      ret = 0;
      break;
    case KVS_CMD_STORE:
      ret = 1;
      break;
    case KVS_CMD_DELETE:
      ret = 2;
      break;
    default:
      break;
  }
  return ret;
}

void on_io_complete(kvs_postprocess_context* ioctx) {
  if(ioctx->result != KVS_SUCCESS && ioctx->result != KVS_ERR_KEY_NOT_EXIST) {
    if(ioctx->context != KVS_CMD_RETRIEVE &&
      ioctx->result != KVS_ERR_BUFFER_SMALL) {
      fprintf(stdout, "io error: op = %d, key = %s, result = %x\n",
        ioctx->context, ioctx->key? (char*)ioctx->key->key : 0,
        ioctx->result);
      exit(1);
    }
  }

  int id = get_map_index(ioctx->context);
  kv_bench_data* kvdata = NULL;
  if (id >= 0) {
    std::unique_lock<std::mutex> key_lock(kvdata_lock[id]);
    auto db_it = kvdata_map[id].find((kvs_key*)ioctx->key);
    if (db_it == kvdata_map[id].end()) {
      fprintf(stderr, "not found key in map %s\n", (char*)ioctx->key->key);
      return;
    }
    kvdata = (kv_bench_data*)db_it->second;
    kvdata_map[id].erase((kvs_key*)ioctx->key);
    key_lock.unlock();
  } else {
    if (*(ioctx->iter_hd) != 1)
        *ioctx->iter_hd = 1;
    std::unique_lock<std::mutex> key_lock(kviter_lock);
    auto db_it = kviter_map.find((kvs_key_space_handle)(*ioctx->ks_hd));
    if (db_it == kviter_map.end()) {
      fprintf(stderr, "not found keyspace handle in map %lu\n", (uint64_t)*ioctx->iter_hd);
      return;
    }
    kvdata = (kv_bench_data*)db_it->second;
    key_lock.unlock();
  }
  Db* owner = kvdata->db;
  const auto context_idx = owner->context_idx;
  latency_stat *l_stat;

  if(use_udd) {
    std::unique_lock<std::mutex> lock(owner->lock_k);
    IoContext *ctx = owner->iocontexts->front();
    owner->iocontexts->pop();
    lock.unlock();
    if(ctx == NULL) {
      fprintf(stderr, "Not enough context, outstanding %d\n", owner->iodone->size());
      exit(1);
    }

    switch(ioctx->context) {
    case KVS_CMD_STORE:
      ctx->value = ioctx->value->value;
      ctx->key = ioctx->key->key;
      l_stat = owner->l_write;
      release_kvskeyvalue(owner, ioctx->key, ioctx->value);
      break;
    case KVS_CMD_RETRIEVE:
      ctx->value = ioctx->value->value;
      ctx->key = ioctx->key->key;
      l_stat = owner->l_read;
      release_kvskeyvalue(owner, ioctx->key, ioctx->value); 
      break;
    case KVS_CMD_DELETE:
      ctx->key = ioctx->key->key;
      ctx->value = NULL;
      l_stat = owner->l_delete;
      release_kvskeyvalue(owner, ioctx->key, 0); 
      break;
    case KVS_CMD_ITER_NEXT:
      //ctx->op = OP_ITER_NEXT;
      print_iterator_keyvals(&owner->iter_list);
      ctx->key = ctx->value = NULL;
      owner->has_iter_finish = 1;
      break;
    }
  
    //ctx->key = ioctx->key->key;
    add_event(owner, ctx);
  } else {  // kdd
    if(kdd_is_polling) {
      owner->context_idx++;
    } else {  // interrupt mode
      //IoContext *ctx = pop_ctx(owner, 0);
      std::unique_lock<std::mutex> lock(owner->lock_k);
      IoContext *ctx = owner->iocontexts->front();
      owner->iocontexts->pop();
      lock.unlock();
      if(ctx == NULL) {
	fprintf(stderr, "Not enough context, outstanding %ld\n", owner->iodone->size());
	exit(1);
      }

      switch(ioctx->context) {
      case KVS_CMD_STORE:
	ctx->value = ioctx->value->value;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_write;
	release_kvskeyvalue(owner, ioctx->key, ioctx->value);
	break;
      case KVS_CMD_RETRIEVE:
	ctx->value = ioctx->value->value;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_read;
	release_kvskeyvalue(owner, ioctx->key, ioctx->value);
	break;
      case KVS_CMD_DELETE:
	ctx->value = NULL;
	ctx->key = ioctx->key->key;
	l_stat = owner->l_delete;
	release_kvskeyvalue(owner, ioctx->key, 0);
	break;
      case KVS_CMD_ITER_NEXT:
	//print_iterator_keyvals(&owner->iter_list);
	//ctx->op= OP_ITER_NEXT;
	ctx->key = NULL;
	ctx->value = NULL;
	std::unique_lock<std::mutex> lock(owner->lock_k);
	owner->has_iter_finish = 1;
	lock.unlock();
	break;
      }

      add_event(owner, ctx);
    } // end of interrupt mode

  }
  
#if defined LATENCY_CHECK
  if (kvdata->time) {
    struct timespec t11;
    unsigned long long start, end;
    uint64_t cur_sample;

    clock_gettime(CLOCK_REALTIME, &t11);
    end = t11.tv_sec * 1000000000L + t11.tv_nsec;
    end /= 1000L;
    start = *((unsigned long long*)kvdata->time);
    free(kvdata->time);
    if (l_stat->cursor >= max_sample) {
      l_stat->cursor = l_stat->cursor % max_sample;
      l_stat->nsamples = max_sample;
    } else {
      l_stat->nsamples = l_stat->cursor + 1; 
    }
    cur_sample = l_stat->cursor;
    l_stat->cursor++;
    l_stat->samples[cur_sample] = end - start;
  }
#endif
  if (ioctx->context != KVS_CMD_ITER_NEXT) free(kvdata);
}

int getevents(Db *db, int min, int max, IoContext_t **context)
{
    int i = 0;
    std::unique_lock<std::mutex> lock(db->lock_k);
    while(!db->iodone->empty() && i < max) {
      context[i] = db->iodone->front();
      db->iodone->pop();
      i++;
    }
    lock.unlock();
    return i;
}

couchstore_error_t couchstore_setup_device(const char *dev_path,
					   char **dev_names,
					   char *config_file,
					   int num_devices, int write_mode,
					   int is_polling)

{
  int buf_len = 256;
  uint64_t cpumask = 0;

  if (-1 == access(kv_conf_path, F_OK)) {
      remove(kv_conf_path);
  }

  if (creat(kv_conf_path, 0666) < 0) {
    fprintf(stderr, "create kv configure file failed");
    exit(0);
  }

  int socket, core;
  get_curcpu(&socket, &core);
  FILE *fp = fopen(kv_conf_path, "w");
  if (fp == NULL) {
    fprintf(stderr, "open kv configure file failed");
    exit(0);
  }

  //TODO: add core mask
  cpumask |= (1ULL << core);
  fprintf(stdout, "master core = %d, mask = %lx\n ", core, cpumask);
  fprintf(fp, "[aio]\n");
  fprintf(fp, "iocoremask=%llu\n", (long long unsigned)cpumask);
  fprintf(fp, "queue_depth=%u\n\n", queue_depth);
  fprintf(fp, "[emu]\n");
  fprintf(fp, "cfg_file=%s\n\n", config_file);

  kdd_is_polling = 0;
  
  if(dev_path[1] == 'd') { // /dev/kvemul or /dev/nvme kernel driver
    use_udd = 0;
  } else {
    use_udd = 1;
  }

  /* udd options */
  fprintf(fp, "[udd]\n");
  fprintf(fp, "core_mask_str=%s\n", udd_core_masks);
  fprintf(fp, "cq_thread_mask=%s\n", udd_cq_thread_masks);
  fprintf(fp, "mem_size_mb=%u\n", udd_mem_size_mb);
  fprintf(fp, "syncio=%u\n", write_mode);
  
  kv_write_mode = write_mode;

  fclose(fp); 
  fprintf(stdout, "device init done\n");

  return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db_kvs(const char *dev_path,
					  Db **pDb, int id)
{
  int ret;
  Db *ppdb;
  *pDb = (Db*)malloc(sizeof(Db));
  memset(*pDb, 0, sizeof(Db));
  ppdb = *pDb;

  ret = kvs_open_device((char *)dev_path, &ppdb->dev);
  if(ret != KVS_SUCCESS) {
    fprintf(stderr, "Device open failed %d\n", ret);
    exit(1);
  }
  
  ppdb->id = id;
  ppdb->context_idx = 0;
  //ppdb->iter_handle = NULL;
  ppdb->has_iter_finish = 1;
  memset(ppdb->contexts, 0, sizeof(ppdb->contexts));
  pthread_mutex_init(&(ppdb->mutex), NULL);

  if(use_udd == 1 || kdd_is_polling == 0) {
    IoContext *context = NULL;
    ppdb->iocontexts = new std::queue<IoContext*>;
    ppdb->iodone = new std::queue<IoContext*>;
    
    ppdb->kvs_key_pool = new std::queue<kvs_key*>;
    ppdb->kvs_value_pool = new std::queue<kvs_value*>;
    
    kvs_key *key = NULL;
    kvs_value *value = NULL;
    for (int i =0; i < 36000; i++) {
      context = (IoContext *)malloc(sizeof(IoContext));
      if(context == NULL){    
        fprintf(stderr, "Can not allocate db context\n");
        kvs_close_device(ppdb->dev);
        exit(0);
      }

      ppdb->iocontexts->push(context);
      
      key = (kvs_key*)malloc(sizeof(kvs_key));
      ppdb->kvs_key_pool->push(key);

      value = (kvs_value*)malloc(sizeof(kvs_value));
      ppdb->kvs_value_pool->push(value);
      
    }
  }
    
  /* Keyspace related op */
  uint32_t valid_cnt = 0;
  const uint32_t retrieve_cnt = 2;
  kvs_key_space_name names[retrieve_cnt];
  char tname[retrieve_cnt][MAX_CONT_PATH_LEN];
  for(uint8_t idx = 0; idx < retrieve_cnt; idx++) {
    names[idx].name_len = MAX_KEYSPACE_NAME_LEN;
    names[idx].name = tname[idx];
  }

  kvs_list_key_spaces(ppdb->dev, 1, retrieve_cnt*sizeof(kvs_key_space_name),
    names, &valid_cnt);
  
  for (uint8_t idx = 0; idx < valid_cnt; idx++) {
    kvs_delete_key_space(ppdb->dev, &names[idx]);
  }
  
  kvs_key_space_name ks_name;
  ks_name.name = (char *)g_container_name;
  ks_name.name_len = strlen(g_container_name);
  kvs_option_key_space option = {KVS_KEY_ORDER_NONE};
  kvs_create_key_space(ppdb->dev, &ks_name, 0, option);
  kvs_open_key_space(ppdb->dev, (char *)g_container_name, &ppdb->cont_hd);

  fprintf(stdout, "device open %s\n", dev_path);

  return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_close_db(Db *db)
{
  if(use_udd || kdd_is_polling == 0) {
    IoContext *tmp;

    std::unique_lock<std::mutex> lock(db->lock_k);

    while(!db->iocontexts->empty()) {
      tmp = db->iocontexts->front();
      db->iocontexts->pop();
      free(tmp);
    }
    delete db->iocontexts;
    delete db->iodone;

    kvs_key *key;
    kvs_value *value;
    while(!db->kvs_key_pool->empty()) {
      key = db->kvs_key_pool->front();
      db->kvs_key_pool->pop();
      free(key);
    }
    delete db->kvs_key_pool;
    
    while(!db->kvs_value_pool->empty()) {
      value = db->kvs_value_pool->front();
      db->kvs_value_pool->pop();
      free(value);
    }
    delete db->kvs_value_pool;
  }

  kvs_close_key_space(db->cont_hd);

  kvs_key_space_name ks_name;
  ks_name.name = (char *)g_container_name;
  ks_name.name_len = strlen(g_container_name);
  kvs_delete_key_space(db->dev, &ks_name);

  kvs_close_device(db->dev);

  free(db);
    
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_exit_env(){
  
  remove(kv_conf_path);

  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t couchstore_iterator_open(Db *db, int iterator_mode) {
  kvs_key_group_filter iter_ctx;
  kvs_iterator_handle iter_hd;

  iter_ctx.bitmask[0] = 0xff;
  iter_ctx.bitmask[1] = 0xff;
  iter_ctx.bitmask[2] = 0;
  iter_ctx.bitmask[3] = 0;
  
  iter_ctx.bit_pattern[0] = '0';
  iter_ctx.bit_pattern[1] = '0';
  iter_ctx.bit_pattern[2] = '0';
  iter_ctx.bit_pattern[3] = '0';

  kvs_option_iterator option;
  memset(&option, 0, sizeof(kvs_option_iterator));
  if(iterator_mode == 0) {
    g_iter_mode.iter_type = KVS_ITERATOR_KEY;
    option.iter_type = KVS_ITERATOR_KEY;
  } else {
    g_iter_mode.iter_type = KVS_ITERAOR_KEY_VALUE;
    option.iter_type = KVS_ITERAOR_KEY_VALUE;
  }

  //kvs_close_iterator_all(db->cont_hd);
  int ret = kvs_create_iterator(db->cont_hd, &option, &iter_ctx, &db->iter_handle);
  if(ret && ret!= KVS_ERR_ITERATOR_OPEN) {
    fprintf(stdout, "open iter failed with err %d\n", ret);
    exit(1);
  }
  db->iter_list.end = 0;
  db->iter_list.num_entries = 0;
  db->iter_list.size = iter_read_size;

  uint8_t *buffer;
  buffer =(uint8_t*) kvs_malloc(iter_read_size, 4096);
  db->iter_list.it_list = (uint8_t*)buffer;

  kv_bench_data *kvdata = (kv_bench_data *)malloc(sizeof(kv_bench_data));
  memset(kvdata, 0, sizeof(kv_bench_data));
  kvdata->db = db;
  std::unique_lock<std::mutex> k_lock(kviter_lock);
  kviter_map.insert(std::make_pair<kvs_key_space_handle, kv_bench_data*>
    ((kvs_key_space_handle)db->cont_hd, (kv_bench_data*)kvdata));
  k_lock.unlock();
  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t couchstore_iterator_close(Db *db) {
  int ret;

  if(db->iter_handle) {
    ret = kvs_delete_iterator(db->cont_hd, db->iter_handle);
    if(db->iter_list.it_list) kvs_free(db->iter_list.it_list);
    fprintf(stdout, "Iterator closed \n");
  }

  std::unique_lock<std::mutex> k_lock(kviter_lock);
  auto it = kviter_map.find(db->cont_hd);
  if (it == kviter_map.end()) {
    fprintf(stderr, "not found iterator handle %lu ", (uint64_t)db->iter_handle);
  } else {
    kv_bench_data *data = it->second;
    kviter_map.erase(db->cont_hd);
    free(data);
  }
  k_lock.unlock();
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_iterator_next(Db *db) {
  kvs_iterator_list *iter_list = &db->iter_list;
  iter_list->size = iter_read_size;
  
  std::unique_lock<std::mutex> lock(db->lock_k);
  db->has_iter_finish = 0;
  lock.unlock();

  memset(iter_list->it_list, 0, iter_read_size);
  int ret = kvs_iterate_next_async(db->cont_hd, db->iter_handle, iter_list, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: read iterator failed for %d\n", ret);
    exit(0);
  }

  return COUCHSTORE_SUCCESS;
}

couchstore_error_t kvs_get_sync(Db *db, sized_buf *key, sized_buf *value,
				 couchstore_open_options options)
{
  int ret;
  kvs_option_retrieve option = {false};
  const kvs_key kvskey = { key->buf, (uint16_t)key->size };
  kvs_value kvsvalue = { value->buf, (uint32_t)value->size , 0, 0 /*offset */};

  ret = kvs_retrieve_kvp(db->cont_hd, (kvs_key*)&kvskey, &option, &kvsvalue);

  if(ret != KVS_SUCCESS && ret != KVS_ERR_KEY_NOT_EXIST &&
    ret != KVS_ERR_BUFFER_SMALL) {
    fprintf(stderr, "KVBENCH: retrieve tuple sync failed for %s, err 0x%x\n", (char*)key->buf, ret);
    exit(1);
  } 
    
  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t kvs_get_async(Db *db, sized_buf *key, sized_buf *value,
				 couchstore_open_options options)

{
  int ret;

  std::unique_lock<std::mutex> lock(db->lock_k);
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  kvs_value *kvsvalue = db->kvs_value_pool->front();
  db->kvs_value_pool->pop();
  lock.unlock();
  
  if(kvskey == NULL || kvsvalue == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}
  
  kvskey->key = key->buf;
  kvskey->length = (uint16_t)key->size;

  kvsvalue->value = value->buf;
  kvsvalue->length = (uint32_t)value->size;
  kvsvalue->actual_value_size = kvsvalue->offset = 0;
  
  kvs_option_retrieve option = {false};
  kv_bench_data* data = (kv_bench_data*)malloc(sizeof(kv_bench_data));
  memset(data, 0, sizeof(kv_bench_data));
  data->db = db;
#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    *p1 = nanosec;
    data->time = (uint64_t*)p1;
  }
#endif
  std::unique_lock<std::mutex> k_lock(kvdata_lock[0]);
  kvdata_map[0].insert(std::make_pair<kvs_key*, kv_bench_data*>((kvs_key*)kvskey, (kv_bench_data*)data));
  k_lock.unlock();

  ret = kvs_retrieve_kvp_async(db->cont_hd, kvskey, &option, kvsvalue, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: retrieve tuple async failed for %s, err 0x%x\n", (char*)key->buf, ret);
    exit(1);
  }

  return COUCHSTORE_SUCCESS; 
}

couchstore_error_t kvs_store_sync(Db *db, Doc* const docs[],
				   unsigned numdocs, couchstore_save_options options)
{
  int i, ret;
  kvs_option_store option = {KVS_STORE_POST, NULL};
  for(i = 0; i < numdocs; i++){
    const kvs_key kvskey = {docs[i]->id.buf, (uint16_t)docs[i]->id.size};
    const kvs_value kvsvalue = { docs[i]->data.buf, (uint32_t)docs[i]->data.size , 0, 0 };

    ret = kvs_store_kvp(db->cont_hd, (kvs_key *)&kvskey, (kvs_value*)&kvsvalue, &option);
    if(ret != KVS_SUCCESS) {
      fprintf(stderr, "KVBENCH: store tuple sync failed %s 0x%x\n", (char*)docs[i]->id.buf, ret);
      exit(1);
    }
  }

  return COUCHSTORE_SUCCESS;
}

couchstore_error_t kvs_store_async(Db *db, Doc* const docs[],
				   unsigned numdocs, couchstore_save_options options)
{
  int ret;

  assert(numdocs == 1);
  kvs_option_store option = {KVS_STORE_POST, NULL};

  std::unique_lock<std::mutex> lock(db->lock_k);
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  kvs_value *kvsvalue = db->kvs_value_pool->front();
  db->kvs_value_pool->pop();
  lock.unlock();
  if(kvskey == NULL || kvsvalue == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}

  kvskey->key = docs[0]->id.buf;
  kvskey->length = (uint16_t)docs[0]->id.size;

  kvsvalue->value = docs[0]->data.buf;
  kvsvalue->length = (uint32_t)docs[0]->data.size;
  kvsvalue->actual_value_size = kvsvalue->offset = 0;
  kv_bench_data* data = (kv_bench_data*)malloc(sizeof(kv_bench_data));
  memset(data, 0, sizeof(kv_bench_data));
  data->db = db;

#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    *p1 = nanosec;
    data->time = (uint64_t*)p1;
  }
#endif
  std::unique_lock<std::mutex> k_lock(kvdata_lock[1]);
  kvdata_map[1].insert(std::make_pair<kvs_key*, kv_bench_data*>((kvs_key*)kvskey, (kv_bench_data*)data));
  k_lock.unlock();

  ret = kvs_store_kvp_async(db->cont_hd, kvskey, kvsvalue, &option, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: store tuple async failed %s 0x%x\n", (char*)docs[0]->id.buf, ret);
    exit(1);
  }

  return COUCHSTORE_SUCCESS;
  
}

couchstore_error_t kvs_delete_sync(Db *db,
				   sized_buf *key,
				   couchstore_open_options options)
{
  int ret;
  kvs_option_delete option = {false};

  const kvs_key  kvskey = { key->buf, (uint16_t)key->size};
  ret = kvs_delete_kvp(db->cont_hd, (kvs_key*)&kvskey, &option);

  return COUCHSTORE_SUCCESS;
}

couchstore_error_t kvs_delete_async(Db *db, sized_buf *key,
				    couchstore_open_options options)
{

  int ret;
  kvs_option_delete option = {false};

  std::unique_lock<std::mutex> lock(db->lock_k);  
  kvs_key *kvskey = db->kvs_key_pool->front();
  db->kvs_key_pool->pop();
  lock.unlock();
  if(kvskey == NULL) {fprintf(stdout, "No elem in the kvs key/value_pool\n"); exit(0);}
  kvskey->key = key->buf;
  kvskey->length = key->size;
  kv_bench_data* data = (kv_bench_data*)malloc(sizeof(kv_bench_data));
  memset(data, 0, sizeof(kv_bench_data));
  data->db = db;

#if defined LATENCY_CHECK
  if(options == 1){
    struct timespec t11;
    unsigned long long nanosec;
    clock_gettime(CLOCK_REALTIME, &t11);
    unsigned long long *p1 = (unsigned long long *)malloc(sizeof(unsigned long long));
    nanosec = t11.tv_sec * 1000000000L + t11.tv_nsec;
    nanosec /= 1000L;
    *p1 = nanosec;
    data->time = (uint64_t*)p1;
  }
#endif
  std::unique_lock<std::mutex> k_lock(kvdata_lock[2]);
  kvdata_map[2].insert(std::make_pair<kvs_key*, kv_bench_data*>((kvs_key*)kvskey, (kv_bench_data*)data));
  k_lock.unlock();

  ret = kvs_delete_kvp_async(db->cont_hd, kvskey, &option, on_io_complete);
  if (ret) {
    fprintf(stderr, "KVBENCH: delete tuple async failed for %s, err 0x%x\n", (char*)key->buf, ret);
    exit(1);
  }
  
  return COUCHSTORE_SUCCESS;
}

#endif

void pass_lstat_to_db(Db *db, latency_stat *l_read, latency_stat *l_write, latency_stat *l_delete)
{
  db->l_read = l_read;
  db->l_write = l_write;
  db->l_delete = l_delete;
}

int release_context(Db *db, IoContext **contexts, int nr){

  if(use_udd || kdd_is_polling == 0) {
    for (int i =0 ;i < nr ; i++) {
      if (contexts[i]) {
	std::unique_lock<std::mutex> lock(db->lock_k);
	db->iocontexts->push(contexts[i]);
	//push_ctx(db, contexts[i], 0);
      }
    }
  }
  
  return 0;
}

couchstore_error_t couchstore_kvs_set_aio_option(int kvs_queue_depth, char *core_masks, char *cq_thread_masks, uint32_t mem_size_mb)
{
  queue_depth = kvs_queue_depth;
  strcpy(udd_core_masks, core_masks);
  strcpy(udd_cq_thread_masks, cq_thread_masks);
  udd_mem_size_mb = mem_size_mb;
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_set_aiothreads(int kvs_aio_threads)
{
  aiothreads_per_device = kvs_aio_threads;
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_set_coremask(char *kvs_core_ids)
{
  coremask = coreid_to_mask(kvs_core_ids);
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_get_aiocompletion(int32_t *count)
{
  *count = aio_count;
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_reset_aiocompletion(){
  aio_count = 0;
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_set_max_sample(uint32_t sample_num)
{
  max_sample = sample_num;
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_close_device(int32_t dev_id)
{

  // kvs_close_device(dev_id);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_db_info(Db *db, DbInfo* info)
{
    // TBD: need to get container information here, device usage
    return COUCHSTORE_SUCCESS;
}
 
LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_documents(Db *db, Doc* const docs[], DocInfo *infos[],
		   unsigned numdocs, couchstore_save_options options)
{
  if(kv_write_mode == 1)
    return kvs_store_sync(db, docs, numdocs, options);
  else
    return kvs_store_async(db, docs, numdocs, options);
}
 
 
LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_document(Db *db, const Doc *doc, DocInfo *info,
        couchstore_save_options options)
{

  return couchstore_save_documents(db, (Doc**)&doc, (DocInfo**)&info, 1, options);;
}

bool couchstore_iterator_check_status(Db *db) {

  return db->iter_list.end;
}

int couchstore_iterator_get_numentries(Db *db) {

  return db->has_iter_finish ? db->iter_list.num_entries : 0;
}

int couchstore_iterator_has_finish(Db *db) {
  return db->has_iter_finish;
}

couchstore_error_t couchstore_open_document_kv (Db *db,
						sized_buf *key,
						sized_buf *value,
						couchstore_open_options options)
{
   if(kv_write_mode == 1)
     return kvs_get_sync(db, key, value, options);
   else
     return kvs_get_async(db, key, value, options);

   return COUCHSTORE_SUCCESS;

}
 
LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_document(Db *db,
                                            const void *id,
                                            size_t idlen,
                                            Doc **pDoc,
                                            couchstore_open_options options)
{
  
    return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_delete_document_kv(Db *db,
						 sized_buf *key,
						 couchstore_open_options options)
{
  if(kv_write_mode == 1)
    return kvs_delete_sync(db, key, options);
  else
    return kvs_delete_async(db, key, options);
}
 
LIBCOUCHSTORE_API
couchstore_error_t couchstore_delete_document(Db *db, 
					      const void *id,
					      size_t idlen,
					      couchstore_open_options options)
{
  return COUCHSTORE_SUCCESS; 
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_walk_id_tree(Db *db,
                                           const sized_buf* startDocID,
                                           couchstore_docinfos_options options,
                                           couchstore_walk_tree_callback_fn callback,
                                           void *ctx)
{
  // TBD: iterator - phase 2
  return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_kvs_malloc(size_t size_bytes, void **buf){
  *buf = kvs_malloc(size_bytes, 4096);
  return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
void couchstore_free_document(Doc *doc)
{
  if (doc->id.buf) kvs_free(doc->id.buf); 
  if (doc->data.buf) kvs_free(doc->data.buf);
  free(doc);
}


LIBCOUCHSTORE_API
void couchstore_free_docinfo(DocInfo *docinfo)
{

  free(docinfo);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_commit(Db *db)
{
    // do nothing for KVS
    return COUCHSTORE_SUCCESS;
}
