/*
 * client_config.h
 *
 *  Created on: Jan 21, 2018
 *      Author: ricardo
 */

#ifndef CLIENT_CONFIG_H_
#define CLIENT_CONFIG_H_

#define CHRONOS_DEBUG
#define CHRONOS_ALL_TXN_AVAILABLE

/* These are the directories where the databases and the datafiles live.
 * Before starting up the server, the datafiles should be moved to the
 * specified directory. The datafiles are used to populate the Chronos
 * tables */
#define CHRONOS_CLIENT_HOME_DIR       "/tmp/chronos/databases"
#define CHRONOS_CLIENT_DATAFILES_DIR  "/tmp/chronos/datafiles"


/* By default the Chronos server runs in port 5000 */
#define CHRONOS_SERVER_ADDRESS  "127.0.0.1"
#define CHRONOS_SERVER_PORT     5000



/* In the Chronos paper, the number of client threads start
 * at 900 and it can increase up to 1800
 */
#ifdef CHRONOS_DEBUG
#define CHRONOS_NUM_CLIENT_THREADS    1
#else
#define CHRONOS_NUM_CLIENT_THREADS    900
#endif


#define CHRONOS_MIN_THINK_TIME_MS          (30)
#define CHRONOS_MAX_THINK_TIME_MS          (1000)


#define CHRONOS_RATE_VIEW_TRANSACTIONS  (60)

#define CHRONOS_DEBUG_LEVEL_MIN         (0)
#define CHRONOS_DEBUG_LEVEL_MAX         (10)

#endif /* CLIENT_CONFIG_H_ */
