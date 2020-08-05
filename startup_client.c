#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <chronos_client.h>
#include <chronos_cache.h>
#include <chronos_environment.h>
#include <chronos_packets.h>

#include "client_config.h"

#define CHRONOS_CLIENT_SUCCESS  (0)
#define CHRONOS_CLIENT_FAIL     (1)

#define client_msg(_prefix, ...) \
  do {                     \
    char _local_buf_[256];             \
    snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
    fprintf(stderr,"%s: %s: at %s:%d", _prefix,_local_buf_, __FILE__, __LINE__);   \
    fprintf(stderr,"\n");              \
  } while(0)

#define client_info(...) \
  client_msg("INFO", __VA_ARGS__)

#define client_error(...) \
  client_msg("ERROR", __VA_ARGS__)

#define client_warning(...) \
  client_msg("WARN", __VA_ARGS__)

#define client_debug(level,...) \
  do {                                                         \
    if (client_debug_level >= level) {                        \
      char _local_buf_[256];                                   \
      snprintf(_local_buf_, sizeof(_local_buf_), __VA_ARGS__); \
      fprintf(stderr, "DEBUG %s:%d: %s", __FILE__, __LINE__, _local_buf_);    \
      fprintf(stderr, "\n");     \
    } \
  } while(0)

int benchmark_debug_level = CHRONOS_DEBUG_LEVEL_MIN;
int client_debug_level = CHRONOS_DEBUG_LEVEL_MIN + 1;
extern int chronos_debug_level;

typedef struct chronosClientContext_t {
  char    serverAddress[100];
  int     serverPort;
  int     numTransactions;
  double  duration_sec;
  int     percentageViewStockTransactions;
  int     debugLevel;

  /* We can only create a limited number of 
   * client threads. */
  int     numClientsThreads;

  int     minThinkingTime;
  int     maxThinkingTime;
  
  int     (*timeToDieFp)(void);

  CHRONOS_CLIENT_CACHE_H  clientCacheH;
  CHRONOS_ENV_H   chronosEnvH;
} chronosClientContext_t;

typedef struct chronosClientThreadInfo_t {
  pthread_t               thread_id;
  int                     thread_num;
  CHRONOS_CONN_H          connectionH;
  int                     socket_fd;
  int                     numUsers;

  chronosClientContext_t  *contextP;
} chronosClientThreadInfo_t;

static int
waitClientThreads(int num_threads, chronosClientThreadInfo_t *infoP, chronosClientContext_t *contextP);

static int
spawnClientThreads(int num_threads, chronosClientThreadInfo_t **info_ret, chronosClientContext_t *contextP);

static void *
userTransactionThread(void *argP);

static int
pickTransactionType(chronosUserTransaction_t *txn_type_ret, chronosClientThreadInfo_t *infoP);

static void
chronosUsage();

static void 
sigintHandler(int sig);

volatile int cache_ready = 0;
int time_to_die = 0;

int isTimeToDie()
{
  return time_to_die;
}

/*
 * Wait the thinking time
 */
static int
waitThinkTime(int minThinkTimeMS, int maxThinkTimeMS) 
{
  struct timespec waitPeriod;
  int randomWaitTimeMS;

  //randomWaitTimeMS = minThinkTimeMS + (rand() % (1 + maxThinkTimeMS - minThinkTimeMS));
  randomWaitTimeMS = minThinkTimeMS;
   
  waitPeriod.tv_sec = randomWaitTimeMS / 1000;
  waitPeriod.tv_nsec = ((int)randomWaitTimeMS % 1000) * 1000000;

  /* TODO: do I need to check the second argument? */
  nanosleep(&waitPeriod, NULL);
  
  return CHRONOS_CLIENT_SUCCESS;
}

#define xstr(a) str(a)
#define str(a) #a

static void
chronosUsage() 
{
  char usage[1024];
  char template[] =
    "Usage: startup_client OPTIONS\n"
    "Starts up a number of chronos clients\n"
    "\n"
    "OPTIONS:\n"
    "-c|--clients [num]              number of threads (default: %d)\n"
    "-a|--address [address]          server ip address (default: %s)\n"
    "-p|--port [num]                 server port (default: %d)\n"
    "-v|--view-transactions [num]    percentage of user transactions (default: %d%)\n"
    "-d|--debug [num]                debug level\n"
    "-h|--help                       help\n";

  snprintf(usage, sizeof(usage), template,
          CHRONOS_NUM_CLIENT_THREADS, CHRONOS_SERVER_ADDRESS, 
          CHRONOS_SERVER_PORT, CHRONOS_RATE_VIEW_TRANSACTIONS);
  printf("%s\n", usage);
}

static int
initProcessArguments(chronosClientContext_t *contextP)
{
  strncpy(contextP->serverAddress, CHRONOS_SERVER_ADDRESS, sizeof(contextP->serverAddress));
  contextP->serverPort = CHRONOS_SERVER_PORT;
  contextP->percentageViewStockTransactions = CHRONOS_RATE_VIEW_TRANSACTIONS;
  contextP->numClientsThreads = CHRONOS_NUM_CLIENT_THREADS;
  contextP->minThinkingTime = CHRONOS_MIN_THINK_TIME_MS;
  contextP->maxThinkingTime = CHRONOS_MAX_THINK_TIME_MS;
  contextP->timeToDieFp = isTimeToDie;

#ifdef client_debug
  contextP->debugLevel = CHRONOS_DEBUG_LEVEL_MAX;
#endif

  return 0;
}

/*
 * Process the command line arguments
 */
static int
processArguments(int argc, char *argv[], chronosClientContext_t *contextP) 
{

  int c;

  int option_index = 0;

  static struct option long_options[] = {
                   {"clients",                required_argument, 0,  'c' },
                   {"address",                required_argument, 0,  'a' },
                   {"port",                   required_argument, 0,  'p' },
                   {"view-transactions",      required_argument, 0,  'v' },
                   {"debug",                  required_argument, 0,  'd' },
                   {"help",                   no_argument      , 0,  'h' },
                   {0,                        0,                 0,  0 }
               };
  if (contextP == NULL) {
    client_error("Invalid argument");
    goto failXit;
  }

  memset(contextP, 0, sizeof(*contextP));

  initProcessArguments(contextP);

  while (1) {
    option_index = 0;

    c = getopt_long(argc, argv, 
                    "c:a:p:v:d:h",
                    long_options, &option_index);
    if (c == -1) {
      break;
    }

    switch(c) {
      case 0:
        if (long_options[option_index].flag != 0) {
          break;
        }

      case 'c':
        contextP->numClientsThreads = atoi(optarg);
        client_debug(2,"*** Num clients: %d", contextP->numClientsThreads);
        break;

      case 'a':
        snprintf(contextP->serverAddress, sizeof(contextP->serverAddress), "%s", optarg);
        client_debug(2, "*** Server address: %s", contextP->serverAddress);
        break;

      case 'p':
        contextP->serverPort = atoi(optarg);
        client_debug(2, "*** Server port: %d", contextP->serverPort);
        break;

      case 'v':
        contextP->percentageViewStockTransactions = atoi(optarg);
        client_debug(2, "*** %% of ViewStock Transactions: %d", contextP->percentageViewStockTransactions);
        break;

      case 'd':
        contextP->debugLevel = atoi(optarg);
        client_debug(2, "*** Debug Level: %d", contextP->debugLevel);
        break;

      case 'h':
        chronosUsage();
        exit(0);
        break;

      default:
        client_error("Invalid argument");
        goto failXit;
    }
  }

  if (contextP->numClientsThreads <= 0) {
    client_error("number of clients must be > 0");
    goto failXit;
  }

  if (contextP->percentageViewStockTransactions <=0 || contextP->percentageViewStockTransactions > 100) {
    client_error("percentage of view_stock transactions must be between 1 and 100");
    goto failXit;
  }

  if (contextP->serverPort <= 0) {
    client_error("port must be a valid one");
    goto failXit;
  }

  if (contextP->serverAddress[0] == '\0') {
    client_error("address must be a valid one");
    goto failXit;
  }

  return CHRONOS_CLIENT_SUCCESS;

failXit:
  chronosUsage();
  return CHRONOS_CLIENT_FAIL;
}

/*
 * This function waits till client threads join
 */
static int
waitClientThreads(int num_threads, chronosClientThreadInfo_t *infoP, chronosClientContext_t *contextP) 
{
  int i;
  int rc;
  int *thread_rc = NULL;
  pthread_t tid = pthread_self();

  client_debug(5, "%lu: Waiting for client threads termination", tid);
  for (i=0; i<num_threads; i++) {
    rc = pthread_join(infoP[i].thread_id, (void **)&thread_rc);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Failed while joining thread %d", infoP[i].thread_num);
    }
    client_debug(4,"%lu: Thread %d finished", tid, infoP[i].thread_num);
  }
  client_debug(5,"%lu: Done with client threads termination", tid);

  return CHRONOS_CLIENT_SUCCESS;
}

/*
 * This function spawns a given number of client threads
 */
static int
spawnClientThreads(int num_threads, chronosClientThreadInfo_t **info_ret, chronosClientContext_t *contextP) 
{

  pthread_attr_t attr;
  chronosClientThreadInfo_t *infoP = NULL;
  int i;
  int rc;
  const int stack_size = 0x100000; // 1 MB
  pthread_t tid = pthread_self();

  if (num_threads < 1 || info_ret == NULL || contextP == NULL) {
    client_error("invalid arguments");
    goto failXit;
  }

  infoP = calloc(num_threads, sizeof(chronosClientThreadInfo_t));
  if (infoP == NULL) {
    client_error("failed to allocate space for threads info");
    goto failXit;
  }

  rc = pthread_attr_init(&attr);
  if (rc != 0) {
    client_error("failed to init thread attributes");
    goto failXit;
  }
  
  rc = pthread_attr_setstacksize(&attr, stack_size);
  if (rc != 0) {
    client_error("failed to set stack size");
    goto failXit;
  }

  /* Spawn N client threads*/
  client_debug(5,"%lu: Spawning %d client threads", tid, num_threads);
  for (i=0; i<num_threads; i++) {
    infoP[i].thread_num = i+ 1;
    infoP[i].contextP = contextP;

    rc = pthread_create(&infoP[i].thread_id,
                        &attr,
                        &userTransactionThread,
                        &infoP[i]);
    if (rc != 0) {
      client_error("failed to spawn thread");
      goto failXit;
    }

    client_debug(4,"%lu: Spawed thread: %d", tid, infoP[i].thread_num);

    while (cache_ready == 0) {
      sleep(1);
    }
  }

  client_debug(5,"%lu: Done spawning %d client threads", tid, num_threads);

  *info_ret = infoP;

  return CHRONOS_CLIENT_SUCCESS;

failXit:
  return CHRONOS_CLIENT_FAIL;
}

void sigintHandler(int sig)
{
  printf("Received signal: %d\n", sig);
  time_to_die = 1;
}


/* 
 * This is the callback function of a client thread. 
 * It receives the following information:
 *
 *  . thinking time
 *  . IP Address
 *  . Port
 *  . # transactions
 *  . % of View-Stock transactions (The rest are uniformly selected
 *    between the other three types of transactions.)
 *  
 *
 */
static void *
userTransactionThread(void *argP) 
{
  chronosClientThreadInfo_t *infoP = (chronosClientThreadInfo_t *)argP;

  CHRONOS_ENV_H           envH = NULL;
  CHRONOS_CONN_H          connectionH = NULL;
  CHRONOS_CACHE_H         cacheH = NULL;
  CHRONOS_CLIENT_CACHE_H  clientCacheH = NULL;
  chronosUserTransaction_t txnType;
#define CHRONOS_CLIENT_SAMPLING_INTERVAL  10
  int cnt_txns = 0;
  int cnt_view_stock = 0;
  int cnt_view_portfolio = 0;
  int cnt_view_purchase = 0;
  int cnt_view_sale = 0;
  int cnt_success = 0;
  int cnt_fail = 0;
  int txn_rc = 0;
  time_t current_time;
  time_t next_sample_time;
  int sample_period = 0;
  pthread_t tid = pthread_self();
  int initial_load = 1;
  int num_loads = 0;
  int num_portfolios = 0;
  int rc = CHRONOS_CLIENT_SUCCESS;
  int own_cache = 1;

  if (infoP == NULL || infoP->contextP == NULL) {
    client_error("Invalid argument");
    goto cleanup;
  }

  client_debug(3,"%lu: This is thread: %d", tid, infoP->thread_num);

  envH = infoP->contextP->chronosEnvH;
  if (envH == NULL) {
    client_error("Null environment handle");
    goto cleanup;
  }

  connectionH = chronosConnHandleAlloc(envH);
  if (connectionH == NULL) {
    client_error("Could not allocate connection handle");
    goto cleanup;
  }

  cacheH = chronosEnvCacheGet(envH);

  if (infoP->contextP->clientCacheH == NULL) {
    clientCacheH = chronosClientCacheAlloc(1 /* numClient */, 
                                           1 /* numClients */,
                                           cacheH);
    infoP->contextP->clientCacheH = clientCacheH;
  }
  else {
    clientCacheH = infoP->contextP->clientCacheH;
    initial_load = 0;
    own_cache = 0;
  }

  num_portfolios = chronosClientCacheNumPortfoliosGet(clientCacheH);

  /* connect to the chronos server */
  rc = chronosClientConnect(infoP->contextP->serverAddress,
                            infoP->contextP->serverPort,
                            NULL,
                            connectionH);
  if (rc != CHRONOS_CLIENT_SUCCESS) {
    client_error("Could not connect to chronos server");
    goto cleanup;
  }

  current_time = time(NULL);
  next_sample_time = current_time + CHRONOS_CLIENT_SAMPLING_INTERVAL;

  /* Determine how many View_Stock transactions we need to execute */
  while(1) {
    CHRONOS_REQUEST_H requestH = NULL;
    
#if 0
    /* connect to the chronos server */
    rc = chronosClientConnect(infoP->contextP->serverAddress,
                              infoP->contextP->serverPort,
                              NULL,
                              connectionH);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Could not connect to chronos server");
      goto cleanup;
    }
#endif

    if (initial_load) {
      client_debug(1,"%lu: Initial population of portfolios: %d/%d", tid, num_loads, num_portfolios);
      requestH = chronosRequestCreateForClient(num_loads, clientCacheH, envH);
    }
    else {

      /* Pick a transaction type */
      if (pickTransactionType(&txnType, infoP) != CHRONOS_CLIENT_SUCCESS) {
        client_error("Failed to pick transaction type");
        goto cleanup;
      }
      if (txnType == CHRONOS_USER_TXN_VIEW_STOCK) {
        cnt_view_stock ++;
      }
      else if (txnType == CHRONOS_USER_TXN_VIEW_PORTFOLIO) {
        cnt_view_portfolio ++;
      }
      else if (txnType == CHRONOS_USER_TXN_PURCHASE) {
        cnt_view_purchase ++;
      }
      else if (txnType == CHRONOS_USER_TXN_SALE) {
        cnt_view_sale ++;
      }

      requestH = chronosRequestCreate(0, txnType, clientCacheH, envH);
    }

    if (requestH == NULL) {
      client_error("Failed to populate request");
      goto cleanup;
    }

    /* Send the request to the server */
    rc = chronosClientSendRequest(requestH, connectionH);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Failed to send transaction request");
      goto cleanup;
    }

    if (initial_load) {
      num_loads ++;

      if (num_loads == num_portfolios) {
        initial_load = 0;
        cache_ready = 1;
        client_debug(1,"%lu: Finished initial population of portfolios", tid);
      }
    }

    cnt_txns ++;
    client_debug(3,"%lu: [thr: %d] txn count: %d", tid, infoP->thread_num, cnt_txns);
    
    rc = chronosClientReceiveResponse(&txn_rc, connectionH, infoP->contextP->timeToDieFp);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Failed to receive transaction response");
      goto cleanup;
    }

    if (txn_rc == 0) {
      cnt_success ++;
    }
    else {
      cnt_fail ++;
    }

    client_debug(3,"%lu: [thr: %d] txn rc: %d", tid, infoP->thread_num, txn_rc);

    rc = chronosRequestFree(requestH);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Failed to release request");
      goto cleanup;
    }

#if 0
    /* disconnect from the chronos server */
    rc = chronosClientDisconnect(connectionH);
    if (rc != CHRONOS_CLIENT_SUCCESS) {
      client_error("Failed to disconnect from server");
    }
#endif

    current_time = time(NULL);
    if (current_time >= next_sample_time) {
      sample_period ++;
      if (initial_load == 0) {
        fprintf(stderr,"STATS: thr: %d\t sample: %d\t count: %d\t success: %d (%.2f%%)\t fail: %d (%.2f%%)"
                       "\t view_stock: %d (%.2f%%)\t view_portfolio: %d (%.2f%%)\t view_purchase: %d (%.2f%%)\t view_sale: %d (%.2f%%)\n"
                       , infoP->thread_num, sample_period, cnt_txns
                       , cnt_success, cnt_txns > 0 ? 100 * (float)cnt_success/cnt_txns : 0
                       , cnt_fail, cnt_txns > 0 ? 100 * (float)cnt_fail/cnt_txns : 0
                       , cnt_view_stock, cnt_txns > 0 ? 100 * (float)cnt_view_stock/cnt_txns : 0
                       , cnt_view_portfolio, cnt_txns > 0 ? 100 * (float)cnt_view_portfolio/cnt_txns : 0
                       , cnt_view_purchase, cnt_txns > 0 ? 100 * (float)cnt_view_purchase/cnt_txns : 0
                       , cnt_view_sale, cnt_txns > 0 ? 100 * (float)cnt_view_sale/cnt_txns : 0); 
      }
      next_sample_time = current_time + CHRONOS_CLIENT_SAMPLING_INTERVAL;
    }

#if 0
    if (!initial_load) {
      client_debug(3,"%lu: [thr: %d] think time....", tid, infoP->thread_num);
      /* Wait some time before issuing next request */
      if (waitThinkTime(infoP->contextP->minThinkingTime,
                        infoP->contextP->maxThinkingTime) != CHRONOS_CLIENT_SUCCESS) {
        client_error("Error while waiting");
        goto cleanup;
      }
    }
#endif

    if (time_to_die == 1) {
      client_info("%lu: Requested termination", tid);
      break;
    }
  }

cleanup:
  /* disconnect from the chronos server */
  rc = chronosClientDisconnect(connectionH);
  if (rc != CHRONOS_CLIENT_SUCCESS) {
    client_error("Failed to disconnect from server");
  }

  rc = chronosConnHandleFree(connectionH);
  if (rc != CHRONOS_CLIENT_SUCCESS) {
    client_error("Failed to free connection handle");
  }

  client_info("%lu: userTransactionThread exiting", tid);
  pthread_exit(NULL);
}

/*
 * Selects a transaction type with a given probability
 */
static int
pickTransactionType(chronosUserTransaction_t *txn_type_ret, chronosClientThreadInfo_t *infoP) 
{
  int random_num;
  int percentage;

  if (infoP == NULL || infoP->contextP == NULL || txn_type_ret == NULL) {
    client_error("Invalid argument");
    goto failXit;
  }

  percentage = infoP->contextP->percentageViewStockTransactions;

  random_num = rand() % 100;

  if (random_num < percentage) {
    *txn_type_ret = CHRONOS_USER_TXN_VIEW_STOCK;
  }
  else {
    random_num = rand() % (CHRONOS_USER_TXN_MAX - 1);
    *txn_type_ret = CHRONOS_USER_TXN_INVAL;
    switch (random_num) {
      case 0:
        *txn_type_ret = CHRONOS_USER_TXN_VIEW_PORTFOLIO;
        break;

      case 1:
        *txn_type_ret = CHRONOS_USER_TXN_VIEW_PORTFOLIO;
        //*txn_type_ret = CHRONOS_USER_TXN_PURCHASE;
        break;

      case 2:
        *txn_type_ret = CHRONOS_USER_TXN_VIEW_PORTFOLIO;
        //*txn_type_ret = CHRONOS_USER_TXN_SALE;
        break;

      default:
        client_error("Transaction type: %d --> %d %s", random_num, *txn_type_ret, CHRONOS_TXN_NAME(*txn_type_ret));
        assert("Invalid transaction type" == 0);
        break;
    }
  }

#if 0
  *txn_type_ret = CHRONOS_USER_TXN_SALE;
#endif

  client_debug(3,"Selected transaction type is: %s", CHRONOS_TXN_NAME(*txn_type_ret));

  return CHRONOS_CLIENT_SUCCESS;

failXit:
  return CHRONOS_CLIENT_FAIL;
}

/* This is the starting point of a Chronos Client.
 *
 * A client process spawns a number of threads as per
 * the user request. 
 *
 * According to the original paper, "a client thread first
 * needs to send the server a TCP connection request. It 
 * suspends until the server accepts the connection request
 * and allocates a server thread. When the connection is 
 * established, the client thread sends a transaction or 
 * query processing request and suspends until the 
 * corresponding server thread finishes processing the transaction
 * and returns the result. After receiving the result, the
 * client thread waits for a think time unit uniformly selected before
 * issuing another request."
 */

int main(int argc, char *argv[]) 
{
  chronosClientContext_t client_context;
  chronosClientThreadInfo_t *thread_infoP = NULL;
  chronos_debug_level = CHRONOS_DEBUG_LEVEL_MIN;

  srand(time(NULL));

  memset(&client_context, 0, sizeof(client_context));

  if (processArguments(argc, argv, &client_context) != CHRONOS_CLIENT_SUCCESS) {
    client_error("Failed to process command line arguments");
    goto failXit;
  }

  /* set the signal hanlder for sigint */
  if (signal(SIGINT, sigintHandler) == SIG_ERR) {
    client_error("Failed to set signal handler");
    goto failXit;    
  }
  
  client_context.chronosEnvH = chronosEnvAlloc(CHRONOS_CLIENT_HOME_DIR, CHRONOS_CLIENT_DATAFILES_DIR);
  if (client_context.chronosEnvH == NULL) {
    client_error("Failed to allocate chronos environment handle");
    goto failXit;
  }

  /* Next we need to spawn the client threads */
  if (spawnClientThreads(client_context.numClientsThreads, &thread_infoP, &client_context) != CHRONOS_CLIENT_SUCCESS) {
    client_error("Failed spawning threads");
    goto failXit;
  }

  if (waitClientThreads(client_context.numClientsThreads, thread_infoP, &client_context) != CHRONOS_CLIENT_SUCCESS) {
    client_error("Failed while waiting for threads termination");
    goto failXit;
  }

  return CHRONOS_CLIENT_SUCCESS;

failXit:
  if (client_context.chronosEnvH != NULL) {
    chronosEnvFree(client_context.chronosEnvH);
    client_context.chronosEnvH = NULL;
  }

  return CHRONOS_CLIENT_FAIL;
}

